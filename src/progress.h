#pragma once

#include <chrono>
#include <cstddef>
#include <string>

namespace pgv {

class ProgressBar {
 public:
  ProgressBar(std::string label, size_t total, bool enabled);
  ~ProgressBar();

  void update(size_t current);
  void increment(size_t amount = 1);
  void finish();

 private:
  void render(bool force);

  std::string label_;
  size_t total_ = 0;
  size_t current_ = 0;
  bool enabled_ = false;
  bool finished_ = false;
  int last_percent_ = -1;
  std::chrono::steady_clock::time_point started_;
  std::chrono::steady_clock::time_point last_render_;
};

class PhaseProgress {
 public:
  PhaseProgress(std::string label, size_t total, bool enabled);
  ~PhaseProgress();

  void step(const std::string& message);
  void finish();

 private:
  std::string label_;
  size_t total_ = 0;
  size_t current_ = 0;
  bool enabled_ = false;
  bool finished_ = false;
  std::chrono::steady_clock::time_point started_;
};

}  // namespace pgv
