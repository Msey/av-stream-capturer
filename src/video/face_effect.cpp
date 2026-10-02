#include "video/face_effect.hpp"

#include "video/pupils.hpp"

#include <onnxruntime_cxx_api.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <vector>

namespace cfx {
namespace {

constexpr int kDefaultNetW = 320;
constexpr int kDefaultNetH = 240;

float iou(const FaceBox& a, const FaceBox& b) {
  const float x1 = std::max(a.x1, b.x1);
  const float y1 = std::max(a.y1, b.y1);
  const float x2 = std::min(a.x2, b.x2);
  const float y2 = std::min(a.y2, b.y2);
  const float iw = std::max(0.f, x2 - x1);
  const float ih = std::max(0.f, y2 - y1);
  const float inter = iw * ih;
  const float areaA = std::max(0.f, a.x2 - a.x1) * std::max(0.f, a.y2 - a.y1);
  const float areaB = std::max(0.f, b.x2 - b.x1) * std::max(0.f, b.y2 - b.y1);
  return inter / (areaA + areaB - inter + 1e-6f);
}

void sampleBilinear(const uint8_t* bgra, int width, int height, int stride, float x, float y, float& r, float& g,
                    float& b) {
  x = std::clamp(x, 0.f, static_cast<float>(width - 1));
  y = std::clamp(y, 0.f, static_cast<float>(height - 1));
  const int x0 = static_cast<int>(x);
  const int y0 = static_cast<int>(y);
  const int x1 = std::min(x0 + 1, width - 1);
  const int y1 = std::min(y0 + 1, height - 1);
  const float tx = x - x0;
  const float ty = y - y0;
  const uint8_t* p00 = bgra + static_cast<size_t>(y0) * stride + x0 * 4;
  const uint8_t* p10 = bgra + static_cast<size_t>(y0) * stride + x1 * 4;
  const uint8_t* p01 = bgra + static_cast<size_t>(y1) * stride + x0 * 4;
  const uint8_t* p11 = bgra + static_cast<size_t>(y1) * stride + x1 * 4;
  auto chan = [&](int c) {
    const float top = p00[c] + (p10[c] - p00[c]) * tx;
    const float bot = p01[c] + (p11[c] - p01[c]) * tx;
    return top + (bot - top) * ty;
  };
  b = chan(0);
  g = chan(1);
  r = chan(2);
}

}  // namespace

struct FaceEffect::Impl {
  Ort::Env env{ORT_LOGGING_LEVEL_ERROR, "conference-fx"};
  Ort::SessionOptions options;
  Ort::Session session{nullptr};
  Ort::MemoryInfo memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
  std::string inputName;
  std::string scoreName;
  std::string boxName;
  int netW = kDefaultNetW;
  int netH = kDefaultNetH;
  std::vector<float> input;
  FaceBox box;
  int frame = 0;
  float lastMs = 0.f;
  OneEuro leftX, leftY, leftR, rightX, rightY, rightR;

  explicit Impl(const std::wstring& path) {
    options.SetLogSeverityLevel(ORT_LOGGING_LEVEL_ERROR);
    options.SetIntraOpNumThreads(2);
    options.SetInterOpNumThreads(1);
    options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    session = Ort::Session(env, path.c_str(), options);
    Ort::AllocatorWithDefaultOptions allocator;
    inputName = session.GetInputNameAllocated(0, allocator).get();
    const auto inShape = session.GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
    if (inShape.size() == 4) {
      if (inShape[2] > 0) netH = static_cast<int>(inShape[2]);
      if (inShape[3] > 0) netW = static_cast<int>(inShape[3]);
    }
    input.assign(static_cast<size_t>(3 * netH * netW), 0.f);

    const size_t outputs = session.GetOutputCount();
    std::vector<std::string> names;
    std::vector<std::vector<int64_t>> shapes;
    names.reserve(outputs);
    for (size_t i = 0; i < outputs; ++i) {
      names.push_back(session.GetOutputNameAllocated(i, allocator).get());
      shapes.push_back(session.GetOutputTypeInfo(i).GetTensorTypeAndShapeInfo().GetShape());
    }
    for (size_t i = 0; i < outputs; ++i) {
      const int last = shapes[i].empty() ? 0 : static_cast<int>(shapes[i].back());
      if (last == 2 && scoreName.empty()) scoreName = names[i];
      else if (last == 4 && boxName.empty()) boxName = names[i];
    }
    if (scoreName.empty() && !names.empty()) scoreName = names[0];
    if (boxName.empty() && names.size() > 1) boxName = names[1];
  }

  void preprocess(const uint8_t* bgra, int width, int height, int stride) {
    float* rPlane = input.data();
    float* gPlane = rPlane + static_cast<size_t>(netH) * netW;
    float* bPlane = gPlane + static_cast<size_t>(netH) * netW;
    for (int y = 0; y < netH; ++y) {
      const float sy = (static_cast<float>(y) + 0.5f) * height / netH - 0.5f;
      for (int x = 0; x < netW; ++x) {
        const float sx = (static_cast<float>(x) + 0.5f) * width / netW - 0.5f;
        float r, g, b;
        sampleBilinear(bgra, width, height, stride, sx, sy, r, g, b);
        const size_t i = static_cast<size_t>(y) * netW + x;
        rPlane[i] = (r - 127.f) / 128.f;
        gPlane[i] = (g - 127.f) / 128.f;
        bPlane[i] = (b - 127.f) / 128.f;
      }
    }
  }

  FaceBox detect(const uint8_t* bgra, int width, int height, int stride) {
    preprocess(bgra, width, height, stride);
    const std::array<int64_t, 4> shape{1, 3, netH, netW};
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memory, input.data(), input.size(), shape.data(), shape.size());
    const char* inNames[] = {inputName.c_str()};
    const char* outNames[] = {scoreName.c_str(), boxName.c_str()};
    auto outputs = session.Run(Ort::RunOptions{nullptr}, inNames, &tensor, 1, outNames, 2);

    int scoreIndex = 0;
    int boxIndex = 1;
    auto rankLast = [&](int index) {
      const auto shapeInfo = outputs[index].GetTensorTypeAndShapeInfo().GetShape();
      return shapeInfo.empty() ? 0 : static_cast<int>(shapeInfo.back());
    };
    if (rankLast(0) == 4 && rankLast(1) == 2) std::swap(scoreIndex, boxIndex);

    const auto scoreShape = outputs[scoreIndex].GetTensorTypeAndShapeInfo().GetShape();
    const auto boxShape = outputs[boxIndex].GetTensorTypeAndShapeInfo().GetShape();
    const float* scores = outputs[scoreIndex].GetTensorData<float>();
    const float* boxes = outputs[boxIndex].GetTensorData<float>();
    int count = 0;
    int scoreStride = 2;
    if (scoreShape.size() >= 2) count = static_cast<int>(scoreShape[scoreShape.size() - 2]);
    if (!scoreShape.empty() && scoreShape.back() == 2) scoreStride = 2;
    else if (scoreShape.size() == 3 && scoreShape[1] == 2) {
      // NCHW-style scores are uncommon for this model; keep the interleaved path.
      count = static_cast<int>(scoreShape.back());
      scoreStride = 1;
    }

    std::vector<FaceBox> candidates;
    candidates.reserve(32);
    for (int i = 0; i < count; ++i) {
      float score = 0.f;
      if (scoreShape.size() == 3 && scoreShape[1] == 2 && scoreShape.back() != 2) {
        score = scores[count + i];
      } else {
        score = scores[i * scoreStride + (scoreStride > 1 ? 1 : 0)];
      }
      if (score < 0.6f) continue;
      FaceBox box;
      box.score = score;
      const float* b = boxes + static_cast<size_t>(i) * 4;
      const bool normalized = std::fabs(b[2]) <= 2.f && std::fabs(b[3]) <= 2.f;
      if (normalized) {
        box.x1 = b[0] * width;
        box.y1 = b[1] * height;
        box.x2 = b[2] * width;
        box.y2 = b[3] * height;
      } else {
        box.x1 = b[0] * width / static_cast<float>(netW);
        box.y1 = b[1] * height / static_cast<float>(netH);
        box.x2 = b[2] * width / static_cast<float>(netW);
        box.y2 = b[3] * height / static_cast<float>(netH);
      }
      if (box.x2 < box.x1) std::swap(box.x1, box.x2);
      if (box.y2 < box.y1) std::swap(box.y1, box.y2);
      box.x1 = std::clamp(box.x1, 0.f, static_cast<float>(width));
      box.x2 = std::clamp(box.x2, 0.f, static_cast<float>(width));
      box.y1 = std::clamp(box.y1, 0.f, static_cast<float>(height));
      box.y2 = std::clamp(box.y2, 0.f, static_cast<float>(height));
      if (box.x2 - box.x1 < 8.f || box.y2 - box.y1 < 8.f) continue;
      candidates.push_back(box);
      if (candidates.size() == 64) break;
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const FaceBox& a, const FaceBox& b) { return a.score > b.score; });
    std::vector<FaceBox> kept;
    for (const FaceBox& candidate : candidates) {
      bool drop = false;
      for (const FaceBox& prev : kept) {
        if (iou(candidate, prev) > 0.3f) {
          drop = true;
          break;
        }
      }
      if (!drop) kept.push_back(candidate);
      if (kept.size() == 4) break;
    }
    FaceBox best;
    float bestArea = 0.f;
    for (const FaceBox& face : kept) {
      const float area = (face.x2 - face.x1) * (face.y2 - face.y1);
      if (area > bestArea) {
        bestArea = area;
        best = face;
        best.found = true;
      }
    }
    return best;
  }
};

FaceEffect::FaceEffect() = default;

FaceEffect::~FaceEffect() { delete impl_; }

bool FaceEffect::load(const std::wstring& modelPath) {
  delete impl_;
  impl_ = nullptr;
  ready_ = false;
  try {
    impl_ = new Impl(modelPath);
    ready_ = true;
    status_ = "UltraFace готов";
    return true;
  } catch (const Ort::Exception& error) {
    status_ = error.what();
    return false;
  } catch (const std::exception& error) {
    status_ = error.what();
    return false;
  }
}

bool FaceEffect::apply(uint8_t* bgra, int width, int height, int stride, float dtSeconds, float& processMs) {
  processMs = 0.f;
  if (!impl_ || !bgra) return false;
  const auto start = std::chrono::steady_clock::now();
  const bool reuse = impl_->box.found && impl_->lastMs > 18.f && (impl_->frame % 2) == 1;
  if (!reuse) impl_->box = impl_->detect(bgra, width, height, stride);
  ++impl_->frame;

  bool found = impl_->box.found;
  if (found) {
    const float fw = impl_->box.x2 - impl_->box.x1;
    const float fh = impl_->box.y2 - impl_->box.y1;
    struct EyeRoi {
      float x, y, w, h;
      OneEuro& fx;
      OneEuro& fy;
      OneEuro& fr;
    };
    EyeRoi eyes[2] = {
        {impl_->box.x1 + fw * 0.16f, impl_->box.y1 + fh * 0.28f, fw * 0.30f, fh * 0.24f, impl_->leftX, impl_->leftY,
         impl_->leftR},
        {impl_->box.x1 + fw * 0.54f, impl_->box.y1 + fh * 0.28f, fw * 0.30f, fh * 0.24f, impl_->rightX, impl_->rightY,
         impl_->rightR},
    };
    for (EyeRoi& eye : eyes) {
      Pupil pupil = locatePupil(bgra, stride, width, height, eye.x, eye.y, eye.w, eye.h);
      if (!pupil.ok) {
        pupil.x = eye.x + eye.w * 0.5f;
        pupil.y = eye.y + eye.h * 0.42f;
        pupil.r = eye.w * 0.16f;
      }
      const float x = eye.fx.filter(pupil.x, dtSeconds);
      const float y = eye.fy.filter(pupil.y, dtSeconds);
      const float r = eye.fr.filter(pupil.r, dtSeconds);
      paintPupil(bgra, stride, width, height, x, y, r);
    }
  } else {
    impl_->leftX.reset();
    impl_->leftY.reset();
    impl_->leftR.reset();
    impl_->rightX.reset();
    impl_->rightY.reset();
    impl_->rightR.reset();
  }

  const auto elapsed = std::chrono::steady_clock::now() - start;
  processMs = std::chrono::duration<float, std::milli>(elapsed).count();
  impl_->lastMs = processMs;
  return found;
}

}  // namespace cfx
