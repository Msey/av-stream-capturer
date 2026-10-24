#include "captions/recognizers.hpp"

#include <cstring>
#include <utility>

#include "sherpa-onnx/c-api/c-api.h"

namespace cfx {

struct StreamingRecognizer::Impl {
  std::string model;
  std::string tokens;
  const SherpaOnnxOnlineRecognizer* recognizer = nullptr;
  const SherpaOnnxOnlineStream* stream = nullptr;
};

StreamingRecognizer::StreamingRecognizer(std::string model, std::string tokens) : impl_(new Impl) {
  impl_->model = std::move(model);
  impl_->tokens = std::move(tokens);
  SherpaOnnxOnlineRecognizerConfig config;
  std::memset(&config, 0, sizeof(config));
  config.model_config.t_one_ctc.model = impl_->model.c_str();
  config.model_config.tokens = impl_->tokens.c_str();
  config.model_config.provider = "cpu";
  config.model_config.num_threads = 1;
  config.decoding_method = "greedy_search";
  config.enable_endpoint = 1;
  config.rule1_min_trailing_silence = 1.2f;
  config.rule2_min_trailing_silence = 0.6f;
  config.rule3_min_utterance_length = 20.f;
  impl_->recognizer = SherpaOnnxCreateOnlineRecognizer(&config);
  if (impl_->recognizer) impl_->stream = SherpaOnnxCreateOnlineStream(impl_->recognizer);
}

StreamingRecognizer::~StreamingRecognizer() {
  if (impl_->stream) SherpaOnnxDestroyOnlineStream(impl_->stream);
  if (impl_->recognizer) SherpaOnnxDestroyOnlineRecognizer(impl_->recognizer);
  delete impl_;
}

bool StreamingRecognizer::ok() const { return impl_->recognizer && impl_->stream; }

void StreamingRecognizer::accept(int sampleRate, const float* samples, int count) {
  if (!ok() || !samples || count <= 0) return;
  SherpaOnnxOnlineStreamAcceptWaveform(impl_->stream, sampleRate, samples, count);
}

void StreamingRecognizer::decode() {
  if (!ok()) return;
  while (SherpaOnnxIsOnlineStreamReady(impl_->recognizer, impl_->stream)) {
    SherpaOnnxDecodeOnlineStream(impl_->recognizer, impl_->stream);
  }
}

std::string StreamingRecognizer::text() const {
  if (!ok()) return {};
  const SherpaOnnxOnlineRecognizerResult* result = SherpaOnnxGetOnlineStreamResult(impl_->recognizer, impl_->stream);
  std::string text = result && result->text ? result->text : "";
  if (result) SherpaOnnxDestroyOnlineRecognizerResult(result);
  return text;
}

bool StreamingRecognizer::isEndpoint() const {
  if (!ok()) return false;
  return SherpaOnnxOnlineStreamIsEndpoint(impl_->recognizer, impl_->stream) != 0;
}

void StreamingRecognizer::reset() {
  if (ok()) SherpaOnnxOnlineStreamReset(impl_->recognizer, impl_->stream);
}

void StreamingRecognizer::restart() {
  if (impl_->stream) SherpaOnnxDestroyOnlineStream(impl_->stream);
  impl_->stream = impl_->recognizer ? SherpaOnnxCreateOnlineStream(impl_->recognizer) : nullptr;
}

struct PhraseRefiner::Impl {
  std::string model;
  std::string tokens;
  const SherpaOnnxOfflineRecognizer* recognizer = nullptr;
};

PhraseRefiner::PhraseRefiner(std::string model, std::string tokens) : impl_(new Impl) {
  impl_->model = std::move(model);
  impl_->tokens = std::move(tokens);
  SherpaOnnxOfflineRecognizerConfig config;
  std::memset(&config, 0, sizeof(config));
  config.feat_config.sample_rate = 16000;
  config.feat_config.feature_dim = 64;
  config.model_config.nemo_ctc.model = impl_->model.c_str();
  config.model_config.tokens = impl_->tokens.c_str();
  config.model_config.provider = "cpu";
  config.model_config.num_threads = 2;
  config.decoding_method = "greedy_search";
  impl_->recognizer = SherpaOnnxCreateOfflineRecognizer(&config);
}

PhraseRefiner::~PhraseRefiner() {
  if (impl_->recognizer) SherpaOnnxDestroyOfflineRecognizer(impl_->recognizer);
  delete impl_;
}

bool PhraseRefiner::ok() const { return impl_->recognizer != nullptr; }

std::string PhraseRefiner::refine(int sampleRate, const float* samples, int count) {
  if (!ok() || !samples || count <= 0 || sampleRate <= 0) return {};
  const SherpaOnnxOfflineStream* stream = SherpaOnnxCreateOfflineStream(impl_->recognizer);
  if (!stream) return {};
  SherpaOnnxAcceptWaveformOffline(stream, sampleRate, samples, count);
  SherpaOnnxDecodeOfflineStream(impl_->recognizer, stream);
  const SherpaOnnxOfflineRecognizerResult* result = SherpaOnnxGetOfflineStreamResult(stream);
  std::string text = result && result->text ? result->text : "";
  if (result) SherpaOnnxDestroyOfflineRecognizerResult(result);
  SherpaOnnxDestroyOfflineStream(stream);
  return text;
}

struct VoiceSpeaker::Impl {
  VoicePaths voice;
  const SherpaOnnxOfflineTts* tts = nullptr;
};

VoiceSpeaker::VoiceSpeaker(VoicePaths voice) : impl_(new Impl) {
  impl_->voice = std::move(voice);
  if (!impl_->voice.model.empty() && !impl_->voice.tokens.empty() && !impl_->voice.dataDir.empty()) {
    SherpaOnnxOfflineTtsConfig config;
    std::memset(&config, 0, sizeof(config));
    config.model.vits.model = impl_->voice.model.c_str();
    config.model.vits.tokens = impl_->voice.tokens.c_str();
    config.model.vits.data_dir = impl_->voice.dataDir.c_str();
    config.model.vits.noise_scale = 0.667f;
    config.model.vits.noise_scale_w = 0.8f;
    config.model.vits.length_scale = 1.f;
    config.model.num_threads = 2;
    config.model.provider = "cpu";
    config.max_num_sentences = 1;
    impl_->tts = SherpaOnnxCreateOfflineTts(&config);
  }
}

VoiceSpeaker::~VoiceSpeaker() {
  if (impl_->tts) SherpaOnnxDestroyOfflineTts(impl_->tts);
  delete impl_;
}

bool VoiceSpeaker::ok() const { return impl_->tts != nullptr; }

bool VoiceSpeaker::ownVoice() const { return impl_->voice.own; }

std::vector<float> VoiceSpeaker::synthesize(const std::string& text, int& sampleRate) const {
  sampleRate = 0;
  if (!ok() || text.empty()) return {};
  SherpaOnnxGenerationConfig generation;
  std::memset(&generation, 0, sizeof(generation));
  generation.sid = 0;
  generation.speed = 1.f;
  generation.silence_scale = 0.2f;
  const SherpaOnnxGeneratedAudio* audio =
      SherpaOnnxOfflineTtsGenerateWithConfig(impl_->tts, text.c_str(), &generation, nullptr, nullptr);
  if (!audio || !audio->samples || audio->n <= 0) {
    if (audio) SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
    return {};
  }
  sampleRate = audio->sample_rate;
  std::vector<float> samples(audio->samples, audio->samples + audio->n);
  SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
  return samples;
}

}  // namespace cfx
