#pragma once

#include "types.h"

#include <cstdint>
#include <utility>
#include <vector>

namespace pgv {

enum class AprilDecision {
  Reject,
  Accept,
  Inconclusive,
};

struct AprilKernelStats {
  std::uint64_t input_polygons = 0;
  std::uint64_t polygon_all_cells = 0;
  std::uint64_t polygon_full_cells = 0;
  std::uint64_t polygon_all_intervals = 0;
  std::uint64_t polygon_full_intervals = 0;
  std::uint64_t pixel_approximations = 0;
  std::uint64_t pixel_all_cells = 0;
  std::uint64_t pixel_full_cells = 0;
  std::uint64_t pixel_all_intervals = 0;
  std::uint64_t pixel_full_intervals = 0;
  std::uint64_t mbr_candidates = 0;
  std::uint64_t rejected_pairs = 0;
  std::uint64_t accepted_pairs = 0;
  std::uint64_t inconclusive_pairs = 0;
  std::uint64_t missing_exact_pairs = 0;
  std::uint64_t false_positive_pairs = 0;
};

struct AprilKernelTimings {
  double geometry_adaptation_seconds = 0.0;
  double polygon_approximation_seconds = 0.0;
  double pixel_approximation_seconds = 0.0;
  double mbr_enumeration_seconds = 0.0;
  double interval_filter_seconds = 0.0;
};

struct AprilBatchCandidates {
  std::vector<std::pair<long long, long long>> pairs;
};

class AprilIndex {
 public:
  static AprilIndex build(const MethodContext& context,
                          AprilKernelStats& stats,
                          AprilKernelTimings& timings);

  AprilBatchCandidates candidates_for_batch(
      const GridSpec& grid,
      size_t start_pixel,
      size_t count,
      AprilKernelStats& stats,
      AprilKernelTimings& timings,
      const std::vector<std::uint8_t>* excluded_polygons = nullptr) const;

  int resolution() const { return resolution_; }
  int hilbert_order() const { return hilbert_order_; }
  const Bounds& domain() const { return domain_; }

 private:
  struct IntervalList {
    std::vector<std::uint64_t> endpoints;
  };

  struct Approximation {
    IntervalList all;
    IntervalList full;
    std::uint64_t all_cells = 0;
    std::uint64_t full_cells = 0;
  };

  struct PolygonApproximation {
    long long polygon_index = 0;
    Bounds bbox;
    Approximation approximation;
  };

  Bounds domain_;
  int resolution_ = 0;
  int hilbert_order_ = 0;
  double cell_size_ = 0.0;
  double epsilon_ = 0.0;
  std::vector<PolygonApproximation> polygons_;

  Approximation approximate_pixel(const PixelRow& pixel) const;
  AprilDecision classify(const Approximation& polygon,
                         const Approximation& pixel) const;
};

}  // namespace pgv
