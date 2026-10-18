#include "sled.h"

#include "progress.h"
#include "utils.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace pgv {

namespace {

using Clock = std::chrono::steady_clock;

struct Point {
  double x = 0.0;
  double y = 0.0;
};

struct EdgeContribution {
  double x_upper = 0.0;
  double y_upper = 0.0;
  double x_lower = 0.0;
  double y_lower = 0.0;
  long double coefficient = 0.0L;
};

struct SledStats {
  size_t input_edges = 0;
  size_t nonhorizontal_edges = 0;
  size_t unique_edges = 0;
  size_t residual_edges = 0;
  size_t cancelled_edges = 0;
  size_t grid_fragments = 0;
  size_t peak_active_edges = 0;
};

bool same_point(const Point& left, const Point& right) {
  return left.x == right.x && left.y == right.y;
}

bool same_edge_key(const EdgeContribution& left, const EdgeContribution& right) {
  return left.x_upper == right.x_upper && left.y_upper == right.y_upper &&
         left.x_lower == right.x_lower && left.y_lower == right.y_lower;
}

bool edge_key_less(const EdgeContribution& left, const EdgeContribution& right) {
  if (left.x_upper != right.x_upper) {
    return left.x_upper < right.x_upper;
  }
  if (left.y_upper != right.y_upper) {
    return left.y_upper < right.y_upper;
  }
  if (left.x_lower != right.x_lower) {
    return left.x_lower < right.x_lower;
  }
  return left.y_lower < right.y_lower;
}

Point parse_point(const json& coordinate) {
  if (!coordinate.is_array() || coordinate.size() < 2 ||
      !coordinate[0].is_number() || !coordinate[1].is_number()) {
    throw std::runtime_error("SLED encountered an invalid GeoJSON coordinate");
  }
  Point point{coordinate[0].get<double>(), coordinate[1].get<double>()};
  if (!std::isfinite(point.x) || !std::isfinite(point.y)) {
    throw std::runtime_error("SLED encountered a non-finite GeoJSON coordinate");
  }
  return point;
}

std::vector<Point> corrected_ring(const json& coordinates, bool outer) {
  if (!coordinates.is_array() || coordinates.size() < 3) {
    throw std::runtime_error("SLED encountered a ring with fewer than three coordinates");
  }
  std::vector<Point> ring;
  ring.reserve(coordinates.size() + 1);
  for (const auto& coordinate : coordinates) {
    Point point = parse_point(coordinate);
    if (ring.empty() || !same_point(ring.back(), point)) {
      ring.push_back(point);
    }
  }
  if (ring.size() < 3) {
    throw std::runtime_error("SLED encountered a degenerate ring");
  }
  if (!same_point(ring.front(), ring.back())) {
    ring.push_back(ring.front());
  }
  if (ring.size() < 4) {
    throw std::runtime_error("SLED encountered a degenerate closed ring");
  }

  long double twice_area = 0.0L;
  for (size_t i = 0; i + 1 < ring.size(); ++i) {
    twice_area += static_cast<long double>(ring[i].x) * ring[i + 1].y -
                  static_cast<long double>(ring[i + 1].x) * ring[i].y;
  }
  if (twice_area == 0.0L) {
    throw std::runtime_error("SLED encountered a zero-area ring");
  }
  if ((twice_area > 0.0L) != outer) {
    std::reverse(ring.begin(), ring.end());
  }
  return ring;
}

void add_ring_edges(const json& coordinates,
                    bool outer,
                    long double weight,
                    std::vector<EdgeContribution>& edges,
                    SledStats& stats) {
  std::vector<Point> ring = corrected_ring(coordinates, outer);
  for (size_t i = 0; i + 1 < ring.size(); ++i) {
    const Point& start = ring[i];
    const Point& end = ring[i + 1];
    ++stats.input_edges;
    if (same_point(start, end) || start.y == end.y) {
      continue;
    }
    ++stats.nonhorizontal_edges;
    if (start.y > end.y) {
      edges.push_back({start.x, start.y, end.x, end.y, weight});
    } else {
      edges.push_back({end.x, end.y, start.x, start.y, -weight});
    }
  }
}

void add_polygon_edges(const json& polygon_coordinates,
                       long double weight,
                       std::vector<EdgeContribution>& edges,
                       SledStats& stats) {
  if (!polygon_coordinates.is_array() || polygon_coordinates.empty()) {
    throw std::runtime_error("SLED encountered a polygon without rings");
  }
  for (size_t ring_index = 0; ring_index < polygon_coordinates.size(); ++ring_index) {
    add_ring_edges(polygon_coordinates[ring_index], ring_index == 0, weight, edges, stats);
  }
}

double parse_weight(const json& record) {
  if (!record.contains("weight") || !record["weight"].is_object()) {
    return 0.0;
  }
  const auto& value = record["weight"].value("value", json(0));
  double parsed = 0.0;
  if (value.is_number()) {
    parsed = value.get<double>();
  } else if (value.is_string() && !value.get<std::string>().empty()) {
    parsed = std::stod(value.get<std::string>());
  }
  return std::isfinite(parsed) ? parsed : 0.0;
}

void add_geometry_edges(const json& geometry,
                        const std::string& cell_id,
                        long double weight,
                        std::vector<EdgeContribution>& edges,
                        SledStats& stats) {
  std::string type = geometry.value("type", "");
  auto coordinates = geometry.find("coordinates");
  if (coordinates == geometry.end()) {
    throw std::runtime_error("SLED geometry is missing coordinates for cell " + cell_id);
  }
  if (type == "Polygon") {
    add_polygon_edges(*coordinates, weight, edges, stats);
    return;
  }
  if (type == "MultiPolygon") {
    if (!coordinates->is_array() || coordinates->empty()) {
      throw std::runtime_error("SLED encountered an empty MultiPolygon for cell " + cell_id);
    }
    for (const auto& polygon : *coordinates) {
      add_polygon_edges(polygon, weight, edges, stats);
    }
    return;
  }
  throw std::runtime_error("SLED supports only Polygon and MultiPolygon geometries, got: " + type);
}

void stream_input_edges(const MethodContext& context,
                        std::vector<EdgeContribution>& edges,
                        SledStats& stats) {
  std::ifstream input(context.records_path);
  if (!input) {
    throw std::runtime_error("records file not found: " + context.records_path.string());
  }
  ProgressBar progress("SLED cells", context.cell_count, context.show_progress);
  std::string line;
  size_t line_no = 0;
  size_t cells = 0;
  while (std::getline(input, line)) {
    ++line_no;
    if (line.find_first_not_of(" \t\r\n") == std::string::npos) {
      continue;
    }
    if (cells >= context.cell_count) {
      break;
    }
    json record = json::parse(line);
    if (!record.contains("geometry") || !record["geometry"].is_object()) {
      throw std::runtime_error("missing GeoJSON geometry at " + context.records_path.string() +
                               ":" + std::to_string(line_no));
    }
    std::string cell_id = record.value("cell_id", std::to_string(line_no));
    add_geometry_edges(record["geometry"], cell_id,
                       static_cast<long double>(parse_weight(record)), edges, stats);
    progress.update(++cells);
  }
  progress.finish();
  if (cells != context.cell_count) {
    throw std::runtime_error("SLED record count changed while reading the dataset");
  }
}

void materialized_input_edges(const MethodContext& context,
                              std::vector<EdgeContribution>& edges,
                              SledStats& stats) {
  if (!context.materialized_records) {
    throw std::runtime_error("materialized SLED execution is missing input records");
  }
  ProgressBar progress("SLED cells", context.materialized_records->size(),
                       context.show_progress);
  size_t cells = 0;
  for (const auto& record : *context.materialized_records) {
    json geometry = json::parse(record.geometry_json);
    add_geometry_edges(geometry, record.cell_id,
                       static_cast<long double>(record.weight), edges, stats);
    progress.update(++cells);
  }
  progress.finish();
  if (cells != context.cell_count) {
    throw std::runtime_error("materialized SLED record count does not match dataset metadata");
  }
}

long double snap_near_integer(long double value) {
  long double nearest = std::round(value);
  long double tolerance = 64.0L * std::numeric_limits<long double>::epsilon() *
                          std::max(1.0L, std::abs(value));
  return std::abs(value - nearest) <= tolerance ? nearest : value;
}

long double interpolate_x(const EdgeContribution& edge, long double y) {
  long double y_upper = edge.y_upper;
  long double y_lower = edge.y_lower;
  long double t = (y_upper - y) / (y_upper - y_lower);
  return static_cast<long double>(edge.x_upper) +
         (static_cast<long double>(edge.x_lower) - edge.x_upper) * t;
}

std::string long_double_string(long double value) {
  std::ostringstream out;
  out << std::setprecision(std::numeric_limits<long double>::max_digits10) << value;
  return out.str();
}

int containing_column(long double x, const GridSpec& grid) {
  long double position = snap_near_integer(
      (x - static_cast<long double>(grid.bounds.x_min)) /
      static_cast<long double>(grid.x_step));
  int column = static_cast<int>(std::floor(position));
  if (column < 0) {
    return -1;
  }
  return column >= grid.x_res ? grid.x_res : column;
}

std::optional<std::pair<int, int>> edge_row_span(const EdgeContribution& edge,
                                                 const GridSpec& grid,
                                                 int max_requested_row) {
  long double y_min = grid.bounds.y_min;
  long double y_max = grid.bounds.y_max;
  long double y_step = grid.y_step;
  long double clipped_top = std::min(static_cast<long double>(edge.y_upper), y_max);
  long double clipped_bottom = std::max(static_cast<long double>(edge.y_lower), y_min);
  if (!(clipped_bottom < clipped_top)) {
    return std::nullopt;
  }
  long double first_position = snap_near_integer((y_max - clipped_top) / y_step);
  long double last_position = snap_near_integer((y_max - clipped_bottom) / y_step);
  int first_row = static_cast<int>(std::floor(first_position));
  int last_row = static_cast<int>(std::ceil(last_position)) - 1;
  first_row = std::clamp(first_row, 0, grid.y_res - 1);
  last_row = std::clamp(last_row, 0, grid.y_res - 1);
  if (first_row > last_row || first_row > max_requested_row) {
    return std::nullopt;
  }
  return std::pair<int, int>{first_row, std::min(last_row, max_requested_row)};
}

void accumulate_edge_fragments(const EdgeContribution& edge,
                               int row,
                               const GridSpec& grid,
                               std::vector<long double>& boundary,
                               std::vector<long double>& difference,
                               std::vector<long long>& candidate_counts,
                               SledStats& stats) {
  long double x_min = grid.bounds.x_min;
  long double x_max = grid.bounds.x_max;
  long double x_step = grid.x_step;
  long double row_top = static_cast<long double>(
      grid.bounds.y_max - static_cast<double>(row) * grid.y_step);
  long double row_bottom = static_cast<long double>(
      grid.bounds.y_max - static_cast<double>(row + 1) * grid.y_step);
  long double segment_top = std::min(static_cast<long double>(edge.y_upper), row_top);
  long double segment_bottom =
      std::max(static_cast<long double>(edge.y_lower), row_bottom);
  if (!(segment_bottom < segment_top)) {
    return;
  }

  long double x_top = interpolate_x(edge, segment_top);
  long double x_bottom = interpolate_x(edge, segment_bottom);
  std::vector<long double> split_parameters = {0.0L, 1.0L};
  if (x_top != x_bottom) {
    long double minimum_x = std::min(x_top, x_bottom);
    long double maximum_x = std::max(x_top, x_bottom);
    long double minimum_position = snap_near_integer((minimum_x - x_min) / x_step);
    long double maximum_position = snap_near_integer((maximum_x - x_min) / x_step);
    int first_line = std::max(static_cast<int>(std::floor(minimum_position)) + 1, 0);
    int last_line = std::min(static_cast<int>(std::ceil(maximum_position)) - 1,
                             grid.x_res);
    for (int line = first_line; line <= last_line; ++line) {
      long double grid_x = static_cast<long double>(
          grid.bounds.x_min + static_cast<double>(line) * grid.x_step);
      long double t = (grid_x - x_top) / (x_bottom - x_top);
      if (t > 0.0L && t < 1.0L) {
        split_parameters.push_back(t);
      }
    }
  }
  std::sort(split_parameters.begin(), split_parameters.end());
  split_parameters.erase(std::unique(split_parameters.begin(), split_parameters.end()),
                         split_parameters.end());

  long double vertical_span = segment_top - segment_bottom;
  for (size_t i = 0; i + 1 < split_parameters.size(); ++i) {
    long double t_start = split_parameters[i];
    long double t_end = split_parameters[i + 1];
    if (!(t_start < t_end)) {
      continue;
    }
    long double x_start = x_top + (x_bottom - x_top) * t_start;
    long double x_end = x_top + (x_bottom - x_top) * t_end;
    long double height = vertical_span * (t_end - t_start);
    if (!(height > 0.0L)) {
      continue;
    }
    ++stats.grid_fragments;
    long double midpoint = (x_start + x_end) * 0.5L;
    long double suffix = edge.coefficient * height * x_step;
    if (midpoint < x_min) {
      difference[0] += suffix;
      difference[static_cast<size_t>(grid.x_res)] -= suffix;
      continue;
    }
    if (midpoint >= x_max) {
      continue;
    }
    int column = containing_column(midpoint, grid);
    if (column < 0) {
      difference[0] += suffix;
      difference[static_cast<size_t>(grid.x_res)] -= suffix;
      continue;
    }
    if (column >= grid.x_res) {
      continue;
    }
    long double column_right = static_cast<long double>(
        grid.bounds.x_min + static_cast<double>(column + 1) * grid.x_step);
    long double right_area =
        ((column_right - x_start) + (column_right - x_end)) * 0.5L * height;
    boundary[static_cast<size_t>(column)] += edge.coefficient * right_area;
    difference[static_cast<size_t>(column + 1)] += suffix;
    difference[static_cast<size_t>(grid.x_res)] -= suffix;
    ++candidate_counts[static_cast<size_t>(column)];
  }
}

}  // namespace

MethodExecution run_sled(const MethodContext& context,
                         const ResultBatchConsumer& consumer) {
  if (context.grid.pixel_count == 0) {
    return {};
  }
  MethodExecution execution;
  if (context.execution_mode == ExecutionMode::Materialized) {
    execution.extra["batch_rows"] = "all";
    execution.extra["requested_batch_rows"] = "all";
    execution.extra["batch_count"] = "1";
    execution.extra["peak_batch_pixels"] = std::to_string(context.grid.pixel_count);
    execution.extra["effective_batch_pixels"] =
        std::to_string(context.grid.pixel_count);
    execution.extra["memory_strategy"] = "fully_materialized_in_memory";
  } else {
    execution.extra["batch_rows"] = std::to_string(context.batch_rows);
    execution.extra["requested_batch_rows"] =
        std::to_string(context.batch_rows);
    execution.extra["batch_count"] =
        std::to_string((context.grid.pixel_count + context.grid.x_res - 1) /
                       static_cast<size_t>(context.grid.x_res));
    size_t row_pixels = std::min(
        context.grid.pixel_count, static_cast<size_t>(context.grid.x_res));
    execution.extra["peak_batch_pixels"] = std::to_string(row_pixels);
    execution.extra["effective_batch_pixels"] = std::to_string(row_pixels);
    execution.extra["memory_strategy"] = "bounded_row_streaming";
  }
  PhaseProgress phases("SLED", 4, context.show_progress);
  SledStats stats;

  phases.step("signed boundary construction");
  auto start = Clock::now();
  std::vector<EdgeContribution> edges;
  edges.reserve(context.coordinate_count);
  if (context.materialized_records) {
    materialized_input_edges(context, edges, stats);
  } else {
    stream_input_edges(context, edges, stats);
  }
  execution.timing_seconds["sled_boundary_construction_seconds"] =
      std::chrono::duration<double>(Clock::now() - start).count();

  phases.step("sort and cancel residual edges");
  start = Clock::now();
  std::sort(edges.begin(), edges.end(), edge_key_less);
  size_t write_index = 0;
  for (size_t index = 0; index < edges.size();) {
    size_t group_end = index + 1;
    long double coefficient = edges[index].coefficient;
    while (group_end < edges.size() && same_edge_key(edges[index], edges[group_end])) {
      coefficient += edges[group_end].coefficient;
      ++group_end;
    }
    ++stats.unique_edges;
    if (coefficient == 0.0L) {
      ++stats.cancelled_edges;
    } else {
      edges[write_index] = edges[index];
      edges[write_index].coefficient = coefficient;
      ++write_index;
    }
    index = group_end;
  }
  edges.resize(write_index);
  edges.shrink_to_fit();
  stats.residual_edges = edges.size();
  execution.timing_seconds["sled_edge_cancellation_seconds"] =
      std::chrono::duration<double>(Clock::now() - start).count();

  if (edges.size() > std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error("SLED compact edge index limit exceeded");
  }
  int max_requested_row = static_cast<int>(
      (context.grid.pixel_count - 1) / static_cast<size_t>(context.grid.x_res));

  phases.step("row event schedule");
  start = Clock::now();
  std::vector<int32_t> last_rows(edges.size(), -1);
  std::vector<size_t> entry_offsets(static_cast<size_t>(max_requested_row + 2), 0);
  size_t scheduled_edges = 0;
  for (size_t edge_index = 0; edge_index < edges.size(); ++edge_index) {
    auto span = edge_row_span(edges[edge_index], context.grid, max_requested_row);
    if (!span) {
      continue;
    }
    ++entry_offsets[static_cast<size_t>(span->first + 1)];
    last_rows[edge_index] = span->second;
    ++scheduled_edges;
  }
  for (size_t row = 1; row < entry_offsets.size(); ++row) {
    entry_offsets[row] += entry_offsets[row - 1];
  }
  std::vector<uint32_t> entries(scheduled_edges);
  std::vector<size_t> cursors = entry_offsets;
  for (size_t edge_index = 0; edge_index < edges.size(); ++edge_index) {
    if (last_rows[edge_index] < 0) {
      continue;
    }
    auto span = edge_row_span(edges[edge_index], context.grid, max_requested_row);
    entries[cursors[static_cast<size_t>(span->first)]++] =
        static_cast<uint32_t>(edge_index);
  }
  std::vector<size_t>().swap(cursors);
  release_unused_memory();
  execution.timing_seconds["sled_event_schedule_seconds"] =
      std::chrono::duration<double>(Clock::now() - start).count();

  phases.step("row sweep and prefix recovery");
  std::vector<uint32_t> active_edges;
  active_edges.reserve(std::min(scheduled_edges, context.coordinate_count));
  std::vector<long double> boundary(static_cast<size_t>(context.grid.x_res), 0.0L);
  std::vector<long double> difference(static_cast<size_t>(context.grid.x_res + 1), 0.0L);
  std::vector<long long> candidate_counts(static_cast<size_t>(context.grid.x_res), 0);
  std::vector<ResultRow> output_row;
  output_row.reserve(static_cast<size_t>(context.grid.x_res));
  std::chrono::duration<double> fragment_time{0.0};
  std::chrono::duration<double> prefix_time{0.0};
  ProgressBar progress("SLED rows", static_cast<size_t>(max_requested_row + 1),
                       context.show_progress);
  const size_t total_rows = static_cast<size_t>(max_requested_row + 1);
  struct CheckpointTarget {
    std::string label;
    size_t completed_rows = 0;
  };
  std::vector<CheckpointTarget> checkpoint_targets;
  checkpoint_targets.reserve(context.progressive_checkpoints.size());
  for (double percentage : context.progressive_checkpoints) {
    size_t completed_rows = static_cast<size_t>(std::ceil(
        static_cast<long double>(total_rows) * percentage / 100.0L));
    completed_rows = std::max<size_t>(1, std::min(total_rows, completed_rows));
    std::ostringstream label;
    label << std::setprecision(15) << std::defaultfloat << percentage;
    std::string value = label.str();
    std::replace(value.begin(), value.end(), '.', 'p');
    checkpoint_targets.push_back({value + "_percent", completed_rows});
  }
  size_t next_checkpoint = 0;
  bool first_row_recorded = false;
  const double preprocessing_seconds =
      execution.timing_seconds["sled_boundary_construction_seconds"] +
      execution.timing_seconds["sled_edge_cancellation_seconds"] +
      execution.timing_seconds["sled_event_schedule_seconds"];

  for (int row = 0; row <= max_requested_row; ++row) {
    size_t entry_start = entry_offsets[static_cast<size_t>(row)];
    size_t entry_end = entry_offsets[static_cast<size_t>(row + 1)];
    active_edges.insert(active_edges.end(), entries.begin() + entry_start,
                        entries.begin() + entry_end);
    stats.peak_active_edges = std::max(stats.peak_active_edges, active_edges.size());
    std::fill(boundary.begin(), boundary.end(), 0.0L);
    std::fill(difference.begin(), difference.end(), 0.0L);
    std::fill(candidate_counts.begin(), candidate_counts.end(), 0);

    auto fragment_start = Clock::now();
    size_t retained = 0;
    for (uint32_t edge_index : active_edges) {
      accumulate_edge_fragments(edges[edge_index], row, context.grid, boundary, difference,
                                candidate_counts, stats);
      if (last_rows[edge_index] > row) {
        active_edges[retained++] = edge_index;
      }
    }
    active_edges.resize(retained);
    fragment_time += Clock::now() - fragment_start;

    auto prefix_start = Clock::now();
    output_row.clear();
    long double suffix = 0.0L;
    size_t row_start = static_cast<size_t>(row) * context.grid.x_res;
    size_t row_count = std::min(static_cast<size_t>(context.grid.x_res),
                                context.grid.pixel_count - row_start);
    for (int column = 0; column < context.grid.x_res; ++column) {
      suffix += difference[static_cast<size_t>(column)];
      if (static_cast<size_t>(column) < row_count) {
        long double density = boundary[static_cast<size_t>(column)] + suffix;
        output_row.push_back({column, row,
                              static_cast<long long>(row_start + column),
                              static_cast<double>(density),
                              candidate_counts[static_cast<size_t>(column)]});
      }
    }
    prefix_time += Clock::now() - prefix_start;
    const size_t completed_rows = static_cast<size_t>(row + 1);
    const double compute_seconds = preprocessing_seconds +
                                   fragment_time.count() +
                                   prefix_time.count();
    if (!context.progressive_checkpoints.empty() && !first_row_recorded) {
      execution.progressive_metrics.push_back(
          {"first_row", completed_rows, total_rows,
           static_cast<double>(completed_rows) / total_rows, compute_seconds});
      first_row_recorded = true;
    }
    while (next_checkpoint < checkpoint_targets.size() &&
           checkpoint_targets[next_checkpoint].completed_rows <= completed_rows) {
      execution.progressive_metrics.push_back(
          {checkpoint_targets[next_checkpoint].label, completed_rows, total_rows,
           static_cast<double>(completed_rows) / total_rows, compute_seconds});
      ++next_checkpoint;
    }
    consumer(output_row);
    progress.update(static_cast<size_t>(row + 1));
  }
  progress.finish();
  execution.timing_seconds["sled_fragment_accumulation_seconds"] = fragment_time.count();
  execution.timing_seconds["sled_prefix_recovery_seconds"] = prefix_time.count();

  execution.extra["sled_input_edges"] = std::to_string(stats.input_edges);
  execution.extra["sled_nonhorizontal_edges"] = std::to_string(stats.nonhorizontal_edges);
  execution.extra["sled_unique_edges"] = std::to_string(stats.unique_edges);
  execution.extra["sled_residual_edges"] = std::to_string(stats.residual_edges);
  execution.extra["sled_cancelled_edges"] = std::to_string(stats.cancelled_edges);
  execution.extra["sled_grid_fragments"] = std::to_string(stats.grid_fragments);
  execution.extra["sled_peak_active_edges"] = std::to_string(stats.peak_active_edges);
  execution.extra["sled_pixel_area"] = long_double_string(
      static_cast<long double>(context.grid.x_step) * context.grid.y_step);
  execution.extra["sled_normalization_mode"] = "exact_coordinate_edges";
  execution.extra["sled_accumulator"] = "row_local_boundary_plus_difference";
  execution.extra["sled_internal_precision"] = "long_double";
  execution.extra["sled_edge_storage"] = "flat_sorted_vector";
  return execution;
}

}  // namespace pgv
