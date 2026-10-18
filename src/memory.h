#pragma once

#include "types.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace pgv {

class MemoryMonitor {
 public:
  explicit MemoryMonitor(int sampling_interval_ms = 10);
  MemoryMonitor(const MemoryMonitor&) = delete;
  MemoryMonitor& operator=(const MemoryMonitor&) = delete;
  ~MemoryMonitor();

  MemoryStats stop();

 private:
  void sample();
  void run();

  int sampling_interval_ms_ = 10;
  std::uint64_t start_bytes_ = 0;
  std::uint64_t end_bytes_ = 0;
  std::atomic<std::uint64_t> peak_bytes_{0};
  std::atomic<bool> stop_requested_{false};
  bool stopped_ = false;
  std::string source_;
  std::mutex mutex_;
  std::condition_variable condition_;
  std::thread worker_;
};

}  // namespace pgv
