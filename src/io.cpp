#include "io.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace pgv {

Bounds merge_bounds(const Bounds& left, const Bounds& right) {
  return {std::min(left.x_min, right.x_min), std::min(left.y_min, right.y_min),
          std::max(left.x_max, right.x_max), std::max(left.y_max, right.y_max)};
}

namespace {

std::string strip_carriage_return(std::string value) {
  if (!value.empty() && value.back() == '\r') {
    value.pop_back();
  }
  return value;
}

void visit_coordinates(const json& value,
                       Bounds& bounds,
                       bool& has_coordinate,
                       size_t& coordinate_count) {
  if (!value.is_array()) {
    throw std::runtime_error("geometry coordinates must be arrays");
  }
  if (value.size() >= 2 && value[0].is_number() && value[1].is_number()) {
    double x = value[0].get<double>();
    double y = value[1].get<double>();
    if (!std::isfinite(x) || !std::isfinite(y)) {
      throw std::runtime_error("geometry contains a non-finite coordinate");
    }
    if (!has_coordinate) {
      bounds = {x, y, x, y};
      has_coordinate = true;
    } else {
      bounds.x_min = std::min(bounds.x_min, x);
      bounds.y_min = std::min(bounds.y_min, y);
      bounds.x_max = std::max(bounds.x_max, x);
      bounds.y_max = std::max(bounds.y_max, y);
    }
    ++coordinate_count;
    return;
  }
  for (const auto& child : value) {
    visit_coordinates(child, bounds, has_coordinate, coordinate_count);
  }
}

bool same_coordinate(const json& left, const json& right) {
  return left.is_array() && right.is_array() && left.size() >= 2 &&
         right.size() >= 2 && left[0].get<double>() == right[0].get<double>() &&
         left[1].get<double>() == right[1].get<double>();
}

std::uint64_t ring_edge_count(const json& ring) {
  if (!ring.is_array() || ring.size() < 3) {
    throw std::runtime_error("geometry ring must contain at least three coordinates");
  }
  return same_coordinate(ring.front(), ring.back())
             ? static_cast<std::uint64_t>(ring.size() - 1)
             : static_cast<std::uint64_t>(ring.size());
}

std::uint64_t polygon_edge_count(const json& polygon) {
  if (!polygon.is_array() || polygon.empty()) {
    throw std::runtime_error("polygon must contain at least one ring");
  }
  std::uint64_t count = 0;
  for (const auto& ring : polygon) {
    count += ring_edge_count(ring);
  }
  return count;
}

std::uint64_t geometry_edge_count(const json& geometry) {
  const std::string type = geometry.at("type").get<std::string>();
  const json& coordinates = geometry.at("coordinates");
  if (type == "Polygon") {
    return polygon_edge_count(coordinates);
  }
  std::uint64_t count = 0;
  if (!coordinates.is_array() || coordinates.empty()) {
    throw std::runtime_error("MultiPolygon must contain at least one polygon");
  }
  for (const auto& polygon : coordinates) {
    count += polygon_edge_count(polygon);
  }
  return count;
}

CellRecord parse_json_record(const std::string& line,
                             const fs::path& path,
                             size_t line_number,
                             long long polygon_index,
                             Bounds* bounds = nullptr,
                             size_t* coordinate_count = nullptr,
                             std::uint64_t* edge_count = nullptr) {
  try {
    json source = json::parse(line);
    if (!source.is_object() || !source.contains("geometry") ||
        !source["geometry"].is_object()) {
      throw std::runtime_error("record requires a geometry object");
    }
    const json& geometry = source["geometry"];
    std::string type = geometry.value("type", "");
    if (type != "Polygon" && type != "MultiPolygon") {
      throw std::runtime_error("only Polygon and MultiPolygon are supported");
    }
    if (!geometry.contains("coordinates")) {
      throw std::runtime_error("geometry requires coordinates");
    }
    if (!source.contains("weight") || !source["weight"].is_object() ||
        !source["weight"].contains("value") ||
        !source["weight"]["value"].is_number()) {
      throw std::runtime_error("record requires numeric weight.value");
    }
    double weight = source["weight"]["value"].get<double>();
    if (!std::isfinite(weight)) {
      throw std::runtime_error("weight.value must be finite");
    }

    Bounds geometry_bounds;
    bool has_coordinate = false;
    size_t geometry_coordinate_count = 0;
    visit_coordinates(geometry["coordinates"], geometry_bounds,
                      has_coordinate, geometry_coordinate_count);
    if (!has_coordinate) {
      throw std::runtime_error("geometry contains no coordinates");
    }
    if (bounds) {
      *bounds = geometry_bounds;
    }
    if (coordinate_count) {
      *coordinate_count = geometry_coordinate_count;
    }
    if (edge_count) {
      *edge_count = geometry_edge_count(geometry);
    }

    std::string cell_id;
    if (source.contains("cell_id")) {
      cell_id = source["cell_id"].is_string()
                    ? source["cell_id"].get<std::string>()
                    : source["cell_id"].dump();
    } else {
      cell_id = std::to_string(polygon_index);
    }
    return {polygon_index, cell_id, weight, geometry.dump()};
  } catch (const std::exception& error) {
    throw std::runtime_error("invalid JSONL record at " + path.string() + ":" +
                             std::to_string(line_number) + ": " + error.what());
  }
}

void update_fingerprint(uint64_t& hash, const std::string& line) {
  constexpr uint64_t kFnvPrime = 1099511628211ULL;
  for (unsigned char byte : line) {
    hash ^= byte;
    hash *= kFnvPrime;
  }
  hash ^= static_cast<unsigned char>('\n');
  hash *= kFnvPrime;
}

std::string fingerprint_string(uint64_t hash) {
  std::ostringstream out;
  out << "fnv1a64:" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return out.str();
}

void write_fixture_records(const fs::path& path,
                           const std::vector<json>& records) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& record : records) {
    out << record.dump() << '\n';
  }
  if (!out) {
    throw std::runtime_error("failed to write JSONL fixture: " + path.string());
  }
}

}  // namespace

DatasetInfo inspect_records(const fs::path& path,
                            std::optional<size_t> max_records) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("records file not found: " + path.string());
  }

  Bounds total_bounds;
  bool seen_bounds = false;
  size_t cell_count = 0;
  size_t coordinate_count = 0;
  std::uint64_t total_edge_count = 0;
  std::uint64_t max_edges_per_record = 0;
  long double total_mbr_width = 0.0L;
  long double total_mbr_height = 0.0L;
  uint64_t fingerprint = 14695981039346656037ULL;
  std::string line;
  size_t line_no = 0;
  while (std::getline(input, line)) {
    ++line_no;
    line = strip_carriage_return(std::move(line));
    if (line.empty()) {
      continue;
    }
    if (max_records && cell_count >= *max_records) {
      break;
    }
    update_fingerprint(fingerprint, line);
    Bounds current;
    size_t current_coordinate_count = 0;
    std::uint64_t current_edge_count = 0;
    parse_json_record(line, path, line_no, static_cast<long long>(cell_count),
                      &current, &current_coordinate_count, &current_edge_count);
    coordinate_count += current_coordinate_count;
    total_edge_count += current_edge_count;
    max_edges_per_record = std::max(max_edges_per_record, current_edge_count);
    total_mbr_width += static_cast<long double>(current.x_max) -
                       static_cast<long double>(current.x_min);
    total_mbr_height += static_cast<long double>(current.y_max) -
                        static_cast<long double>(current.y_min);
    total_bounds = seen_bounds ? merge_bounds(total_bounds, current) : current;
    seen_bounds = true;
    ++cell_count;
  }

  if (cell_count == 0 || !seen_bounds) {
    throw std::runtime_error("records file contains no usable records: " + path.string());
  }
  return {total_bounds, cell_count, coordinate_count, total_edge_count,
          max_edges_per_record, total_mbr_width, total_mbr_height,
          fingerprint_string(fingerprint)};
}

void for_each_record(const fs::path& path,
                     const std::function<void(const CellRecord&)>& consumer,
                     std::optional<size_t> max_records) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("records file not found: " + path.string());
  }
  std::string line;
  size_t line_no = 0;
  long long polygon_index = 0;
  while (std::getline(input, line)) {
    ++line_no;
    line = strip_carriage_return(std::move(line));
    if (line.empty()) {
      continue;
    }
    if (max_records && static_cast<size_t>(polygon_index) >= *max_records) {
      break;
    }
    CellRecord cell = parse_json_record(line, path, line_no, polygon_index++);
    consumer(cell);
  }
  if (polygon_index == 0) {
    throw std::runtime_error("records file contains no usable records: " + path.string());
  }
}

std::vector<CellRecord> read_records(const fs::path& path,
                                     std::optional<size_t> max_records) {
  std::vector<CellRecord> records;
  if (max_records) {
    records.reserve(*max_records);
  }
  for_each_record(path, [&](const CellRecord& record) { records.push_back(record); },
                  max_records);
  return records;
}

GridSpec make_grid(const Bounds& bounds,
                   int x_res,
                   int y_res,
                   std::optional<int> limit_pixels) {
  if (x_res <= 0 || y_res <= 0 || !(bounds.x_min < bounds.x_max) ||
      !(bounds.y_min < bounds.y_max)) {
    throw std::runtime_error("grid requires positive bounds and resolution");
  }
  size_t full_count = static_cast<size_t>(x_res) * static_cast<size_t>(y_res);
  size_t pixel_count = limit_pixels
                           ? std::min(full_count, static_cast<size_t>(*limit_pixels))
                           : full_count;
  return {bounds,
          x_res,
          y_res,
          (bounds.x_max - bounds.x_min) / static_cast<double>(x_res),
          (bounds.y_max - bounds.y_min) / static_cast<double>(y_res),
          pixel_count};
}

std::vector<PixelRow> make_pixels(const GridSpec& grid) {
  std::vector<PixelRow> pixels;
  pixels.reserve(grid.pixel_count);
  for (size_t pixel_id = 0; pixel_id < grid.pixel_count; ++pixel_id) {
    pixels.push_back(grid.pixel_at(pixel_id));
  }
  return pixels;
}

PixelRow GridSpec::pixel_at(size_t pixel_id) const {
  if (pixel_id >= pixel_count) {
    throw std::runtime_error("pixel id is outside the configured grid");
  }
  int u = static_cast<int>(pixel_id % static_cast<size_t>(x_res));
  int v = static_cast<int>(pixel_id / static_cast<size_t>(x_res));
  double x_left = bounds.x_min + static_cast<double>(u) * x_step;
  double x_right = bounds.x_min + static_cast<double>(u + 1) * x_step;
  double y_top = bounds.y_max - static_cast<double>(v) * y_step;
  double y_bottom = bounds.y_max - static_cast<double>(v + 1) * y_step;
  return {u, v, static_cast<long long>(pixel_id), x_left, y_bottom, x_right, y_top};
}

size_t GridSpec::max_batch_pixels(int batch_rows) const {
  if (batch_rows <= 0) {
    throw std::runtime_error("batch rows must be positive");
  }
  size_t requested = static_cast<size_t>(x_res) * static_cast<size_t>(batch_rows);
  return std::max<size_t>(1, std::min(pixel_count, requested));
}

ResultWriter::ResultWriter(const fs::path& csv_path, const fs::path& json_path) {
  fs::create_directories(csv_path.parent_path());
  fs::create_directories(json_path.parent_path());
  csv_.open(csv_path);
  json_.open(json_path);
  if (!csv_ || !json_) {
    throw std::runtime_error("cannot open result output files");
  }
  csv_ << "u,v,pixel_id,density,candidate_count\n" << std::setprecision(17);
  json_ << "[\n";
}

ResultWriter::~ResultWriter() {
  if (!finished_) {
    try {
      finish();
    } catch (...) {
    }
  }
}

void ResultWriter::append(const ResultRow& row) {
  if (finished_) {
    throw std::runtime_error("cannot append to a finished result writer");
  }
  csv_ << row.u << ',' << row.v << ',' << row.pixel_id << ',' << row.density << ','
       << row.candidate_count << '\n';

  json value = {{"u", row.u},
                {"v", row.v},
                {"pixel_id", row.pixel_id},
                {"density", row.density},
                {"candidate_count", row.candidate_count}};
  if (!first_json_row_) {
    json_ << ",\n";
  }
  std::istringstream lines(value.dump(2));
  std::string line;
  bool first_line = true;
  while (std::getline(lines, line)) {
    if (!first_line) {
      json_ << '\n';
    }
    json_ << "  " << line;
    first_line = false;
  }
  first_json_row_ = false;
  ++row_count_;
  total_candidates_ += row.candidate_count;
}

void ResultWriter::append_batch(const std::vector<ResultRow>& rows) {
  for (const auto& row : rows) {
    append(row);
  }
}

void ResultWriter::finish() {
  if (finished_) {
    return;
  }
  json_ << "\n]\n";
  csv_.flush();
  json_.flush();
  if (!csv_ || !json_) {
    throw std::runtime_error("failed while writing result output files");
  }
  finished_ = true;
}

double write_validation_grid(const fs::path& path,
                              const std::vector<ResultRow>& rows,
                              const GridSpec& grid,
                              std::map<std::string, std::string>& metadata) {
  static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559,
                "validation grids require IEEE-754 binary64");
  if (rows.size() != static_cast<size_t>(grid.x_res) * grid.y_res) {
    throw std::runtime_error("validation grid requires every configured pixel");
  }
  auto started = std::chrono::steady_clock::now();
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) throw std::runtime_error("cannot open validation grid: " + path.string());
  std::array<char, 65536> bytes;
  size_t used = 0;
  for (size_t pixel = 0; pixel < rows.size(); ++pixel) {
    if (rows[pixel].pixel_id != static_cast<long long>(pixel)) {
      throw std::runtime_error("validation grid is not in contiguous pixel_id order");
    }
    std::uint64_t bits;
    std::memcpy(&bits, &rows[pixel].density, sizeof(bits));
    for (unsigned byte = 0; byte < 8; ++byte) {
      bytes[used++] = static_cast<char>((bits >> (8 * byte)) & 0xff);
    }
    if (used == bytes.size()) {
      out.write(bytes.data(), static_cast<std::streamsize>(used));
      used = 0;
    }
  }
  if (used) out.write(bytes.data(), static_cast<std::streamsize>(used));
  out.flush();
  if (!out) throw std::runtime_error("failed writing validation grid: " + path.string());
  out.close();
  if (!out) throw std::runtime_error("failed closing validation grid: " + path.string());
  double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  metadata["validation_grid_path"] = fs::absolute(path).string();
  metadata["validation_grid_shape"] = json::array({grid.y_res, grid.x_res}).dump();
  metadata["validation_grid_format"] = "ieee754_float64_le";
  metadata["validation_grid_order"] = "row_major_top_down_pixel_id";
  metadata["validation_grid_pixel_count"] = std::to_string(rows.size());
  metadata["validation_grid_bytes"] = std::to_string(rows.size() * 8);
  std::ostringstream seconds;
  seconds << std::setprecision(17) << elapsed;
  metadata["validation_grid_export_seconds"] = seconds.str();
  return elapsed;
}

json summary_json(const RunSummary& summary) {
  json data = {{"method", summary.method},
               {"dataset", summary.dataset},
               {"records_path", summary.records_path},
               {"resolution", summary.resolution},
               {"bbox",
                {{"x_min", summary.bbox.x_min},
                 {"y_min", summary.bbox.y_min},
                 {"x_max", summary.bbox.x_max},
                 {"y_max", summary.bbox.y_max}}},
               {"cell_count", summary.cell_count},
               {"input_polygon_count", summary.cell_count},
               {"pixel_count", summary.pixel_count},
               {"total_candidate_pairs", summary.total_candidate_pairs},
               {"runtime_seconds", summary.runtime_seconds},
               {"algorithm_seconds", summary.algorithm_seconds},
               {"computation_seconds", summary.computation_seconds},
               {"serialization_seconds", summary.serialization_seconds},
               {"export_seconds", summary.serialization_seconds},
               {"initialization_seconds", summary.initialization_seconds},
               {"timing_contract", "v2_dense_memory"},
               {"common_input_state", "CellRecord_geometry_JSON_in_memory"},
               {"result_state", "complete_dense_grid_in_memory"},
               {"duckdb_version", summary.duckdb_version},
               {"task_dir", summary.task_dir},
               {"output_csv", summary.output_csv},
               {"output_json", summary.output_json},
               {"output_info", summary.output_info},
               {"command_manifest", summary.command_manifest}};
  for (const auto& [key, value] : summary.extra) {
    data[key] = value;
  }
  return data;
}

void write_run_info(const fs::path& path, const RunSummary& summary) {
  fs::create_directories(path.parent_path());
  json data = summary_json(summary);
  std::ofstream out(path);
  for (auto it = data.begin(); it != data.end(); ++it) {
    out << it.key() << '=';
    if (it->is_object() || it->is_array()) {
      out << it->dump();
    } else if (it->is_string()) {
      out << it->get<std::string>();
    } else {
      out << it->dump();
    }
    out << '\n';
  }
  out << "\n--- timing_breakdown ---\n";
  for (const auto& [key, seconds] : summary.timing_seconds) {
    out << "timing_" << key << '=' << std::setprecision(17) << seconds << '\n';
  }
  out << "timing_algorithm_seconds=" << std::setprecision(17)
      << summary.algorithm_seconds << '\n';
  out << "timing_total_seconds=" << std::setprecision(17) << summary.runtime_seconds << '\n';
  out << "\n--- memory_usage ---\n";
  out << "memory_rss_at_method_start_bytes="
      << summary.memory.rss_at_start_bytes << '\n';
  out << "memory_peak_rss_bytes=" << summary.memory.peak_rss_bytes << '\n';
  out << "memory_peak_rss_mib=" << std::setprecision(17)
      << static_cast<long double>(summary.memory.peak_rss_bytes) /
             (1024.0L * 1024.0L)
      << '\n';
  out << "memory_peak_rss_delta_bytes="
      << summary.memory.peak_rss_delta_bytes << '\n';
  out << "memory_rss_at_method_end_bytes="
      << summary.memory.rss_at_end_bytes << '\n';
  out << "memory_sampling_interval_ms="
      << summary.memory.sampling_interval_ms << '\n';
  out << "memory_measurement_scope=method_setup_algorithm_cleanup\n";
  out << "memory_measurement_source=" << summary.memory.source << '\n';
}

void write_progressive_metrics(
    const fs::path& path,
    const std::vector<ProgressiveMetric>& metrics) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  if (!out) {
    throw std::runtime_error("cannot write progressive metrics: " + path.string());
  }
  out << "checkpoint_label,completed_rows,total_rows,completion_fraction,compute_seconds\n";
  out << std::setprecision(17);
  for (const auto& metric : metrics) {
    out << metric.checkpoint_label << ',' << metric.completed_rows << ','
        << metric.total_rows << ',' << metric.completion_fraction << ','
        << metric.compute_seconds << '\n';
  }
}

std::map<long long, double> read_density_csv(const fs::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot read CSV: " + path.string());
  }
  std::map<long long, double> values;
  std::string line;
  std::getline(in, line);
  while (std::getline(in, line)) {
    std::stringstream src(line);
    std::string field;
    std::vector<std::string> fields;
    while (std::getline(src, field, ',')) {
      fields.push_back(field);
    }
    if (fields.size() >= 4) {
      values[std::stoll(fields[2])] = std::stod(fields[3]);
    }
  }
  return values;
}

ComparisonRow compare_csv(const std::string& reference_method,
                          const std::string& method,
                          const fs::path& left,
                          const fs::path& right) {
  std::ifstream left_input(left);
  std::ifstream right_input(right);
  if (!left_input || !right_input) {
    throw std::runtime_error("cannot read comparison CSV files");
  }
  std::string left_line;
  std::string right_line;
  std::getline(left_input, left_line);
  std::getline(right_input, right_line);

  auto parse_row = [](const std::string& line) {
    std::stringstream src(line);
    std::string field;
    std::vector<std::string> fields;
    while (std::getline(src, field, ',')) {
      fields.push_back(field);
    }
    if (fields.size() != 5) {
      throw std::runtime_error("invalid result CSV row: " + line);
    }
    return std::pair<long long, double>{std::stoll(fields[2]), std::stod(fields[3])};
  };

  ComparisonRow row;
  row.reference_method = reference_method;
  row.method = method;
  while (true) {
    bool has_left = static_cast<bool>(std::getline(left_input, left_line));
    bool has_right = static_cast<bool>(std::getline(right_input, right_line));
    if (has_left != has_right) {
      throw std::runtime_error("output pixel sets differ between " + left.string() + " and " +
                               right.string());
    }
    if (!has_left) {
      break;
    }
    auto [left_id, left_value] = parse_row(left_line);
    auto [right_id, right_value] = parse_row(right_line);
    if (left_id != right_id) {
      throw std::runtime_error("output pixel order differs at pixel_id " + std::to_string(left_id));
    }
    double diff = std::abs(left_value - right_value);
    double scale = std::max({std::abs(left_value), std::abs(right_value), 1.0});
    double rel = diff / scale;
    if (diff > row.max_abs_diff) {
      row.max_abs_diff = diff;
      row.max_rel_diff = rel;
      row.max_abs_pixel_id = left_id;
      row.reference_density = left_value;
      row.method_density = right_value;
    }
  }
  row.passed = row.max_abs_diff <= kCompareTolerance || row.max_rel_diff <= kCompareRelativeTolerance;
  return row;
}

void write_comparison_csv(const fs::path& path, const std::vector<ComparisonRow>& rows) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  out << "reference_method,method,max_abs_diff,max_rel_diff,max_abs_pixel_id,reference_density,method_density,passed\n";
  out << std::setprecision(17);
  for (const auto& row : rows) {
    out << row.reference_method << ',' << row.method << ',' << row.max_abs_diff << ',' << row.max_rel_diff << ',';
    if (row.max_abs_pixel_id >= 0) {
      out << row.max_abs_pixel_id;
    }
    out << ',' << row.reference_density << ',' << row.method_density << ',' << (row.passed ? "true" : "false")
        << '\n';
  }
}

std::pair<fs::path, fs::path> output_paths(const fs::path& command_task_dir,
                                           const std::string& method,
                                           bool multi_method) {
  fs::path task_dir = multi_method ? command_task_dir / method : command_task_dir;
  return {task_dir, task_dir / "pixels.csv"};
}

void write_command_manifest(const fs::path& path,
                            const std::map<std::string, std::string>& info,
                            const std::vector<std::string>& methods,
                            const std::vector<RunSummary>& summaries) {
  fs::create_directories(path.parent_path());
  std::ofstream out(path);
  for (const auto& [key, value] : info) {
    out << key << '=' << value << '\n';
  }
  out << "methods=" << json(methods).dump() << '\n';
  for (const auto& summary : summaries) {
    out << "method_task_dir." << summary.method << '=' << summary.task_dir << '\n';
    out << "method_csv." << summary.method << '=' << summary.output_csv << '\n';
    out << "method_json." << summary.method << '=' << summary.output_json << '\n';
    out << "method_info." << summary.method << '=' << summary.output_info << '\n';
  }
}

void write_self_test_records(const fs::path& path) {
  json a = {{"dataset", "self_test"},
            {"cell_id", "a"},
            {"geometry",
             {{"type", "Polygon"},
              {"coordinates",
               json::array({json::array({json::array({0, 0}), json::array({2, 0}), json::array({2, 2}),
                                          json::array({0, 2}), json::array({0, 0})})})}}},
            {"weight", {{"value", 2.0}}}};
  json b = {{"dataset", "self_test"},
            {"cell_id", "b"},
            {"geometry",
             {{"type", "Polygon"},
              {"coordinates",
               json::array({json::array({json::array({1, 1}), json::array({3, 1}), json::array({3, 3}),
                                          json::array({1, 3}), json::array({1, 1})})})}}},
            {"weight", {{"value", 3.0}}}};
  write_fixture_records(path, {a, b});
}

void write_spatial_join_stats_test_records(const fs::path& path) {
  json polygon_with_hole = {
      {"dataset", "spatial_join_stats_test"},
      {"cell_id", "polygon_with_hole"},
      {"geometry",
       {{"type", "Polygon"},
        {"coordinates",
         json::array({
             json::array({json::array({0, 0}), json::array({2, 0}),
                          json::array({2, 2}), json::array({0, 2}),
                          json::array({0, 0})}),
             json::array({json::array({0.5, 0.5}), json::array({1.5, 0.5}),
                          json::array({1.5, 1.5}), json::array({0.5, 1.5}),
                          json::array({0.5, 0.5})})})}}},
      {"weight", {{"value", 1.0}}}};
  json multipolygon = {
      {"dataset", "spatial_join_stats_test"},
      {"cell_id", "two_components"},
      {"geometry",
       {{"type", "MultiPolygon"},
        {"coordinates",
         json::array({
             json::array({json::array({json::array({2, 0}), json::array({3, 0}),
                                       json::array({3, 1}), json::array({2, 1}),
                                       json::array({2, 0})})}),
             json::array({json::array({json::array({3, 3}), json::array({4, 3}),
                                       json::array({4, 4}), json::array({3, 4}),
                                       json::array({3, 3})})})})}}},
      {"weight", {{"value", 2.0}}}};
  write_fixture_records(path, {polygon_with_hole, multipolygon});
}

void write_april_duplicate_id_test_records(const fs::path& path) {
  json left = {
      {"dataset", "april_duplicate_id_test"},
      {"cell_id", "duplicate"},
      {"geometry",
       {{"type", "Polygon"},
        {"coordinates",
         json::array({json::array({json::array({0, 0}), json::array({1, 0}),
                                  json::array({1, 1}), json::array({0, 1}),
                                  json::array({0, 0})})})}}},
      {"weight", {{"value", 1.0}}}};
  json right = {
      {"dataset", "april_duplicate_id_test"},
      {"cell_id", "duplicate"},
      {"geometry",
       {{"type", "Polygon"},
        {"coordinates",
         json::array({json::array({json::array({1, 0}), json::array({2, 0}),
                                  json::array({2, 1}), json::array({1, 1}),
                                  json::array({1, 0})})})}}},
      {"weight", {{"value", 2.0}}}};
  write_fixture_records(path, {left, right});
}

void write_sled_geometry_test_records(const fs::path& path) {
  json aligned = {{"dataset", "sled_geometry_test"},
                  {"cell_id", "aligned"},
                  {"geometry",
                   {{"type", "Polygon"},
                    {"coordinates",
                     json::array({json::array({json::array({0, 0}), json::array({2, 0}),
                                                json::array({2, 2}), json::array({0, 2}),
                                                json::array({0, 0})})})}}},
                  {"weight", {{"value", 1.0}}}};
  json slanted = {{"dataset", "sled_geometry_test"},
                  {"cell_id", "slanted"},
                  {"geometry",
                   {{"type", "Polygon"},
                    {"coordinates",
                     json::array({json::array({json::array({0.25, 0.25}), json::array({3.75, 0.75}),
                                                json::array({2.25, 3.75}),
                                                json::array({0.25, 0.25})})})}}},
                  {"weight", {{"value", 2.0}}}};
  json hole = {{"dataset", "sled_geometry_test"},
               {"cell_id", "hole"},
               {"geometry",
                {{"type", "Polygon"},
                 {"coordinates",
                  json::array({json::array({json::array({0, 0}), json::array({4, 0}),
                                             json::array({4, 4}), json::array({0, 4}),
                                             json::array({0, 0})}),
                               json::array({json::array({1, 1}), json::array({3, 1}),
                                             json::array({3, 3}), json::array({1, 3}),
                                             json::array({1, 1})})})}}},
               {"weight", {{"value", 0.5}}}};
  json multipolygon = {{"dataset", "sled_geometry_test"},
                       {"cell_id", "multipolygon_negative"},
                       {"geometry",
                        {{"type", "MultiPolygon"},
                         {"coordinates",
                          json::array({json::array({json::array({json::array({0, 3}), json::array({1, 3}),
                                                               json::array({1, 4}), json::array({0, 4}),
                                                               json::array({0, 3})})}),
                                       json::array({json::array({json::array({3, 0}), json::array({4, 0}),
                                                               json::array({4, 1}), json::array({3, 1}),
                                                               json::array({3, 0})})})})}}},
                       {"weight", {{"value", -0.75}}}};
  json zero_weight = {{"dataset", "sled_geometry_test"},
                      {"cell_id", "zero_weight"},
                      {"geometry",
                       {{"type", "Polygon"},
                        {"coordinates",
                         json::array({json::array({json::array({1.25, 1.25}), json::array({2.75, 1.25}),
                                                    json::array({2.75, 2.75}), json::array({1.25, 2.75}),
                                                    json::array({1.25, 1.25})})})}}},
                      {"weight", {{"value", 0.0}}}};

  write_fixture_records(path, {aligned, slanted, hole, multipolygon, zero_weight});
}

void write_sled_shared_boundary_test_records(const fs::path& path,
                                             double left_weight,
                                             double right_weight) {
  json left = {{"dataset", "sled_shared_boundary_test"},
               {"cell_id", "left"},
               {"geometry",
                {{"type", "Polygon"},
                 {"coordinates",
                  json::array({json::array({json::array({0, 0}), json::array({1, 0}),
                                             json::array({1, 1}), json::array({0, 1}),
                                             json::array({0, 0})})})}}},
               {"weight", {{"value", left_weight}}}};
  json right = {{"dataset", "sled_shared_boundary_test"},
                {"cell_id", "right"},
                {"geometry",
                 {{"type", "Polygon"},
                  {"coordinates",
                   json::array({json::array({json::array({1, 0}), json::array({2, 0}),
                                              json::array({2, 1}), json::array({1, 1}),
                                              json::array({1, 0})})})}}},
                {"weight", {{"value", right_weight}}}};
  write_fixture_records(path, {left, right});
}

void write_sled_bbox_test_records(const fs::path& path) {
  json left_outside = {{"dataset", "sled_bbox_test"},
                       {"cell_id", "left_outside"},
                       {"geometry",
                        {{"type", "Polygon"},
                         {"coordinates",
                          json::array({json::array({json::array({-2, -1}), json::array({3, -1}),
                                                     json::array({3, 5}), json::array({-2, 5}),
                                                     json::array({-2, -1})})})}}},
                       {"weight", {{"value", 2.0}}}};
  json right_outside = {{"dataset", "sled_bbox_test"},
                        {"cell_id", "right_outside"},
                        {"geometry",
                         {{"type", "Polygon"},
                          {"coordinates",
                           json::array({json::array({json::array({3.5, 0}), json::array({5, 0}),
                                                      json::array({5, 4}), json::array({3.5, 4}),
                                                      json::array({3.5, 0})})})}}},
                        {"weight", {{"value", -1.0}}}};
  write_fixture_records(path, {left_outside, right_outside});
}

}  // namespace pgv
