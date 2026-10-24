#pragma once

#include "common/params.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace cfx {

struct CaptionLine {
  int id = 0;
  std::string text;
};

struct CaptionView {
  std::vector<CaptionLine> lines;
  std::string draft;
  std::string status;
};

class CaptionEngine {
 public:
  explicit CaptionEngine(Params& params);
  ~CaptionEngine();

  CaptionEngine(const CaptionEngine&) = delete;
  CaptionEngine& operator=(const CaptionEngine&) = delete;

  void start();
  void stop();
  void setSource(std::uint32_t pid, bool exclude);
  void setRefine(bool enabled);
  void clear();
  CaptionView copyView() const;

 private:
  void listenMain();
  void refineMain();

  Params& params_;
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace cfx
