#include "april.h"

#include "io.h"

#include <nlohmann/json.hpp>

#include <boost/geometry/algorithms/area.hpp>
#include <boost/geometry/algorithms/correct.hpp>
#include <boost/geometry/algorithms/covered_by.hpp>
#include <boost/geometry/algorithms/disjoint.hpp>
#include <boost/geometry/algorithms/envelope.hpp>
#include <boost/geometry/algorithms/equals.hpp>
#include <boost/geometry/algorithms/is_empty.hpp>
#include <boost/geometry/geometries/box.hpp>
#include <boost/geometry/geometries/multi_polygon.hpp>
#include <boost/geometry/geometries/point_xy.hpp>
#include <boost/geometry/geometries/polygon.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace pgv {

namespace bg = boost::geometry;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;
using BgPoint = bg::model::d2::point_xy<double>;
using BgPolygon = bg::model::polygon<BgPoint>;
using BgMultiPolygon = bg::model::multi_polygon<BgPolygon>;
using BgBox = bg::model::box<BgPoint>;

namespace {

double elapsed_seconds(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

struct IndexRange {
  int first = 0;
  int last = -1;

  bool empty() const { return first > last; }
};

bool finite_point(const json& coordinate) {
  return coordinate.is_array() && coordinate.size() >= 2 &&
         coordinate[0].is_number() && coordinate[1].is_number() &&
         std::isfinite(coordinate[0].get<double>()) &&
         std::isfinite(coordinate[1].get<double>());
}

void append_ring(const json& coordinates, BgPolygon::ring_type& ring) {
  if (!coordinates.is_array() || coordinates.size() < 4) {
    throw std::runtime_error("APRIL encountered a ring with fewer than four coordinates");
  }
  ring.clear();
  ring.reserve(coordinates.size() + 1);
  for (const auto& coordinate : coordinates) {
    if (!finite_point(coordinate)) {
      throw std::runtime_error("APRIL encountered an invalid GeoJSON coordinate");
    }
    ring.emplace_back(coordinate[0].get<double>(), coordinate[1].get<double>());
  }
  if (!bg::equals(ring.front(), ring.back())) {
    ring.push_back(ring.front());
  }
}

BgPolygon parse_polygon(const json& coordinates) {
  if (!coordinates.is_array() || coordinates.empty()) {
    throw std::runtime_error("APRIL encountered a polygon without rings");
  }
  BgPolygon polygon;
  append_ring(coordinates[0], polygon.outer());
  polygon.inners().resize(coordinates.size() - 1);
  for (size_t i = 1; i < coordinates.size(); ++i) {
    append_ring(coordinates[i], polygon.inners()[i - 1]);
  }
  bg::correct(polygon);
  if (bg::is_empty(polygon) || std::abs(bg::area(polygon)) == 0.0) {
    throw std::runtime_error("APRIL encountered an empty or zero-area polygon");
  }
  return polygon;
}

BgMultiPolygon parse_geometry(const std::string& geometry_json,
                              const std::string& cell_id) {
  json geometry = json::parse(geometry_json);
  std::string type = geometry.value("type", "");
  auto coordinates = geometry.find("coordinates");
  if (coordinates == geometry.end()) {
    throw std::runtime_error("APRIL geometry is missing coordinates for cell " + cell_id);
  }

  BgMultiPolygon result;
  if (type == "Polygon") {
    result.push_back(parse_polygon(*coordinates));
  } else if (type == "MultiPolygon") {
    if (!coordinates->is_array() || coordinates->empty()) {
      throw std::runtime_error("APRIL encountered an empty MultiPolygon for cell " + cell_id);
    }
    result.reserve(coordinates->size());
    for (const auto& polygon : *coordinates) {
      result.push_back(parse_polygon(polygon));
    }
  } else {
    throw std::runtime_error(
        "APRIL supports only Polygon and MultiPolygon geometries, got: " + type);
  }
  return result;
}

Bounds box_bounds(const BgBox& box) {
  return {bg::get<bg::min_corner, 0>(box), bg::get<bg::min_corner, 1>(box),
          bg::get<bg::max_corner, 0>(box), bg::get<bg::max_corner, 1>(box)};
}

bool overlaps(const Bounds& left, const Bounds& right, double epsilon = 0.0) {
  return left.x_max + epsilon >= right.x_min &&
         right.x_max + epsilon >= left.x_min &&
         left.y_max + epsilon >= right.y_min &&
         right.y_max + epsilon >= left.y_min;
}

IndexRange broad_range(double minimum,
                       double maximum,
                       double origin,
                       double step,
                       int count) {
  long long first = static_cast<long long>(std::floor((minimum - origin) / step)) - 1;
  long long last = static_cast<long long>(std::floor((maximum - origin) / step)) + 1;
  first = std::max<long long>(0, first);
  last = std::min<long long>(count - 1, last);
  if (first > last) {
    return {};
  }
  return {static_cast<int>(first), static_cast<int>(last)};
}

IndexRange output_row_range(double minimum,
                            double maximum,
                            const GridSpec& grid) {
  long long first = static_cast<long long>(
      std::floor((grid.bounds.y_max - maximum) / grid.y_step)) - 1;
  long long last = static_cast<long long>(
      std::floor((grid.bounds.y_max - minimum) / grid.y_step)) + 1;
  first = std::max<long long>(0, first);
  last = std::min<long long>(grid.y_res - 1, last);
  if (first > last) {
    return {};
  }
  return {static_cast<int>(first), static_cast<int>(last)};
}

BgPolygon rectangle_polygon(double x_min,
                            double y_min,
                            double x_max,
                            double y_max) {
  BgPolygon polygon;
  auto& ring = polygon.outer();
  ring.emplace_back(x_min, y_min);
  ring.emplace_back(x_min, y_max);
  ring.emplace_back(x_max, y_max);
  ring.emplace_back(x_max, y_min);
  ring.emplace_back(x_min, y_min);
  bg::correct(polygon);
  return polygon;
}

void hilbert_rotate(std::uint32_t side,
                    std::uint32_t& x,
                    std::uint32_t& y,
                    std::uint32_t rx,
                    std::uint32_t ry) {
  if (ry == 0) {
    if (rx == 1) {
      x = side - 1 - x;
      y = side - 1 - y;
    }
    std::swap(x, y);
  }
}

std::uint64_t hilbert_id(std::uint32_t size,
                         std::uint32_t input_x,
                         std::uint32_t input_y) {
  std::uint32_t x = input_x;
  std::uint32_t y = input_y;
  std::uint64_t id = 0;
  for (std::uint32_t side = size / 2; side > 0; side /= 2) {
    std::uint32_t rx = (x & side) != 0;
    std::uint32_t ry = (y & side) != 0;
    id += static_cast<std::uint64_t>(side) * side * ((3U * rx) ^ ry);
    hilbert_rotate(side, x, y, rx, ry);
  }
  return id;
}

std::vector<std::uint64_t> intervalize(std::vector<std::uint64_t> ids) {
  if (ids.empty()) {
    return {};
  }
  std::sort(ids.begin(), ids.end());
  ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
  std::vector<std::uint64_t> endpoints;
  endpoints.reserve(ids.size());
  std::uint64_t start = ids.front();
  std::uint64_t previous = start;
  for (size_t i = 1; i < ids.size(); ++i) {
    if (ids[i] != previous + 1) {
      endpoints.push_back(start);
      endpoints.push_back(previous + 1);
      start = ids[i];
    }
    previous = ids[i];
  }
  endpoints.push_back(start);
  endpoints.push_back(previous + 1);
  return endpoints;
}

bool interval_overlap(const std::vector<std::uint64_t>& left,
                      const std::vector<std::uint64_t>& right) {
  size_t left_index = 0;
  size_t right_index = 0;
  while (left_index + 1 < left.size() && right_index + 1 < right.size()) {
    std::uint64_t left_start = left[left_index];
    std::uint64_t left_end = left[left_index + 1];
    std::uint64_t right_start = right[right_index];
    std::uint64_t right_end = right[right_index + 1];
    if (left_start < right_end && right_start < left_end) {
      return true;
    }
    if (left_end <= right_end) {
      left_index += 2;
    } else {
      right_index += 2;
    }
  }
  return false;
}

int power_of_two_order(int value) {
  int order = 0;
  while (value > 1) {
    value >>= 1;
    ++order;
  }
  return order;
}

}  // namespace

AprilIndex AprilIndex::build(const MethodContext& context,
                             AprilKernelStats& stats,
                             AprilKernelTimings& timings) {
  AprilIndex index;
  index.resolution_ = context.ri_resolution.first;
  if (index.resolution_ <= 0 || index.resolution_ != context.ri_resolution.second ||
      (index.resolution_ & (index.resolution_ - 1)) != 0) {
    throw std::runtime_error("APRIL raster resolution must be square and power-of-two");
  }
  index.hilbert_order_ = power_of_two_order(index.resolution_);

  Bounds combined{
      std::min(context.dataset_bounds.x_min, context.grid.bounds.x_min),
      std::min(context.dataset_bounds.y_min, context.grid.bounds.y_min),
      std::max(context.dataset_bounds.x_max, context.grid.bounds.x_max),
      std::max(context.dataset_bounds.y_max, context.grid.bounds.y_max)};
  double side = std::max(combined.x_max - combined.x_min,
                         combined.y_max - combined.y_min);
  if (!(side > 0.0) || !std::isfinite(side)) {
    throw std::runtime_error("APRIL requires finite, non-zero dataset bounds");
  }
  index.domain_ = {combined.x_min, combined.y_min,
                   combined.x_min + side, combined.y_min + side};
  index.cell_size_ = side / index.resolution_;
  index.epsilon_ = side * 1e-12;
  index.polygons_.reserve(context.cell_count);

  auto add_record = [&](const CellRecord& record) {
    auto start = Clock::now();
    BgMultiPolygon geometry = parse_geometry(record.geometry_json, record.cell_id);
    BgBox envelope;
    bg::envelope(geometry, envelope);
    Bounds bbox = box_bounds(envelope);
    timings.geometry_adaptation_seconds += elapsed_seconds(start);

    start = Clock::now();
    IndexRange x_range = broad_range(bbox.x_min, bbox.x_max,
                                     index.domain_.x_min, index.cell_size_,
                                     index.resolution_);
    IndexRange y_range = broad_range(bbox.y_min, bbox.y_max,
                                     index.domain_.y_min, index.cell_size_,
                                     index.resolution_);
    std::vector<std::uint64_t> all_ids;
    std::vector<std::uint64_t> full_ids;
    if (!x_range.empty() && !y_range.empty()) {
      size_t reserve = static_cast<size_t>(x_range.last - x_range.first + 1) *
                       static_cast<size_t>(y_range.last - y_range.first + 1);
      all_ids.reserve(reserve);
      full_ids.reserve(reserve);
      for (int y = y_range.first; y <= y_range.last; ++y) {
        double y_min = index.domain_.y_min + y * index.cell_size_;
        double y_max = y_min + index.cell_size_;
        for (int x = x_range.first; x <= x_range.last; ++x) {
          double x_min = index.domain_.x_min + x * index.cell_size_;
          double x_max = x_min + index.cell_size_;
          BgBox cell_box(BgPoint(x_min, y_min), BgPoint(x_max, y_max));
          if (bg::disjoint(geometry, cell_box)) {
            continue;
          }
          std::uint64_t id = hilbert_id(
              static_cast<std::uint32_t>(index.resolution_),
              static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
          all_ids.push_back(id);
          BgPolygon cell = rectangle_polygon(x_min, y_min, x_max, y_max);
          if (bg::covered_by(cell, geometry)) {
            full_ids.push_back(id);
          }
        }
      }
    }

    PolygonApproximation polygon;
    polygon.polygon_index = record.polygon_index;
    polygon.bbox = bbox;
    polygon.approximation.all_cells = all_ids.size();
    polygon.approximation.full_cells = full_ids.size();
    polygon.approximation.all.endpoints = intervalize(std::move(all_ids));
    polygon.approximation.full.endpoints = intervalize(std::move(full_ids));

    stats.polygon_all_cells += polygon.approximation.all_cells;
    stats.polygon_full_cells += polygon.approximation.full_cells;
    stats.polygon_all_intervals += polygon.approximation.all.endpoints.size() / 2;
    stats.polygon_full_intervals += polygon.approximation.full.endpoints.size() / 2;
    index.polygons_.push_back(std::move(polygon));
    ++stats.input_polygons;
    timings.polygon_approximation_seconds += elapsed_seconds(start);
  };

  if (context.materialized_records) {
    for (const auto& record : *context.materialized_records) {
      add_record(record);
    }
  } else {
    for_each_record(context.records_path, add_record, context.cell_count);
  }
  if (index.polygons_.size() != context.cell_count) {
    throw std::runtime_error("APRIL record count changed while reading the dataset");
  }
  return index;
}

AprilIndex::Approximation AprilIndex::approximate_pixel(
    const PixelRow& pixel) const {
  Approximation approximation;
  IndexRange x_range = broad_range(pixel.x_min, pixel.x_max,
                                   domain_.x_min, cell_size_, resolution_);
  IndexRange y_range = broad_range(pixel.y_min, pixel.y_max,
                                   domain_.y_min, cell_size_, resolution_);
  std::vector<std::uint64_t> all_ids;
  std::vector<std::uint64_t> full_ids;
  if (!x_range.empty() && !y_range.empty()) {
    size_t reserve = static_cast<size_t>(x_range.last - x_range.first + 1) *
                     static_cast<size_t>(y_range.last - y_range.first + 1);
    all_ids.reserve(reserve);
    full_ids.reserve(reserve);
    for (int y = y_range.first; y <= y_range.last; ++y) {
      double y_min = domain_.y_min + y * cell_size_;
      double y_max = y_min + cell_size_;
      for (int x = x_range.first; x <= x_range.last; ++x) {
        double x_min = domain_.x_min + x * cell_size_;
        double x_max = x_min + cell_size_;
        Bounds cell{x_min, y_min, x_max, y_max};
        Bounds pixel_bounds{pixel.x_min, pixel.y_min, pixel.x_max, pixel.y_max};
        if (!overlaps(cell, pixel_bounds, epsilon_)) {
          continue;
        }
        std::uint64_t id = hilbert_id(
            static_cast<std::uint32_t>(resolution_),
            static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
        all_ids.push_back(id);
        if (x_min + epsilon_ >= pixel.x_min &&
            x_max <= pixel.x_max + epsilon_ &&
            y_min + epsilon_ >= pixel.y_min &&
            y_max <= pixel.y_max + epsilon_) {
          full_ids.push_back(id);
        }
      }
    }
  }
  approximation.all_cells = all_ids.size();
  approximation.full_cells = full_ids.size();
  approximation.all.endpoints = intervalize(std::move(all_ids));
  approximation.full.endpoints = intervalize(std::move(full_ids));
  return approximation;
}

AprilDecision AprilIndex::classify(const Approximation& polygon,
                                   const Approximation& pixel) const {
  if (!interval_overlap(polygon.all.endpoints, pixel.all.endpoints)) {
    return AprilDecision::Reject;
  }
  if (interval_overlap(polygon.all.endpoints, pixel.full.endpoints) ||
      interval_overlap(polygon.full.endpoints, pixel.all.endpoints)) {
    return AprilDecision::Accept;
  }
  return AprilDecision::Inconclusive;
}

AprilBatchCandidates AprilIndex::candidates_for_batch(
    const GridSpec& grid,
    size_t start_pixel,
    size_t count,
    AprilKernelStats& stats,
    AprilKernelTimings& timings,
    const std::vector<std::uint8_t>* excluded_polygons) const {
  AprilBatchCandidates result;
  if (count == 0) {
    return result;
  }

  auto start = Clock::now();
  std::vector<PixelRow> pixels;
  std::vector<Approximation> pixel_approximations;
  pixels.reserve(count);
  pixel_approximations.reserve(count);
  for (size_t offset = 0; offset < count; ++offset) {
    PixelRow pixel = grid.pixel_at(start_pixel + offset);
    Approximation approximation = approximate_pixel(pixel);
    stats.pixel_all_cells += approximation.all_cells;
    stats.pixel_full_cells += approximation.full_cells;
    stats.pixel_all_intervals += approximation.all.endpoints.size() / 2;
    stats.pixel_full_intervals += approximation.full.endpoints.size() / 2;
    ++stats.pixel_approximations;
    pixels.push_back(pixel);
    pixel_approximations.push_back(std::move(approximation));
  }
  timings.pixel_approximation_seconds += elapsed_seconds(start);

  start = Clock::now();
  int first_row = pixels.front().v;
  int last_row = pixels.back().v;
  std::vector<std::vector<size_t>> row_polygons(
      static_cast<size_t>(last_row - first_row + 1));
  for (size_t polygon_index = 0; polygon_index < polygons_.size();
       ++polygon_index) {
    const auto& polygon = polygons_[polygon_index];
    if (excluded_polygons && polygon.polygon_index >= 0 &&
        static_cast<size_t>(polygon.polygon_index) < excluded_polygons->size() &&
        (*excluded_polygons)[static_cast<size_t>(polygon.polygon_index)] != 0) {
      continue;
    }
    if (!overlaps(polygon.bbox, grid.bounds, epsilon_)) {
      continue;
    }
    IndexRange rows = output_row_range(polygon.bbox.y_min,
                                       polygon.bbox.y_max, grid);
    int first = std::max(first_row, rows.first);
    int last = std::min(last_row, rows.last);
    for (int row = first; row <= last; ++row) {
      row_polygons[static_cast<size_t>(row - first_row)].push_back(polygon_index);
    }
  }
  timings.mbr_enumeration_seconds += elapsed_seconds(start);

  start = Clock::now();
  for (size_t offset = 0; offset < pixels.size(); ++offset) {
    const PixelRow& pixel = pixels[offset];
    Bounds pixel_bounds{pixel.x_min, pixel.y_min, pixel.x_max, pixel.y_max};
    const auto& active = row_polygons[static_cast<size_t>(pixel.v - first_row)];
    for (size_t polygon_index : active) {
      const auto& polygon = polygons_[polygon_index];
      if (!overlaps(polygon.bbox, pixel_bounds, epsilon_)) {
        continue;
      }
      ++stats.mbr_candidates;
      AprilDecision decision = classify(polygon.approximation,
                                        pixel_approximations[offset]);
      if (decision == AprilDecision::Reject) {
        ++stats.rejected_pairs;
        continue;
      }
      if (decision == AprilDecision::Accept) {
        ++stats.accepted_pairs;
      } else {
        ++stats.inconclusive_pairs;
      }
      result.pairs.emplace_back(polygon.polygon_index, pixel.pixel_id);
    }
  }
  timings.interval_filter_seconds += elapsed_seconds(start);
  return result;
}

}  // namespace pgv
