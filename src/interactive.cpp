#include "interactive.h"

#include "april.h"
#include "duckdb.h"
#include "io.h"
#include "memory.h"
#include "methods.h"
#include "progress.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace pgv {
namespace {

using Clock = std::chrono::steady_clock;
constexpr std::uint64_t kInteractiveSeed = 42;

struct Point {
  double x = 0.0;
  double y = 0.0;
};

struct Edge {
  double x_upper = 0.0;
  double y_upper = 0.0;
  double x_lower = 0.0;
  double y_lower = 0.0;
  long double coefficient = 0.0L;
  int count_delta = 1;
};

struct IndexStats {
  size_t input_edges = 0;
  size_t nonhorizontal_edges = 0;
  size_t unique_edges = 0;
  size_t residual_edges = 0;
  size_t cancelled_edges = 0;
};

struct SweepTiming {
  double schedule_seconds = 0.0;
  double fragment_seconds = 0.0;
  double prefix_seconds = 0.0;

  double algorithm_seconds() const {
    return schedule_seconds + fragment_seconds + prefix_seconds;
  }
};

struct TrialSpec {
  std::string operation;
  std::string trial_id;
  std::optional<double> viewport_scale;
  std::optional<int> pan_index;
  std::optional<int> delete_ratio;
  size_t deleted_count = 0;
  size_t remaining_count = 0;
  Bounds bounds;
  std::vector<size_t> deleted_polygons;
};

struct TrialResult {
  std::string method;
  TrialSpec spec;
  RunSummary summary;
};

bool same_point(const Point& left, const Point& right) {
  return left.x == right.x && left.y == right.y;
}

bool same_key(const Edge& left, const Edge& right) {
  return left.x_upper == right.x_upper && left.y_upper == right.y_upper &&
         left.x_lower == right.x_lower && left.y_lower == right.y_lower;
}

bool key_less(const Edge& left, const Edge& right) {
  if (left.x_upper != right.x_upper) return left.x_upper < right.x_upper;
  if (left.y_upper != right.y_upper) return left.y_upper < right.y_upper;
  if (left.x_lower != right.x_lower) return left.x_lower < right.x_lower;
  return left.y_lower < right.y_lower;
}

Point parse_point(const json& coordinate) {
  if (!coordinate.is_array() || coordinate.size() < 2 ||
      !coordinate[0].is_number() || !coordinate[1].is_number()) {
    throw std::runtime_error("interactive SLED encountered an invalid coordinate");
  }
  Point point{coordinate[0].get<double>(), coordinate[1].get<double>()};
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    throw std::runtime_error("interactive SLED encountered a non-finite coordinate");
  }
  return point;
}

std::vector<Point> corrected_ring(const json& coordinates, bool outer) {
  if (!coordinates.is_array() || coordinates.size() < 3) {
    throw std::runtime_error("interactive SLED encountered a ring with fewer than three coordinates");
  }
  std::vector<Point> ring;
  ring.reserve(coordinates.size() + 1);
  for (const auto& coordinate : coordinates) {
    Point point = parse_point(coordinate);
    if (ring.empty() || !same_point(ring.back(), point)) ring.push_back(point);
  }
  if (ring.size() < 3) {
    throw std::runtime_error("interactive SLED encountered a degenerate ring");
  }
  if (!same_point(ring.front(), ring.back())) ring.push_back(ring.front());
  long double twice_area = 0.0L;
  for (size_t i = 0; i + 1 < ring.size(); ++i) {
    twice_area += static_cast<long double>(ring[i].x) * ring[i + 1].y -
                  static_cast<long double>(ring[i + 1].x) * ring[i].y;
  }
  if (twice_area == 0.0L) {
    throw std::runtime_error("interactive SLED encountered a zero-area ring");
  }
  if ((twice_area > 0.0L) != outer) std::reverse(ring.begin(), ring.end());
  return ring;
}

void add_ring_edges(const json& coordinates,
                    bool outer,
                    long double weight,
                    std::vector<Edge>& edges,
                    IndexStats& stats) {
  std::vector<Point> ring = corrected_ring(coordinates, outer);
  for (size_t i = 0; i + 1 < ring.size(); ++i) {
    const Point& start = ring[i];
    const Point& end = ring[i + 1];
    ++stats.input_edges;
    if (same_point(start, end) || start.y == end.y) continue;
    ++stats.nonhorizontal_edges;
    if (start.y > end.y) {
      edges.push_back({start.x, start.y, end.x, end.y, weight, 1});
    } else {
      edges.push_back({end.x, end.y, start.x, start.y, -weight, 1});
    }
  }
}

void add_polygon_edges(const json& coordinates,
                       long double weight,
                       std::vector<Edge>& edges,
                       IndexStats& stats) {
  if (!coordinates.is_array() || coordinates.empty()) {
    throw std::runtime_error("interactive SLED encountered a polygon without rings");
  }
  for (size_t ring = 0; ring < coordinates.size(); ++ring) {
    add_ring_edges(coordinates[ring], ring == 0, weight, edges, stats);
  }
}

void add_geometry_edges(const CellRecord& record,
                        std::vector<Edge>& edges,
                        IndexStats& stats) {
  json geometry = json::parse(record.geometry_json);
  std::string type = geometry.value("type", "");
  auto coordinates = geometry.find("coordinates");
  if (coordinates == geometry.end()) {
    throw std::runtime_error("interactive SLED geometry is missing coordinates for cell " +
                             record.cell_id);
  }
  long double weight = static_cast<long double>(record.weight);
  if (type == "Polygon") {
    add_polygon_edges(*coordinates, weight, edges, stats);
  } else if (type == "MultiPolygon") {
    if (!coordinates->is_array() || coordinates->empty()) {
      throw std::runtime_error("interactive SLED encountered an empty MultiPolygon");
    }
    for (const auto& polygon : *coordinates) {
      add_polygon_edges(polygon, weight, edges, stats);
    }
  } else {
    throw std::runtime_error("interactive SLED supports only Polygon and MultiPolygon");
  }
}

long double snap_near_integer(long double value) {
  long double nearest = std::round(value);
  long double tolerance = 64.0L * std::numeric_limits<long double>::epsilon() *
                          std::max(1.0L, std::abs(value));
  return std::abs(value - nearest) <= tolerance ? nearest : value;
}

long double interpolate_x(const Edge& edge, long double y) {
  long double t = (static_cast<long double>(edge.y_upper) - y) /
                  (static_cast<long double>(edge.y_upper) - edge.y_lower);
  return static_cast<long double>(edge.x_upper) +
         (static_cast<long double>(edge.x_lower) - edge.x_upper) * t;
}

int containing_column(long double x, const GridSpec& grid) {
  long double position = snap_near_integer(
      (x - static_cast<long double>(grid.bounds.x_min)) /
      static_cast<long double>(grid.x_step));
  int column = static_cast<int>(std::floor(position));
  if (column < 0) return -1;
  return column >= grid.x_res ? grid.x_res : column;
}

std::optional<std::pair<int, int>> edge_row_span(const Edge& edge,
                                                 const GridSpec& grid) {
  long double clipped_top = std::min(static_cast<long double>(edge.y_upper),
                                     static_cast<long double>(grid.bounds.y_max));
  long double clipped_bottom = std::max(static_cast<long double>(edge.y_lower),
                                        static_cast<long double>(grid.bounds.y_min));
  if (!(clipped_bottom < clipped_top)) return std::nullopt;
  long double first_position = snap_near_integer(
      (static_cast<long double>(grid.bounds.y_max) - clipped_top) / grid.y_step);
  long double last_position = snap_near_integer(
      (static_cast<long double>(grid.bounds.y_max) - clipped_bottom) / grid.y_step);
  int first = static_cast<int>(std::floor(first_position));
  int last = static_cast<int>(std::ceil(last_position)) - 1;
  first = std::clamp(first, 0, grid.y_res - 1);
  last = std::clamp(last, 0, grid.y_res - 1);
  if (first > last) return std::nullopt;
  return std::pair<int, int>{first, last};
}

void accumulate_fragments(const Edge& edge,
                          int row,
                          const GridSpec& grid,
                          std::vector<long double>& boundary,
                          std::vector<long double>& difference,
                          std::vector<long long>& candidates) {
  long double x_min = grid.bounds.x_min;
  long double x_max = grid.bounds.x_max;
  long double x_step = grid.x_step;
  long double row_top = static_cast<long double>(grid.bounds.y_max) -
                        static_cast<long double>(row) * grid.y_step;
  long double row_bottom = static_cast<long double>(grid.bounds.y_max) -
                           static_cast<long double>(row + 1) * grid.y_step;
  long double segment_top = std::min(static_cast<long double>(edge.y_upper), row_top);
  long double segment_bottom = std::max(static_cast<long double>(edge.y_lower), row_bottom);
  if (!(segment_bottom < segment_top)) return;

  long double x_top = interpolate_x(edge, segment_top);
  long double x_bottom = interpolate_x(edge, segment_bottom);
  std::vector<long double> parameters = {0.0L, 1.0L};
  if (x_top != x_bottom) {
    long double minimum = std::min(x_top, x_bottom);
    long double maximum = std::max(x_top, x_bottom);
    int first_line = std::max(
        static_cast<int>(std::floor(snap_near_integer((minimum - x_min) / x_step))) + 1, 0);
    int last_line = std::min(
        static_cast<int>(std::ceil(snap_near_integer((maximum - x_min) / x_step))) - 1,
        grid.x_res);
    for (int line = first_line; line <= last_line; ++line) {
      long double grid_x = static_cast<long double>(grid.bounds.x_min) +
                           static_cast<long double>(line) * grid.x_step;
      long double t = (grid_x - x_top) / (x_bottom - x_top);
      if (t > 0.0L && t < 1.0L) parameters.push_back(t);
    }
  }
  std::sort(parameters.begin(), parameters.end());
  parameters.erase(std::unique(parameters.begin(), parameters.end()), parameters.end());

  long double vertical_span = segment_top - segment_bottom;
  for (size_t i = 0; i + 1 < parameters.size(); ++i) {
    long double begin = parameters[i];
    long double end = parameters[i + 1];
    if (!(begin < end)) continue;
    long double x_begin = x_top + (x_bottom - x_top) * begin;
    long double x_end = x_top + (x_bottom - x_top) * end;
    long double height = vertical_span * (end - begin);
    if (!(height > 0.0L)) continue;
    long double midpoint = (x_begin + x_end) * 0.5L;
    long double suffix = edge.coefficient * height * x_step;
    if (midpoint < x_min) {
      difference[0] += suffix;
      difference[static_cast<size_t>(grid.x_res)] -= suffix;
      continue;
    }
    if (midpoint >= x_max) continue;
    int column = containing_column(midpoint, grid);
    if (column < 0) {
      difference[0] += suffix;
      difference[static_cast<size_t>(grid.x_res)] -= suffix;
      continue;
    }
    if (column >= grid.x_res) continue;
    long double right = static_cast<long double>(grid.bounds.x_min) +
                        static_cast<long double>(column + 1) * grid.x_step;
    long double right_area = ((right - x_begin) + (right - x_end)) * 0.5L * height;
    boundary[static_cast<size_t>(column)] += edge.coefficient * right_area;
    difference[static_cast<size_t>(column + 1)] += suffix;
    difference[static_cast<size_t>(grid.x_res)] -= suffix;
    candidates[static_cast<size_t>(column)] += edge.count_delta;
  }
}

SweepTiming sweep_edges(const std::vector<Edge>& edges,
                        const GridSpec& grid,
                        const ResultBatchConsumer& consumer,
                        bool show_progress,
                        const std::string& label) {
  SweepTiming timing;
  auto started = Clock::now();
  std::vector<int32_t> last_rows(edges.size(), -1);
  std::vector<size_t> offsets(static_cast<size_t>(grid.y_res + 1), 0);
  size_t scheduled = 0;
  for (size_t i = 0; i < edges.size(); ++i) {
    auto span = edge_row_span(edges[i], grid);
    if (!span) continue;
    ++offsets[static_cast<size_t>(span->first + 1)];
    last_rows[i] = span->second;
    ++scheduled;
  }
  for (size_t row = 1; row < offsets.size(); ++row) offsets[row] += offsets[row - 1];
  std::vector<uint32_t> entries(scheduled);
  std::vector<size_t> cursors = offsets;
  for (size_t i = 0; i < edges.size(); ++i) {
    if (last_rows[i] < 0) continue;
    auto span = edge_row_span(edges[i], grid);
    entries[cursors[static_cast<size_t>(span->first)]++] = static_cast<uint32_t>(i);
  }
  timing.schedule_seconds = std::chrono::duration<double>(Clock::now() - started).count();

  std::vector<uint32_t> active;
  std::vector<long double> boundary(static_cast<size_t>(grid.x_res), 0.0L);
  std::vector<long double> difference(static_cast<size_t>(grid.x_res + 1), 0.0L);
  std::vector<long long> candidates(static_cast<size_t>(grid.x_res), 0);
  std::vector<ResultRow> output;
  output.reserve(static_cast<size_t>(grid.x_res));
  ProgressBar progress(label, static_cast<size_t>(grid.y_res), show_progress);
  for (int row = 0; row < grid.y_res; ++row) {
    active.insert(active.end(), entries.begin() + offsets[static_cast<size_t>(row)],
                  entries.begin() + offsets[static_cast<size_t>(row + 1)]);
    std::fill(boundary.begin(), boundary.end(), 0.0L);
    std::fill(difference.begin(), difference.end(), 0.0L);
    std::fill(candidates.begin(), candidates.end(), 0);
    started = Clock::now();
    size_t retained = 0;
    for (uint32_t index : active) {
      accumulate_fragments(edges[index], row, grid, boundary, difference, candidates);
      if (last_rows[index] > row) active[retained++] = index;
    }
    active.resize(retained);
    timing.fragment_seconds += std::chrono::duration<double>(Clock::now() - started).count();

    started = Clock::now();
    output.clear();
    long double suffix = 0.0L;
    size_t row_start = static_cast<size_t>(row) * grid.x_res;
    for (int column = 0; column < grid.x_res; ++column) {
      suffix += difference[static_cast<size_t>(column)];
      output.push_back({column, row, static_cast<long long>(row_start + column),
                        static_cast<double>(boundary[static_cast<size_t>(column)] + suffix),
                        candidates[static_cast<size_t>(column)]});
    }
    timing.prefix_seconds += std::chrono::duration<double>(Clock::now() - started).count();
    consumer(output);
    progress.update(static_cast<size_t>(row + 1));
  }
  progress.finish();
  return timing;
}

class SledInteractiveIndex {
 public:
  static SledInteractiveIndex build(const std::vector<CellRecord>& records,
                                    size_t polygon_count,
                                    bool show_progress) {
    SledInteractiveIndex index;
    index.polygon_offsets_.push_back(0);
    ProgressBar progress("interactive SLED polygons", polygon_count, show_progress);
    for (const CellRecord& record : records) {
      add_geometry_edges(record, index.polygon_edges_, index.stats_);
      index.polygon_offsets_.push_back(index.polygon_edges_.size());
      progress.update(index.polygon_offsets_.size() - 1);
    }
    progress.finish();
    if (index.polygon_offsets_.size() != polygon_count + 1) {
      throw std::runtime_error("interactive SLED polygon count changed during preprocessing");
    }
    std::vector<uint32_t> order(index.polygon_edges_.size());
    if (order.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::runtime_error("interactive SLED edge index exceeds uint32 capacity");
    }
    std::iota(order.begin(), order.end(), 0U);
    std::sort(order.begin(), order.end(), [&](uint32_t left, uint32_t right) {
      return key_less(index.polygon_edges_[left], index.polygon_edges_[right]);
    });
    for (size_t begin = 0; begin < order.size();) {
      size_t end = begin + 1;
      Edge reduced = index.polygon_edges_[order[begin]];
      while (end < order.size() &&
             same_key(index.polygon_edges_[order[begin]], index.polygon_edges_[order[end]])) {
        reduced.coefficient += index.polygon_edges_[order[end]].coefficient;
        ++end;
      }
      ++index.stats_.unique_edges;
      if (reduced.coefficient == 0.0L) {
        ++index.stats_.cancelled_edges;
      } else {
        reduced.count_delta = 1;
        index.residual_edges_.push_back(reduced);
      }
      begin = end;
    }
    index.stats_.residual_edges = index.residual_edges_.size();
    return index;
  }

  // Apply the deletion to the reusable edge representation, then sweep every
  // remaining contribution. This is the full-recomputation ablation.
  std::vector<Edge> after_deletion(const std::vector<size_t>& deleted) const {
    auto delta = deletion_delta(deleted);
    std::vector<Edge> remaining;
    remaining.reserve(residual_edges_.size() + delta.size());
    size_t i = 0, j = 0;
    while (i < residual_edges_.size() || j < delta.size()) {
      Edge edge;
      if (j == delta.size() || (i < residual_edges_.size() && key_less(residual_edges_[i], delta[j]))) {
        edge = residual_edges_[i++];
      } else if (i == residual_edges_.size() || key_less(delta[j], residual_edges_[i])) {
        edge = delta[j++];
      } else {
        edge = residual_edges_[i++];
        edge.coefficient += delta[j++].coefficient;
      }
      if (edge.coefficient != 0.0L) {
        edge.count_delta = 1;
        remaining.push_back(edge);
      }
    }
    return remaining;
  }

  const std::vector<Edge>& residual_edges() const { return residual_edges_; }
  const IndexStats& stats() const { return stats_; }
  size_t polygon_count() const { return polygon_offsets_.size() - 1; }

  std::vector<Edge> deletion_delta(const std::vector<size_t>& deleted) const {
    size_t edge_count = 0;
    for (size_t polygon : deleted) {
      if (polygon >= polygon_count()) {
        throw std::runtime_error("interactive deletion polygon index is out of range");
      }
      edge_count += polygon_offsets_[polygon + 1] - polygon_offsets_[polygon];
    }
    std::vector<Edge> changes;
    changes.reserve(edge_count);
    for (size_t polygon : deleted) {
      for (size_t i = polygon_offsets_[polygon]; i < polygon_offsets_[polygon + 1]; ++i) {
        Edge edge = polygon_edges_[i];
        edge.coefficient = -edge.coefficient;
        edge.count_delta = 0;
        changes.push_back(edge);
      }
    }
    std::sort(changes.begin(), changes.end(), key_less);
    std::vector<Edge> reduced;
    for (size_t begin = 0; begin < changes.size();) {
      size_t end = begin + 1;
      Edge change = changes[begin];
      while (end < changes.size() && same_key(changes[begin], changes[end])) {
        change.coefficient += changes[end].coefficient;
        ++end;
      }
      auto position = std::lower_bound(residual_edges_.begin(), residual_edges_.end(),
                                       change, key_less);
      long double old_coefficient =
          position != residual_edges_.end() && same_key(*position, change)
              ? position->coefficient
              : 0.0L;
      long double new_coefficient = old_coefficient + change.coefficient;
      change.count_delta = (new_coefficient != 0.0L ? 1 : 0) -
                           (old_coefficient != 0.0L ? 1 : 0);
      if (change.coefficient != 0.0L || change.count_delta != 0) {
        reduced.push_back(change);
      }
      begin = end;
    }
    return reduced;
  }

 private:
  std::vector<Edge> polygon_edges_;
  std::vector<size_t> polygon_offsets_;
  std::vector<Edge> residual_edges_;
  IndexStats stats_;
};

std::string ratio_string(double value) {
  std::ostringstream out;
  out << std::setprecision(15) << std::defaultfloat << value;
  return out.str();
}

std::string ratio_label(double value) {
  std::string label = ratio_string(value);
  std::replace(label.begin(), label.end(), '.', 'p');
  std::replace(label.begin(), label.end(), '+', 'p');
  std::replace(label.begin(), label.end(), '-', 'm');
  return label;
}

size_t selected_count(size_t source_count, double ratio) {
  if (ratio >= 100.0) return source_count;
  size_t count = static_cast<size_t>(std::floor(
      static_cast<long double>(source_count) * ratio / 100.0L));
  return std::max<size_t>(1, std::min(source_count, count));
}

Bounds scaled_centered(const Bounds& base, double ratio) {
  double center_x = (base.x_min + base.x_max) * 0.5;
  double center_y = (base.y_min + base.y_max) * 0.5;
  double half_width = (base.x_max - base.x_min) * ratio * 0.5;
  double half_height = (base.y_max - base.y_min) * ratio * 0.5;
  return {center_x - half_width, center_y - half_height,
          center_x + half_width, center_y + half_height};
}

double stable_unit(std::mt19937_64& generator) {
  return static_cast<double>(generator() >> 11) / 9007199254740992.0;
}

std::vector<size_t> stable_permutation(size_t count) {
  std::vector<size_t> order(count);
  std::iota(order.begin(), order.end(), 0);
  std::mt19937_64 generator(kInteractiveSeed);
  for (size_t i = count; i > 1; --i) {
    size_t selected = static_cast<size_t>(generator() % i);
    std::swap(order[i - 1], order[selected]);
  }
  return order;
}

std::string csv_field(const std::string& value) {
  if (value.find_first_of(",\"\n\r") == std::string::npos) return value;
  std::string escaped = "\"";
  for (char ch : value) escaped += ch == '"' ? "\"\"" : std::string(1, ch);
  escaped += '"';
  return escaped;
}

std::string dataset_name(const Options& opts, const fs::path& records) {
  if (!opts.records || opts.dataset != "nyc_taxi_zones") return opts.dataset;
  std::string name = records.filename().string();
  const std::string suffix = "_records.jsonl";
  if (name.size() > suffix.size() &&
      name.substr(name.size() - suffix.size()) == suffix) {
    return name.substr(0, name.size() - suffix.size());
  }
  return records.stem().string();
}

TrialResult run_sled_trial(const TrialSpec& spec,
                           const SledInteractiveIndex& index,
                           const GridSpec& grid,
                           const std::vector<double>* baseline_density,
                           const std::vector<long long>* baseline_candidates,
                           const fs::path& task_dir,
                           const fs::path& records,
                           const std::string& dataset,
                           const std::string& resolution,
                           const std::string& fingerprint,
                           size_t source_count,
                           double dataset_ratio,
                           bool show_progress,
                           const Options& opts,
                           double initialization_seconds) {
  auto end_to_end_start = Clock::now();
  fs::create_directories(task_dir);
  fs::path csv_path = task_dir / "pixels.csv";
  fs::path json_path = task_dir / "pixels.json";
  fs::path info_path = task_dir / "run_info.txt";
  release_unused_memory();
  MemoryMonitor monitor;
  auto computation_start = Clock::now();
  auto dense_ready = computation_start;
  std::vector<ResultRow> result_rows;
  result_rows.reserve(grid.pixel_count);
  long long total_candidates = 0;
  double materialization_seconds = 0.0;
  bool delta_update = spec.operation == "updating" && opts.sled_update_mode == "delta";

  std::vector<Edge> deletion_edges;
  double deletion_preparation_seconds = 0.0;
  const std::vector<Edge>* sweep_input = &index.residual_edges();
  if (spec.operation == "updating") {
    auto started = Clock::now();
    deletion_edges = delta_update ? index.deletion_delta(spec.deleted_polygons)
                                  : index.after_deletion(spec.deleted_polygons);
    deletion_preparation_seconds =
        std::chrono::duration<double>(Clock::now() - started).count();
    sweep_input = &deletion_edges;
    if (delta_update && (!baseline_density || !baseline_candidates)) {
      throw std::runtime_error("interactive update is missing the initial density map");
    }
  }

  auto consume = [&](const std::vector<ResultRow>& rows) {
    auto started = Clock::now();
    for (ResultRow row : rows) {
      size_t pixel = static_cast<size_t>(row.pixel_id);
      if (pixel != result_rows.size()) throw std::runtime_error("non-contiguous SLED output");
      if (delta_update) {
        row.density += (*baseline_density)[pixel];
        row.candidate_count += (*baseline_candidates)[pixel];
        if (row.candidate_count < 0) throw std::runtime_error("negative updated candidate count");
      }
      total_candidates += row.candidate_count;
      result_rows.push_back(row);
    }
    auto collected = Clock::now();
    materialization_seconds += std::chrono::duration<double>(collected - started).count();
    if (result_rows.size() == grid.pixel_count) dense_ready = collected;
  };

  SweepTiming sweep = sweep_edges(*sweep_input, grid, consume, show_progress,
                                  "SLED interactive " + spec.trial_id);
  if (result_rows.size() != grid.pixel_count) throw std::runtime_error("incomplete SLED grid");
  double computation_seconds = std::chrono::duration<double>(dense_ready - computation_start).count();
  MemoryStats memory = monitor.stop();
  double serialization_seconds = 0.0;
  if (!opts.discard_output) {
    auto started = Clock::now();
    ResultWriter writer(csv_path, json_path);
    writer.append_batch(result_rows);
    writer.finish();
    serialization_seconds = std::chrono::duration<double>(Clock::now() - started).count();
  }
  std::map<std::string, std::string> validation_metadata;
  if (opts.validation_grid) {
    serialization_seconds += write_validation_grid(
        task_dir / "validation_density.f64le", result_rows, grid, validation_metadata);
  }
  double runtime = std::chrono::duration<double>(Clock::now() - end_to_end_start).count();
  double algorithm = deletion_preparation_seconds + sweep.algorithm_seconds();

  RunSummary summary;
  summary.method = "SLED";
  summary.dataset = dataset;
  summary.records_path = fs::absolute(records).string();
  summary.resolution = resolution;
  summary.bbox = spec.bounds;
  summary.cell_count = spec.remaining_count;
  summary.pixel_count = grid.pixel_count;
  summary.total_candidate_pairs = total_candidates;
  summary.runtime_seconds = runtime;
  summary.algorithm_seconds = algorithm;
  summary.computation_seconds = computation_seconds;
  summary.serialization_seconds = serialization_seconds;
  summary.initialization_seconds = initialization_seconds;
  summary.duckdb_version = "not_used";
  summary.task_dir = fs::absolute(task_dir).string();
  summary.output_csv = opts.discard_output ? "" : fs::absolute(csv_path).string();
  summary.output_json = opts.discard_output ? "" : fs::absolute(json_path).string();
  summary.output_info = fs::absolute(info_path).string();
  summary.extra = {{"execution_mode", "streaming"},
                   {"interactive_operation", spec.operation},
                   {"interactive_trial_id", spec.trial_id},
                   {"interactive_random_seed", std::to_string(kInteractiveSeed)},
                   {"interactive_timing_scope", "request_start_to_dense_grid_ready_including_deletion_and_merge"},
                   {"dataset_fingerprint", fingerprint},
                   {"dataset_ratio_percent", ratio_string(dataset_ratio)},
                   {"source_cell_count", std::to_string(source_count)},
                   {"selected_cell_count", std::to_string(index.polygon_count())},
                   {"deleted_polygon_count", std::to_string(spec.deleted_count)},
                   {"remaining_polygon_count", std::to_string(spec.remaining_count)},
                   {"deleted_polygon_indices", json(spec.deleted_polygons).dump()},
                   {"sled_interactive_changed_edges", std::to_string(deletion_edges.size())},
                   {"sled_interactive_index_reused", "true"}};
  if (spec.viewport_scale) {
    summary.extra["viewport_scale"] = ratio_string(*spec.viewport_scale);
  }
  if (spec.pan_index) summary.extra["pan_index"] = std::to_string(*spec.pan_index);
  if (spec.delete_ratio) {
    summary.extra["delete_ratio_percent"] = std::to_string(*spec.delete_ratio);
  }
  summary.extra["sled_update_mode"] = opts.sled_update_mode;
  summary.extra.insert(validation_metadata.begin(), validation_metadata.end());
  summary.extra["output_discarded"] = opts.discard_output ? "true" : "false";
  summary.extra["operation"] = spec.operation;
  summary.extra["trial_id"] = spec.trial_id;
  summary.timing_seconds = {
      {"interactive_result_materialization_seconds", materialization_seconds},
      {"sled_interactive_deletion_preparation_seconds", deletion_preparation_seconds},
      {"sled_interactive_event_schedule_seconds", sweep.schedule_seconds},
      {"sled_interactive_fragment_accumulation_seconds", sweep.fragment_seconds},
      {"sled_interactive_prefix_recovery_seconds", sweep.prefix_seconds},
      {"sled_interactive_algorithm_seconds", algorithm},
      {"sled_interactive_total_seconds", runtime}};
  summary.memory = memory;
  write_run_info(info_path, summary);
  return {"SLED", spec, summary};
}

void write_trial_fields(std::ostream& out, const TrialResult& trial) {
  const TrialSpec& spec = trial.spec;
  const RunSummary& summary = trial.summary;
  out << spec.operation << ',' << spec.trial_id << ',';
  if (spec.viewport_scale) out << *spec.viewport_scale;
  out << ',';
  if (spec.pan_index) out << *spec.pan_index;
  out << ',';
  if (spec.delete_ratio) out << *spec.delete_ratio;
  out << ',' << spec.deleted_count << ',' << spec.remaining_count << ','
      << spec.bounds.x_min << ',' << spec.bounds.y_min << ','
      << spec.bounds.x_max << ',' << spec.bounds.y_max << ','
      << summary.runtime_seconds << ',' << summary.algorithm_seconds << ','
      << summary.computation_seconds << ',' << summary.serialization_seconds << ','
      << "v2_dense_memory,"
      << summary.memory.peak_rss_bytes << ','
      << static_cast<long double>(summary.memory.peak_rss_bytes) /
             (1024.0L * 1024.0L)
      << ',' << csv_field(summary.task_dir) << '\n';
}

void write_results(const fs::path& path,
                   const std::vector<TrialResult>& trials,
                   bool include_method) {
  std::ofstream out(path);
  if (!out) throw std::runtime_error("cannot write interactive results: " + path.string());
  if (include_method) out << "method,";
  out << "operation,trial_id,viewport_scale,pan_index,delete_ratio_percent,"
         "deleted_polygon_count,remaining_polygon_count,x_min,y_min,x_max,y_max,"
         "runtime_seconds,algorithm_seconds,computation_seconds,serialization_seconds,timing_contract,memory_peak_rss_bytes,memory_peak_rss_mib,task_dir\n";
  out << std::setprecision(17);
  for (const TrialResult& trial : trials) {
    if (include_method) out << trial.method << ',';
    write_trial_fields(out, trial);
  }
}

struct DuckInteractiveState {
  DuckInteractiveState() = default;
  DuckInteractiveState(const DuckInteractiveState&) = delete;
  DuckInteractiveState& operator=(const DuckInteractiveState&) = delete;
  DuckInteractiveState(DuckInteractiveState&&) = default;
  DuckInteractiveState& operator=(DuckInteractiveState&&) = default;
  ~DuckInteractiveState() {
    db.reset();
    if (!temp_directory.empty()) {
      std::error_code error;
      fs::remove_all(temp_directory, error);
    }
  }

  std::string method;
  fs::path temp_directory;
  std::unique_ptr<DuckDB> db;
  std::optional<AprilIndex> april_index;
  AprilKernelStats april_stats;
  std::vector<std::uint8_t> excluded_polygons;
  double preprocessing_seconds = 0.0;
  std::map<std::string, double> preprocessing_timings;
};

void clear_directory(const fs::path& path) {
  std::error_code error;
  fs::remove_all(path, error);
  if (error) {
    throw std::runtime_error("cannot clear interactive temporary directory: " +
                             path.string() + ": " + error.message());
  }
  fs::create_directories(path);
}

DuckInteractiveState prepare_duck_state(const std::string& method,
                                        const MethodContext& base_context,
                                        const fs::path& method_root) {
  DuckInteractiveState state;
  state.method = method;
  state.temp_directory = method_root / ".duckdb_tmp";
  clear_directory(state.temp_directory);
  auto all_started = Clock::now();
  auto started = Clock::now();
  state.db = std::make_unique<DuckDB>();
  load_spatial(*state.db);
  configure_method_database(*state.db, base_context.threads,
                            state.temp_directory);
  state.preprocessing_timings["interactive_database_setup_seconds"] =
      std::chrono::duration<double>(Clock::now() - started).count();
  started = Clock::now();
  create_cells_table(*state.db, *base_context.materialized_records);
  state.preprocessing_timings["interactive_cells_load_seconds"] =
      std::chrono::duration<double>(Clock::now() - started).count();
  state.db->exec(
      "CREATE TEMP TABLE interactive_deleted(polygon_index BIGINT PRIMARY KEY);");
  if (method == "rtree") {
    started = Clock::now();
    state.db->exec(
        "CREATE INDEX cells_geom_rtree ON cells USING RTREE (geom);");
    state.preprocessing_timings["interactive_rtree_index_build_seconds"] =
        std::chrono::duration<double>(Clock::now() - started).count();
  } else if (method == "april_duckdb") {
    AprilKernelTimings timings;
    started = Clock::now();
    state.april_index.emplace(
        AprilIndex::build(base_context, state.april_stats, timings));
    state.preprocessing_timings["interactive_april_index_build_seconds"] =
        std::chrono::duration<double>(Clock::now() - started).count();
    state.preprocessing_timings["interactive_april_geometry_adaptation_seconds"] =
        timings.geometry_adaptation_seconds;
    state.preprocessing_timings["interactive_april_polygon_approximation_seconds"] =
        timings.polygon_approximation_seconds;
    state.excluded_polygons.assign(base_context.cell_count, 0);
  }
  state.preprocessing_seconds =
      std::chrono::duration<double>(Clock::now() - all_started).count();
  return state;
}

double method_algorithm_seconds(const MethodExecution& execution) {
  double total = 0.0;
  for (const auto& [key, seconds] : execution.timing_seconds) {
    if (key.find("_cleanup_seconds") == std::string::npos &&
        key.find("_total_seconds") == std::string::npos &&
        key.find("_algorithm_seconds") == std::string::npos) {
      total += seconds;
    }
  }
  return total;
}

TrialResult run_duck_trial(const TrialSpec& spec,
                           DuckInteractiveState& state,
                           const MethodContext& base_context,
                           const GridSpec& grid,
                           const fs::path& task_dir,
                           const std::string& dataset,
                           const std::string& resolution,
                           const std::string& fingerprint,
                           size_t source_count,
                           double dataset_ratio,
                           const Options& opts,
                           double initialization_seconds) {
  bool updating = spec.operation == "updating";
  auto end_to_end_start = Clock::now();
  fs::create_directories(task_dir);
  fs::path csv_path = task_dir / "pixels.csv";
  fs::path json_path = task_dir / "pixels.json";
  fs::path info_path = task_dir / "run_info.txt";
  release_unused_memory();
  MemoryMonitor monitor;
  auto computation_start = Clock::now();
  auto dense_ready = computation_start;
  std::vector<ResultRow> result_rows;
  result_rows.reserve(grid.pixel_count);
  long long total_candidates = 0;
  double materialization_seconds = 0.0;
  if (updating) state.db->exec("BEGIN TRANSACTION;");
  double deletion_seconds = 0.0;
  MethodExecution execution;
  try {
    if (updating) {
      auto started = Clock::now();
      PreparedStatement insert(state.db->connection(),
                               "INSERT INTO interactive_deleted VALUES (?);");
      for (size_t polygon : spec.deleted_polygons) {
        insert.clear();
        insert.bind(1, static_cast<long long>(polygon));
        insert.execute();
        if (state.method == "april_duckdb") {
          state.excluded_polygons[polygon] = 1;
        }
      }
      state.db->exec(
          "DELETE FROM cells USING interactive_deleted AS d "
          "WHERE cells.polygon_index = d.polygon_index;");
      deletion_seconds =
          std::chrono::duration<double>(Clock::now() - started).count();
    }

    MethodContext context = base_context;
    context.grid = grid;
    context.cell_count = spec.remaining_count;
    ResultBatchConsumer consumer =
        [&](const std::vector<ResultRow>& rows) {
          auto started = Clock::now();
          for (const auto& row : rows) {
            if (row.pixel_id != static_cast<long long>(result_rows.size()))
              throw std::runtime_error("non-contiguous baseline output");
            result_rows.push_back(row);
            total_candidates += row.candidate_count;
          }
          auto collected = Clock::now();
          materialization_seconds += std::chrono::duration<double>(collected - started).count();
          if (result_rows.size() == grid.pixel_count) dense_ready = collected;
        };
    if (state.method == "scan") {
      execution = run_scan(*state.db, context, consumer);
    } else if (state.method == "spatial_join") {
      execution = run_spatial_join(*state.db, context, consumer, !updating);
    } else if (state.method == "rtree") {
      execution = run_rtree(*state.db, context, consumer, false);
    } else if (state.method == "april_duckdb") {
      execution = run_april_duckdb(
          *state.db, context, consumer, &*state.april_index,
          updating ? &state.excluded_polygons : nullptr, &state.april_stats,
          !updating);
    } else {
      throw std::runtime_error("unsupported interactive method: " + state.method);
    }
    if (result_rows.size() != grid.pixel_count) throw std::runtime_error("incomplete baseline grid");
  } catch (...) {
    if (updating) {
      try {
        state.db->exec("ROLLBACK;");
      } catch (...) {
      }
      for (size_t polygon : spec.deleted_polygons) {
        if (polygon < state.excluded_polygons.size()) {
          state.excluded_polygons[polygon] = 0;
        }
      }
    }
    throw;
  }

  double computation_seconds = std::chrono::duration<double>(dense_ready - computation_start).count();
  MemoryStats memory = monitor.stop();
  double serialization_seconds = 0.0;
  if (!opts.discard_output) {
    auto started = Clock::now();
    ResultWriter writer(csv_path, json_path);
    writer.append_batch(result_rows);
    writer.finish();
    serialization_seconds = std::chrono::duration<double>(Clock::now() - started).count();
  }
  std::map<std::string, std::string> validation_metadata;
  if (opts.validation_grid) {
    serialization_seconds += write_validation_grid(
        task_dir / "validation_density.f64le", result_rows, grid, validation_metadata);
  }
  double runtime =
      std::chrono::duration<double>(Clock::now() - end_to_end_start).count();
  double method_compute_seconds = method_algorithm_seconds(execution);
  double algorithm = deletion_seconds + method_compute_seconds;
  if (updating) {
    state.db->exec("ROLLBACK;");
    for (size_t polygon : spec.deleted_polygons) {
      if (polygon < state.excluded_polygons.size()) {
        state.excluded_polygons[polygon] = 0;
      }
    }
    duckdb_result count_result = state.db->query("SELECT COUNT(*) FROM cells;");
    long long restored_count = duckdb_value_int64(&count_result, 0, 0);
    duckdb_destroy_result(&count_result);
    if (restored_count != static_cast<long long>(base_context.cell_count)) {
      throw std::runtime_error(
          "interactive update failed to restore the original cell table");
    }
  }

  RunSummary summary;
  summary.method = state.method;
  summary.dataset = dataset;
  summary.records_path = fs::absolute(base_context.records_path).string();
  summary.resolution = resolution;
  summary.bbox = spec.bounds;
  summary.cell_count = spec.remaining_count;
  summary.pixel_count = grid.pixel_count;
  summary.total_candidate_pairs = total_candidates;
  summary.runtime_seconds = runtime;
  summary.algorithm_seconds = algorithm;
  summary.computation_seconds = computation_seconds;
  summary.serialization_seconds = serialization_seconds;
  summary.initialization_seconds = initialization_seconds;
  summary.duckdb_version = duckdb_library_version_string();
  summary.task_dir = fs::absolute(task_dir).string();
  summary.output_csv = opts.discard_output ? "" : fs::absolute(csv_path).string();
  summary.output_json = opts.discard_output ? "" : fs::absolute(json_path).string();
  summary.output_info = fs::absolute(info_path).string();
  summary.extra = {{"execution_mode", "streaming"},
                   {"interactive_operation", spec.operation},
                   {"interactive_trial_id", spec.trial_id},
                   {"interactive_random_seed", std::to_string(kInteractiveSeed)},
                   {"interactive_timing_scope", "request_start_to_dense_grid_ready_including_deletion_and_merge"},
                   {"interactive_reusable_state", "true"},
                   {"dataset_fingerprint", fingerprint},
                   {"dataset_ratio_percent", ratio_string(dataset_ratio)},
                   {"source_cell_count", std::to_string(source_count)},
                   {"selected_cell_count", std::to_string(base_context.cell_count)},
                   {"deleted_polygon_count", std::to_string(spec.deleted_count)},
                   {"remaining_polygon_count", std::to_string(spec.remaining_count)},
                   {"deleted_polygon_indices", json(spec.deleted_polygons).dump()}};
  for (const auto& [key, value] : execution.extra) summary.extra[key] = value;
  if (state.method == "spatial_join") {
    summary.extra["spatial_join_candidate_pairs"] =
        std::to_string(summary.total_candidate_pairs);
  }
  if (spec.viewport_scale) {
    summary.extra["viewport_scale"] = ratio_string(*spec.viewport_scale);
  }
  if (spec.pan_index) summary.extra["pan_index"] = std::to_string(*spec.pan_index);
  if (spec.delete_ratio) {
    summary.extra["delete_ratio_percent"] = std::to_string(*spec.delete_ratio);
  }
  summary.extra.insert(validation_metadata.begin(), validation_metadata.end());
  summary.extra["output_discarded"] = opts.discard_output ? "true" : "false";
  summary.extra["operation"] = spec.operation;
  summary.extra["trial_id"] = spec.trial_id;
  summary.timing_seconds = execution.timing_seconds;
  summary.timing_seconds["interactive_result_materialization_seconds"] = materialization_seconds;
  summary.timing_seconds["interactive_deletion_seconds"] = deletion_seconds;
  summary.timing_seconds["interactive_method_compute_seconds"] =
      method_compute_seconds;
  summary.timing_seconds[state.method + "_interactive_algorithm_seconds"] =
      algorithm;
  summary.timing_seconds[state.method + "_interactive_total_seconds"] = runtime;
  summary.memory = memory;
  write_run_info(info_path, summary);
  return {state.method, spec, summary};
}

std::vector<TrialSpec> make_trial_specs(const std::string& operation,
                                        const Bounds& base_bounds,
                                        size_t selected) {
  std::vector<TrialSpec> specs;
  if (operation == "zoom" || operation == "all") {
    const std::vector<std::pair<double, std::string>> zooms = {
        {0.25, "zoom_0p25"}, {0.5, "zoom_0p5"},
        {0.75, "zoom_0p75"}, {1.0, "zoom_1"}};
    for (const auto& [scale, label] : zooms) {
      specs.push_back({"zoom", label, scale, std::nullopt, std::nullopt,
                       0, selected, scaled_centered(base_bounds, scale), {}});
    }
  }
  if (operation == "panning" || operation == "all") {
    std::mt19937_64 generator(kInteractiveSeed);
    double width = (base_bounds.x_max - base_bounds.x_min) * 0.5;
    double height = (base_bounds.y_max - base_bounds.y_min) * 0.5;
    for (int trial = 1; trial <= 5; ++trial) {
      double x_min = base_bounds.x_min + stable_unit(generator) * width;
      double y_min = base_bounds.y_min + stable_unit(generator) * height;
      std::ostringstream label;
      label << "pan_" << std::setw(2) << std::setfill('0') << trial;
      specs.push_back({"panning", label.str(), std::nullopt, trial,
                       std::nullopt, 0, selected,
                       {x_min, y_min, x_min + width, y_min + height}, {}});
    }
  }
  if (operation == "updating" || operation == "all") {
    std::vector<size_t> order = stable_permutation(selected);
    for (int ratio : {10, 20, 30, 40}) {
      size_t deleted = std::max<size_t>(
          1, static_cast<size_t>(std::floor(
                 static_cast<long double>(selected) * ratio / 100.0L)));
      deleted = std::min(deleted, selected);
      std::vector<size_t> polygons(order.begin(), order.begin() + deleted);
      specs.push_back({"updating", "delete_" + std::to_string(ratio),
                       std::nullopt, std::nullopt, ratio, deleted,
                       selected - deleted, base_bounds, std::move(polygons)});
    }
  }
  return specs;
}

void write_interactive_info(
    const fs::path& path,
    const std::string& method,
    const std::string& operation,
    const std::string& dataset,
    const fs::path& records,
    const std::string& resolution,
    double dataset_ratio,
    const DatasetInfo& source,
    const DatasetInfo& active,
    const Bounds& base_bounds,
    size_t selected,
    size_t trial_count,
    double preprocessing_seconds,
    double initial_density_seconds,
    double initialization_seconds,
    const Options& opts,
    const fs::path& results_path,
    const fs::path& root,
    const std::map<std::string, double>& preprocessing_timings,
    const SledInteractiveIndex* sled_index,
    const char* argv0) {
  std::ofstream info(path);
  if (!info) throw std::runtime_error("cannot write interactive run info");
  info << std::setprecision(17)
       << "method=" << method << '\n'
       << "interactive_operation=" << operation << '\n'
       << "dataset=" << dataset << '\n'
       << "records_path=" << fs::absolute(records).string() << '\n'
       << "resolution=" << resolution << '\n'
       << "dataset_ratio_percent=" << ratio_string(dataset_ratio) << '\n'
       << "source_cell_count=" << source.cell_count << '\n'
       << "selected_cell_count=" << selected << '\n'
       << "source_dataset_fingerprint=" << source.fingerprint << '\n'
       << "dataset_fingerprint=" << active.fingerprint << '\n'
       << "base_bbox=" << json({{"x_min", base_bounds.x_min},
                                  {"y_min", base_bounds.y_min},
                                  {"x_max", base_bounds.x_max},
                                  {"y_max", base_bounds.y_max}}).dump() << '\n'
       << "random_seed=" << kInteractiveSeed << '\n'
       << "trial_count=" << trial_count << '\n'
       << "interactive_preprocessing_seconds=" << preprocessing_seconds << '\n'
       << "interactive_initial_density_seconds=" << initial_density_seconds << '\n'
       << "initialization_seconds=" << initialization_seconds << '\n'
       << "sled_update_mode=" << opts.sled_update_mode << '\n'
       << "timing_contract=v2_dense_memory\n"
       << "validation_grid=" << (opts.validation_grid ? "true" : "false") << '\n'
       << "interactive_timing_scope=request_start_to_dense_grid_ready_including_deletion_and_merge\n"
       << "interactive_results_csv=" << fs::absolute(results_path).string() << '\n'
       << "task_dir=" << fs::absolute(root).string() << '\n'
       << "source_command=" << (argv0 ? argv0 : "pgv_baselines")
       << " --interactive-operation " << operation
       << " --method " << method << '\n';
  for (const auto& [key, seconds] : preprocessing_timings) {
    info << key << '=' << seconds << '\n';
  }
  if (sled_index) {
    info << "sled_input_edges=" << sled_index->stats().input_edges << '\n'
         << "sled_nonhorizontal_edges="
         << sled_index->stats().nonhorizontal_edges << '\n'
         << "sled_unique_edges=" << sled_index->stats().unique_edges << '\n'
         << "sled_residual_edges=" << sled_index->stats().residual_edges << '\n'
         << "sled_cancelled_edges=" << sled_index->stats().cancelled_edges << '\n';
  }
}

struct InteractiveComparison {
  std::string operation;
  std::string trial_id;
  ComparisonRow comparison;
};

void write_interactive_comparisons(
    const fs::path& path,
    const std::vector<InteractiveComparison>& rows) {
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("cannot write interactive comparison: " +
                             path.string());
  }
  out << "operation,trial_id,reference_method,method,max_abs_diff,max_rel_diff,"
         "max_abs_pixel_id,reference_density,method_density,passed\n";
  out << std::setprecision(17);
  for (const auto& row : rows) {
    const ComparisonRow& comparison = row.comparison;
    out << row.operation << ',' << row.trial_id << ','
        << comparison.reference_method << ',' << comparison.method << ','
        << comparison.max_abs_diff << ',' << comparison.max_rel_diff << ',';
    if (comparison.max_abs_pixel_id >= 0) out << comparison.max_abs_pixel_id;
    out << ',' << comparison.reference_density << ','
        << comparison.method_density << ','
        << (comparison.passed ? "true" : "false") << '\n';
  }
}

}  // namespace

int run_interactive(const Options& opts,
                    const ProjectPaths& paths,
                    const char* argv0) {
  fs::path records = opts.records
                         ? *opts.records
                         : paths.repo_root / "dataset" / "processed" /
                               (opts.dataset + "_records.jsonl");
  std::string dataset = dataset_name(opts, records);
  auto initialization_start = Clock::now();
  DatasetInfo source = inspect_records(records);
  size_t selected = selected_count(source.cell_count, opts.dataset_ratio);
  DatasetInfo active = selected == source.cell_count ? source : inspect_records(records, selected);
  std::vector<CellRecord> common_records = read_records(records, selected);
  double initialization_seconds = std::chrono::duration<double>(Clock::now() - initialization_start).count();
  Bounds base_bounds = opts.bbox ? *opts.bbox : source.bounds;
  auto [x_res, y_res] = parse_resolution(opts.resolution);
  std::string run_name = opts.run_name.value_or(timestamp_name());
  fs::path out_dir = opts.out_dir.value_or(paths.pgv_root / "results");
  std::string operation = *opts.interactive_operation;
  std::vector<TrialSpec> specs = make_trial_specs(operation, base_bounds, selected);
  if (!opts.interactive_trials.empty()) {
    std::vector<TrialSpec> filtered;
    for (const auto& id : opts.interactive_trials) {
      auto found = std::find_if(specs.begin(), specs.end(), [&](const TrialSpec& spec) { return spec.trial_id == id; });
      if (found == specs.end()) throw std::runtime_error("unknown trial for selected operation: " + id);
      filtered.push_back(*found);
    }
    specs = std::move(filtered);
  }
  GridSpec base_grid = make_grid(base_bounds, x_res, y_res, std::nullopt);
  auto ri_resolution = parse_resolution(opts.ri_resolution);
  MethodContext base_context{records,
                             active.fingerprint,
                             active.bounds,
                             base_grid,
                             ri_resolution,
                             selected,
                             active.coordinate_count,
                             active.total_edge_count,
                             active.max_edges_per_record,
                             active.total_mbr_width,
                             active.total_mbr_height,
                             opts.threads,
                             opts.batch_rows,
                             ExecutionMode::Streaming,
                             {},
                             &common_records,
                             nullptr,
                             !opts.no_progress,
                             false,
                             false};

  std::vector<TrialResult> all_trials;
  std::map<std::string, fs::path> method_roots;
  std::map<std::string, double> method_preprocessing;
  for (const std::string& method : opts.methods) {
    fs::path root = out_dir / safe_path_component(
        run_name + "_" + dataset + "_" + method + "_interactive_" +
        operation + "_streaming_" + opts.resolution + "_ratio" +
        ratio_label(opts.dataset_ratio));
    fs::create_directories(root);
    method_roots[method] = fs::absolute(root);
    std::vector<TrialResult> method_trials;
    method_trials.reserve(specs.size());
    double preprocessing_seconds = 0.0;
    double initial_density_seconds = 0.0;
    std::map<std::string, double> preprocessing_timings;

    if (method == "SLED") {
      auto preprocessing_start = Clock::now();
      SledInteractiveIndex index = SledInteractiveIndex::build(
          common_records, selected, !opts.no_progress);
      preprocessing_seconds =
          std::chrono::duration<double>(Clock::now() - preprocessing_start).count();
      std::vector<double> baseline_density;
      std::vector<long long> baseline_candidates;
      if (opts.sled_update_mode == "delta" && std::any_of(specs.begin(), specs.end(), [](const TrialSpec& spec) { return spec.operation == "updating"; })) {
        auto started = Clock::now();
        baseline_density.resize(base_grid.pixel_count);
        baseline_candidates.resize(base_grid.pixel_count);
        sweep_edges(index.residual_edges(), base_grid,
                    [&](const std::vector<ResultRow>& rows) {
                      for (const ResultRow& row : rows) {
                        size_t pixel = static_cast<size_t>(row.pixel_id);
                        baseline_density[pixel] = row.density;
                        baseline_candidates[pixel] = row.candidate_count;
                      }
                    }, !opts.no_progress, "SLED interactive initial density");
        initial_density_seconds =
            std::chrono::duration<double>(Clock::now() - started).count();
      }
      for (const TrialSpec& spec : specs) {
        GridSpec grid = make_grid(spec.bounds, x_res, y_res, std::nullopt);
        TrialResult result = run_sled_trial(
            spec, index, grid,
            spec.operation == "updating" ? &baseline_density : nullptr,
            spec.operation == "updating" ? &baseline_candidates : nullptr,
            root / spec.operation / spec.trial_id, records, dataset,
            opts.resolution, active.fingerprint, source.cell_count,
            opts.dataset_ratio, !opts.no_progress, opts, initialization_seconds);
        std::cout << "interactive " << method << ' ' << spec.operation << ' '
                  << spec.trial_id << ": seconds=" << std::fixed
                  << std::setprecision(6) << result.summary.runtime_seconds
                  << ", algorithm_seconds=" << result.summary.algorithm_seconds
                  << ", task_dir=" << result.summary.task_dir << '\n';
        method_trials.push_back(std::move(result));
      }
      fs::path results_path = root / "interactive_results.csv";
      write_results(results_path, method_trials, false);
      write_interactive_info(root / "interactive_run_info.txt", method,
                             operation, dataset, records, opts.resolution,
                             opts.dataset_ratio, source, active, base_bounds,
                             selected, method_trials.size(), preprocessing_seconds,
                             initial_density_seconds, initialization_seconds, opts, results_path, root,
                             preprocessing_timings, &index, argv0);
    } else {
      DuckInteractiveState state = prepare_duck_state(method, base_context, root);
      preprocessing_seconds = state.preprocessing_seconds;
      preprocessing_timings = state.preprocessing_timings;
      for (const TrialSpec& spec : specs) {
        GridSpec grid = make_grid(spec.bounds, x_res, y_res, std::nullopt);
        TrialResult result = run_duck_trial(
            spec, state, base_context, grid,
            root / spec.operation / spec.trial_id, dataset, opts.resolution,
            active.fingerprint, source.cell_count, opts.dataset_ratio, opts, initialization_seconds);
        std::cout << "interactive " << method << ' ' << spec.operation << ' '
                  << spec.trial_id << ": seconds=" << std::fixed
                  << std::setprecision(6) << result.summary.runtime_seconds
                  << ", algorithm_seconds=" << result.summary.algorithm_seconds
                  << ", task_dir=" << result.summary.task_dir << '\n';
        method_trials.push_back(std::move(result));
      }
      fs::path results_path = root / "interactive_results.csv";
      write_results(results_path, method_trials, false);
      write_interactive_info(root / "interactive_run_info.txt", method,
                             operation, dataset, records, opts.resolution,
                             opts.dataset_ratio, source, active, base_bounds,
                             selected, method_trials.size(), preprocessing_seconds,
                             0.0, initialization_seconds, opts, results_path, root, preprocessing_timings,
                             nullptr, argv0);
    }
    method_preprocessing[method] = preprocessing_seconds;
    all_trials.insert(all_trials.end(), method_trials.begin(), method_trials.end());
  }

  bool multiple_methods = opts.methods.size() > 1;
  fs::path summary_root;
  if (multiple_methods) {
    summary_root = out_dir / safe_path_component(
        run_name + "_" + dataset + "_" + opts.method +
        "_interactive_comparison_" + operation + "_streaming_" +
        opts.resolution + "_ratio" + ratio_label(opts.dataset_ratio));
    fs::create_directories(summary_root);
    write_results(summary_root / "interactive_results.csv", all_trials, true);
    std::ofstream manifest(summary_root / "command_manifest.txt");
    if (!manifest) throw std::runtime_error("cannot write interactive command manifest");
    manifest << "interactive_operation=" << operation << '\n'
             << "methods=";
    for (size_t index = 0; index < opts.methods.size(); ++index) {
      if (index) manifest << ',';
      manifest << opts.methods[index];
    }
    manifest << '\n'
             << "dataset=" << dataset << '\n'
             << "resolution=" << opts.resolution << '\n'
             << "dataset_ratio_percent=" << ratio_string(opts.dataset_ratio) << '\n'
             << "random_seed=" << kInteractiveSeed << '\n'
             << "combined_results_csv="
             << fs::absolute(summary_root / "interactive_results.csv").string()
             << '\n';
    if (opts.compare) {
      manifest << "interactive_comparison_csv="
               << fs::absolute(summary_root / "interactive_comparison.csv").string()
               << '\n';
    }
    for (const std::string& method : opts.methods) {
      manifest << "method_" << method << "_root="
               << method_roots[method].string() << '\n'
               << "method_" << method << "_preprocessing_seconds="
               << std::setprecision(17) << method_preprocessing[method] << '\n';
    }
  } else {
    summary_root = method_roots.at(opts.methods.front());
  }

  if (opts.compare) {
    std::vector<InteractiveComparison> comparisons;
    size_t trials_per_method = specs.size();
    for (size_t trial = 0; trial < trials_per_method; ++trial) {
      const TrialResult& reference = all_trials[trial];
      for (size_t method = 0; method < opts.methods.size(); ++method) {
        const TrialResult& candidate =
            all_trials[method * trials_per_method + trial];
        ComparisonRow row = compare_csv(
            reference.method, candidate.method, reference.summary.output_csv,
            candidate.summary.output_csv);
        comparisons.push_back(
            {candidate.spec.operation, candidate.spec.trial_id, row});
      }
    }
    fs::path comparison_path = summary_root / "interactive_comparison.csv";
    write_interactive_comparisons(comparison_path, comparisons);
    std::cout << "interactive_comparison_csv="
              << fs::absolute(comparison_path).string() << '\n';
    for (const auto& row : comparisons) {
      if (!row.comparison.passed) {
        throw std::runtime_error(
            "interactive outputs differ for " + row.comparison.method + " " +
            row.operation + "/" + row.trial_id);
      }
    }
  }
  return 0;
}

}  // namespace pgv
