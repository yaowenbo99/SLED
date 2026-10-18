#include "memory.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <stdexcept>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace pgv {
namespace {

std::uint64_t current_rss_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  kern_return_t status = task_info(
      mach_task_self(), MACH_TASK_BASIC_INFO,
      reinterpret_cast<task_info_t>(&info), &count);
  return status == KERN_SUCCESS
             ? static_cast<std::uint64_t>(info.resident_size)
             : 0;
#elif defined(__linux__)
  std::ifstream statm("/proc/self/statm");
  std::uint64_t total_pages = 0;
  std::uint64_t resident_pages = 0;
  if (!(statm >> total_pages >> resident_pages)) {
    return 0;
  }
  long page_size = sysconf(_SC_PAGESIZE);
  return page_size > 0
             ? resident_pages * static_cast<std::uint64_t>(page_size)
             : 0;
#else
  return 0;
#endif
}

const char* memory_source() {
#if defined(__APPLE__)
  return "mach_task_info";
#elif defined(__linux__)
  return "linux_proc_statm";
#else
  return "unsupported";
#endif
}

}  // namespace

MemoryMonitor::MemoryMonitor(int sampling_interval_ms)
    : sampling_interval_ms_(sampling_interval_ms), source_(memory_source()) {
  if (sampling_interval_ms_ <= 0) {
    throw std::runtime_error("memory sampling interval must be positive");
  }
  start_bytes_ = current_rss_bytes();
  if (start_bytes_ == 0) {
    throw std::runtime_error("failed to read process resident memory");
  }
  peak_bytes_.store(start_bytes_);
  worker_ = std::thread(&MemoryMonitor::run, this);
}

MemoryMonitor::~MemoryMonitor() {
  try {
    stop();
  } catch (...) {
  }
}

void MemoryMonitor::sample() {
  std::uint64_t current = current_rss_bytes();
  if (current == 0) {
    return;
  }
  std::uint64_t observed = peak_bytes_.load(std::memory_order_relaxed);
  while (current > observed &&
         !peak_bytes_.compare_exchange_weak(
             observed, current, std::memory_order_relaxed)) {
  }
}

void MemoryMonitor::run() {
  std::unique_lock<std::mutex> lock(mutex_);
  while (!condition_.wait_for(
      lock, std::chrono::milliseconds(sampling_interval_ms_),
      [&] { return stop_requested_.load(std::memory_order_relaxed); })) {
    lock.unlock();
    sample();
    lock.lock();
  }
}

MemoryStats MemoryMonitor::stop() {
  if (!stopped_) {
    stop_requested_.store(true, std::memory_order_relaxed);
    condition_.notify_all();
    if (worker_.joinable()) {
      worker_.join();
    }
    sample();
    end_bytes_ = current_rss_bytes();
    if (end_bytes_ == 0) {
      end_bytes_ = peak_bytes_.load(std::memory_order_relaxed);
    }
    std::uint64_t observed = peak_bytes_.load(std::memory_order_relaxed);
    peak_bytes_.store(std::max(observed, end_bytes_), std::memory_order_relaxed);
    stopped_ = true;
  }

  std::uint64_t peak = peak_bytes_.load(std::memory_order_relaxed);
  return {start_bytes_, peak, peak >= start_bytes_ ? peak - start_bytes_ : 0,
          end_bytes_, sampling_interval_ms_, source_};
}

}  // namespace pgv
