#include "captions/caption_engine.hpp"

#include "captions/model_files.hpp"
#include "captions/recognizers.hpp"
#include "capture/app_loopback.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

namespace cfx {
namespace {

double nowMs() {
  static const double freq = [] {
    LARGE_INTEGER value{};
    QueryPerformanceFrequency(&value);
    return static_cast<double>(value.QuadPart) / 1000.0;
  }();
  LARGE_INTEGER value{};
  QueryPerformanceCounter(&value);
  return static_cast<double>(value.QuadPart) / freq;
}

}  // namespace

struct CaptionEngine::Impl {
  std::thread listen;
  std::thread refine;
  std::atomic<int> run{0};
  std::atomic<int> restart{0};
  std::atomic<int> clearRequest{0};
  std::atomic<std::uint32_t> pid{0};
  std::atomic<int> exclude{1};
  std::atomic<int> refineEnabled{1};

  std::mutex viewMu;
  CaptionView view;
  int nextId = 1;

  std::mutex jobMu;
  std::condition_variable jobCv;
  struct Job {
    int id = 0;
    int rate = 16000;
    std::vector<float> samples;
  };
  std::vector<Job> jobs;
};

CaptionEngine::CaptionEngine(Params& params) : params_(params), impl_(new Impl) {
  impl_->view.status = "Субтитры выключены";
}

CaptionEngine::~CaptionEngine() {
  stop();
  delete impl_;
}

void CaptionEngine::start() {
  if (impl_->run.load()) return;
  if (impl_->listen.joinable()) impl_->listen.join();
  if (impl_->refine.joinable()) impl_->refine.join();
  impl_->run.store(1);
  impl_->listen = std::thread([this] { listenMain(); });
  impl_->refine = std::thread([this] { refineMain(); });
}

void CaptionEngine::stop() {
  impl_->run.store(0);
  impl_->jobCv.notify_all();
  if (impl_->listen.joinable()) impl_->listen.join();
  if (impl_->refine.joinable()) impl_->refine.join();
  std::lock_guard lock(impl_->viewMu);
  if (impl_->view.status.empty() || impl_->view.status == "Слушаю собеседника") impl_->view.status = "Субтитры выключены";
}

void CaptionEngine::setSource(std::uint32_t pid, bool exclude) {
  impl_->pid.store(pid);
  impl_->exclude.store(exclude ? 1 : 0);
  impl_->restart.store(1);
}

void CaptionEngine::setRefine(bool enabled) { impl_->refineEnabled.store(enabled ? 1 : 0); }

void CaptionEngine::clear() {
  {
    std::lock_guard lock(impl_->viewMu);
    impl_->view.lines.clear();
    impl_->view.draft.clear();
  }
  impl_->clearRequest.fetch_add(1);
}

CaptionView CaptionEngine::copyView() const {
  std::lock_guard lock(impl_->viewMu);
  return impl_->view;
}

void CaptionEngine::listenMain() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto setStatus = [this](const std::string& text) {
    std::lock_guard lock(impl_->viewMu);
    impl_->view.status = text;
  };

  AsrPaths models;
  while (impl_->run.load()) {
    models = findAsrModels(exeDirectory());
    if (!models.toneModel.empty() && !models.toneTokens.empty()) break;
    setStatus("Нет модели распознавания рядом с программой");
    Sleep(500);
  }
  if (!impl_->run.load()) {
    if (SUCCEEDED(com)) CoUninitialize();
    return;
  }

  setStatus("Загружаю модель речи…");
  StreamingRecognizer tone(models.toneModel, models.toneTokens);
  if (!tone.ok()) {
    setStatus("Модель речи не открылась");
    while (impl_->run.load()) Sleep(200);
    if (SUCCEEDED(com)) CoUninitialize();
    return;
  }

  const bool punctuation = !models.gigaModel.empty() && !models.gigaTokens.empty();

  AppLoopback loopback;
  std::vector<float> phrase;
  std::vector<float> packet(48000);
  int rate = 16000;
  int seenClear = impl_->clearRequest.load();
  bool primed = false;
  ULONGLONG lastAudio = GetTickCount64();

  auto publishDraft = [this](const std::string& text) {
    std::lock_guard lock(impl_->viewMu);
    impl_->view.draft = text;
  };
  auto finishPhrase = [&](bool force) {
    if (!force && !tone.isEndpoint()) return;
    const std::string text = tone.text();
    tone.reset();
    publishDraft({});
    if (text.empty()) {
      phrase.clear();
      return;
    }
    Impl::Job job;
    job.id = impl_->nextId++;
    job.rate = rate;
    job.samples.swap(phrase);
    {
      std::lock_guard lock(impl_->viewMu);
      impl_->view.lines.push_back(CaptionLine{job.id, text});
      if (impl_->view.lines.size() > 200) impl_->view.lines.erase(impl_->view.lines.begin());
      impl_->view.draft.clear();
    }
    if (punctuation && impl_->refineEnabled.load() && !job.samples.empty()) {
      std::lock_guard lock(impl_->jobMu);
      impl_->jobs.clear();
      impl_->jobs.push_back(std::move(job));
      impl_->jobCv.notify_one();
    }
  };

  while (impl_->run.load()) {
    if (impl_->clearRequest.load() != seenClear) {
      seenClear = impl_->clearRequest.load();
      phrase.clear();
      tone.reset();
      publishDraft({});
    }

    std::string error;
    const std::uint32_t pid = impl_->pid.load();
    const bool exclude = impl_->exclude.load() != 0;
    if (!loopback.open(pid, exclude, error)) {
      setStatus(error);
      for (int i = 0; i < 10 && impl_->run.load() && !impl_->restart.load(); ++i) Sleep(100);
      impl_->restart.store(0);
      continue;
    }
    rate = loopback.sampleRate();
    tone.restart();
    phrase.clear();
    primed = false;
    if (rate > 0) {
      std::vector<float> pad(static_cast<size_t>(rate * 3 / 10), 0.f);
      tone.accept(rate, pad.data(), static_cast<int>(pad.size()));
      tone.decode();
    }
    primed = true;
    impl_->restart.store(0);
    setStatus(punctuation ? "Слушаю собеседника" : "Слушаю собеседника, без пунктуации");
    lastAudio = GetTickCount64();
    (void)primed;

    while (impl_->run.load() && !impl_->restart.load()) {
      if (impl_->clearRequest.load() != seenClear) {
        seenClear = impl_->clearRequest.load();
        phrase.clear();
        tone.reset();
        publishDraft({});
      }
      if (static_cast<int>(packet.size()) < rate) packet.resize(static_cast<size_t>(rate));
      const int got = loopback.read(packet.data(), static_cast<int>(packet.size()), 100);
      if (got < 0) break;
      const ULONGLONG now = GetTickCount64();
      if (got == 0) {
        if (now - lastAudio >= 100 && rate > 0) {
          const int silence = std::max(1, rate / 10);
          std::vector<float> zeros(static_cast<size_t>(silence), 0.f);
          const double started = nowMs();
          tone.accept(rate, zeros.data(), silence);
          tone.decode();
          params_.captionChunkMs.store(static_cast<float>(nowMs() - started));
          phrase.insert(phrase.end(), zeros.begin(), zeros.end());
          lastAudio = now;
          const std::string partial = tone.text();
          if (!partial.empty()) publishDraft(partial);
          finishPhrase(false);
        }
        continue;
      }
      lastAudio = now;
      const double started = nowMs();
      tone.accept(rate, packet.data(), got);
      tone.decode();
      params_.captionChunkMs.store(static_cast<float>(nowMs() - started));
      phrase.insert(phrase.end(), packet.begin(), packet.begin() + got);
      const std::string partial = tone.text();
      if (!partial.empty()) publishDraft(partial);
      const bool tooLong = rate > 0 && static_cast<int>(phrase.size()) > rate * 20;
      finishPhrase(tooLong);
    }
    if (!phrase.empty()) finishPhrase(true);
    loopback.close();
  }

  if (SUCCEEDED(com)) CoUninitialize();
}

void CaptionEngine::refineMain() {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  AsrPaths models;
  while (impl_->run.load() && (models.gigaModel.empty() || models.gigaTokens.empty())) {
    models = findAsrModels(exeDirectory());
    if (!models.gigaModel.empty() && !models.gigaTokens.empty()) break;
    Sleep(200);
  }
  if (!impl_->run.load()) {
    if (SUCCEEDED(com)) CoUninitialize();
    return;
  }
  PhraseRefiner refiner(models.gigaModel, models.gigaTokens);
  while (impl_->run.load()) {
    Impl::Job job;
    {
      std::unique_lock lock(impl_->jobMu);
      impl_->jobCv.wait(lock, [this] { return !impl_->run.load() || !impl_->jobs.empty(); });
      if (!impl_->run.load() && impl_->jobs.empty()) break;
      if (impl_->jobs.empty()) continue;
      job = std::move(impl_->jobs.back());
      impl_->jobs.clear();
    }
    if (!refiner.ok() || job.samples.empty()) continue;
    const double started = nowMs();
    const std::string better = refiner.refine(job.rate, job.samples.data(), static_cast<int>(job.samples.size()));
    params_.captionRefineMs.store(static_cast<float>(nowMs() - started));
    if (better.empty()) continue;
    std::lock_guard lock(impl_->viewMu);
    for (CaptionLine& line : impl_->view.lines) {
      if (line.id == job.id) {
        line.text = better;
        break;
      }
    }
  }
  if (SUCCEEDED(com)) CoUninitialize();
}

}  // namespace cfx
