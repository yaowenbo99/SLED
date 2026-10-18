#include "progress.h"

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <utility>

namespace pgv {

namespace {

double elapsed_seconds(std::chrono::steady_clock::time_point started) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

std::string format_elapsed(std::chrono::steady_clock::time_point started) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(1) << elapsed_seconds(started) << "s";
  return out.str();
}

}  // namespace

ProgressBar::ProgressBar(std::string label, size_t total, bool enabled)
    : label_(std::move(label)),
      total_(total),
      enabled_(enabled),
      started_(std::chrono::steady_clock::now()),
      last_render_(started_) {
  render(true);
}

ProgressBar::~ProgressBar() {
  finish();
}

void ProgressBar::update(size_t current) {
  current_ = std::min(current, total_);
  render(false);
}

void ProgressBar::increment(size_t amount) {
  update(current_ + amount);
}

void ProgressBar::finish() {
  if (!enabled_ || finished_) {
    return;
  }
  bool already_complete = current_ >= total_ && last_percent_ >= 100;
  current_ = total_;
  if (!already_complete) {
    render(true);
  }
  std::cerr << '\n';
  finished_ = true;
}

void ProgressBar::render(bool force) {
  if (!enabled_) {
    return;
  }
  auto now = std::chrono::steady_clock::now();
  double ratio = total_ == 0 ? 1.0 : static_cast<double>(current_) / static_cast<double>(total_);
  int percent = static_cast<int>(ratio * 100.0);
  bool enough_time = std::chrono::duration<double>(now - last_render_).count() >= 0.25;
  bool enough_progress = last_percent_ < 0 || percent != last_percent_;
  if (!force && !enough_time && !enough_progress) {
    return;
  }

  constexpr int width = 30;
  int filled = static_cast<int>(ratio * width);
  filled = std::clamp(filled, 0, width);
  std::string bar(static_cast<size_t>(filled), '#');
  bar.append(static_cast<size_t>(width - filled), '-');

  std::cerr << '\r' << label_ << " [" << bar << "] " << std::fixed << std::setprecision(1)
            << (ratio * 100.0) << "% " << current_ << "/" << total_
            << " elapsed=" << format_elapsed(started_) << std::flush;
  last_percent_ = percent;
  last_render_ = now;
}

PhaseProgress::PhaseProgress(std::string label, size_t total, bool enabled)
    : label_(std::move(label)),
      total_(total),
      enabled_(enabled),
      started_(std::chrono::steady_clock::now()) {}

PhaseProgress::~PhaseProgress() {
  finish();
}

void PhaseProgress::step(const std::string& message) {
  if (!enabled_) {
    ++current_;
    return;
  }
  ++current_;
  std::cerr << label_ << " phase " << current_ << "/" << total_ << ": " << message
            << " elapsed=" << format_elapsed(started_) << '\n';
}

void PhaseProgress::finish() {
  if (!enabled_ || finished_) {
    return;
  }
  std::cerr << label_ << " phases done wall_elapsed="
            << format_elapsed(started_) << '\n';
  finished_ = true;
}

}  // namespace pgv
