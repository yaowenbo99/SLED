#include "runner.h"

#include "duckdb.h"
#include "io.h"
#include "memory.h"
#include "methods.h"
#include "sled.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace pgv {

namespace {

std::vector<std::string> all_methods() {
  return {"scan", "spatial_join", "rtree", "april_duckdb", "SLED"};
}

std::string join_method_argument(const std::vector<std::string>& methods) {
  std::ostringstream out;
  for (size_t i = 0; i < methods.size(); ++i) {
    if (i) {
      out << ',';
    }
    out << methods[i];
  }
  return out.str();
}

std::string format_dataset_ratio(double ratio) {
  std::ostringstream out;
  out << std::setprecision(15) << std::defaultfloat << ratio;
  return out.str();
}

std::string format_percentage(double percentage) {
  std::ostringstream out;
  out << std::setprecision(15) << std::defaultfloat << percentage;
  return out.str();
}

std::string join_progressive_checkpoints(
    const std::vector<double>& checkpoints) {
  std::ostringstream out;
  for (size_t i = 0; i < checkpoints.size(); ++i) {
    if (i) {
      out << ',';
    }
    out << format_percentage(checkpoints[i]);
  }
  return out.str();
}

std::string dataset_ratio_path_label(double ratio) {
  std::string label = format_dataset_ratio(ratio);
  std::replace(label.begin(), label.end(), '.', 'p');
  std::replace(label.begin(), label.end(), '+', 'p');
  std::replace(label.begin(), label.end(), '-', 'm');
  return "ratio" + label;
}

size_t selected_record_count(size_t source_count, double ratio) {
  if (ratio >= 100.0) {
    return source_count;
  }
  long double scaled = static_cast<long double>(source_count) *
                       static_cast<long double>(ratio) / 100.0L;
  size_t selected = static_cast<size_t>(std::floor(scaled));
  return std::max<size_t>(1, std::min(source_count, selected));
}

void print_comparison_table(const std::vector<ComparisonRow>& rows, const fs::path& output_path) {
  std::cout << "comparison_csv=" << fs::absolute(output_path).string() << "\n";
  std::cout << "reference_method,method,max_abs_diff,max_rel_diff,max_abs_pixel_id,reference_density,method_density,passed\n";
  std::cout << std::setprecision(12);
  for (const auto& row : rows) {
    std::cout << row.reference_method << ',' << row.method << ',' << row.max_abs_diff << ',' << row.max_rel_diff
              << ',';
    if (row.max_abs_pixel_id >= 0) {
      std::cout << row.max_abs_pixel_id;
    }
    std::cout << ',' << row.reference_density << ',' << row.method_density << ','
              << (row.passed ? "true" : "false") << "\n";
  }
}

void verify_scan_candidate_counts(const RunSummary& summary) {
  std::ifstream input(summary.output_csv);
  if (!input) {
    throw std::runtime_error("cannot read scan output: " + summary.output_csv);
  }
  std::string line;
  std::getline(input, line);
  size_t rows = 0;
  while (std::getline(input, line)) {
    size_t separator = line.rfind(',');
    if (separator == std::string::npos) {
      throw std::runtime_error("invalid scan CSV row: " + line);
    }
    long long candidates = std::stoll(line.substr(separator + 1));
    if (candidates != static_cast<long long>(summary.cell_count)) {
      throw std::runtime_error("scan candidate count does not equal the polygon count");
    }
    ++rows;
  }
  if (rows != summary.pixel_count) {
    throw std::runtime_error("scan candidate check found an unexpected pixel count");
  }
}

long long sum_csv_candidate_counts(const fs::path& path) {
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot read result CSV: " + path.string());
  }
  std::string line;
  std::getline(input, line);
  long long total = 0;
  while (std::getline(input, line)) {
    size_t separator = line.rfind(',');
    if (separator == std::string::npos) {
      throw std::runtime_error("invalid result CSV row: " + line);
    }
    total += std::stoll(line.substr(separator + 1));
  }
  return total;
}

class ScopedTempDirectory {
 public:
  explicit ScopedTempDirectory(fs::path path) : path_(std::move(path)) {
    std::error_code error;
    fs::remove_all(path_, error);
    if (error) {
      throw std::runtime_error("cannot clear DuckDB temporary directory: " +
                               path_.string() + ": " + error.message());
    }
    fs::create_directories(path_);
  }

  ScopedTempDirectory(const ScopedTempDirectory&) = delete;
  ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

  ~ScopedTempDirectory() {
    std::error_code error;
    fs::remove_all(path_, error);
  }

  const fs::path& path() const { return path_; }

  void cleanup() {
    if (cleaned_) {
      return;
    }
    std::error_code error;
    fs::remove_all(path_, error);
    if (error) {
      throw std::runtime_error("cannot remove DuckDB temporary directory: " +
                               path_.string() + ": " + error.message());
    }
    cleaned_ = true;
  }

 private:
  fs::path path_;
  bool cleaned_ = false;
};

}  // namespace

RunSummary run_method(const std::string& method,
                      const Options& opts,
                      const MethodContext& context,
                      const std::string& dataset,
                      const fs::path& command_task_dir,
                      bool multi_method,
                      const fs::path& command_manifest,
                      const std::map<std::string, std::string>& common_extra) {
  auto end_to_end_start = std::chrono::steady_clock::now();
  auto [task_dir, output_csv] = output_paths(command_task_dir, method, multi_method);
  fs::path output_json = task_dir / "pixels.json";
  fs::path output_info = task_dir / "run_info.txt";
  fs::create_directories(task_dir);
  release_unused_memory();
  MemoryMonitor memory_monitor;
  MethodExecution execution;
  MethodContext method_context = context;
  if (!method_context.materialized_records) {
    throw std::runtime_error("v2 timing requires common in-memory input records");
  }
  // Streaming is an internal batching policy; every benchmark materializes its
  // complete output before the computation interval ends.
  std::map<size_t, double> row_ready_times;
  std::map<std::string, size_t> snapshot_targets;
  std::map<std::string, std::vector<ResultRow>> snapshots;
  if (!opts.progressive_checkpoints.empty()) {
    snapshot_targets["first_row"] = 1;
    size_t total_rows = (context.grid.pixel_count + context.grid.x_res - 1) / context.grid.x_res;
    for (double percentage : opts.progressive_checkpoints) {
      std::string label = format_percentage(percentage);
      std::replace(label.begin(), label.end(), '.', 'p');
      snapshot_targets[label + "_percent"] = std::max<size_t>(1, static_cast<size_t>(
          std::ceil(static_cast<long double>(total_rows) * percentage / 100.0L)));
    }
  }
  auto computation_start = std::chrono::steady_clock::now();
  auto dense_ready = computation_start;
  std::vector<ResultRow> materialized_results;
  materialized_results.reserve(context.grid.pixel_count);
  long long total_candidates = 0;
  double result_collection_seconds = 0.0;
  ResultBatchConsumer consumer = [&](const std::vector<ResultRow>& rows) {
    auto collection_start = std::chrono::steady_clock::now();
    for (const auto& row : rows) {
      if (row.pixel_id != static_cast<long long>(materialized_results.size())) {
        throw std::runtime_error("dense output must contain each pixel exactly once in order");
      }
      materialized_results.push_back(row);
      total_candidates += row.candidate_count;
    }
    auto collected = std::chrono::steady_clock::now();
    result_collection_seconds += std::chrono::duration<double>(collected - collection_start).count();
    if (materialized_results.size() == context.grid.pixel_count) dense_ready = collected;
    if (!snapshot_targets.empty()) {
      size_t completed_rows = (materialized_results.size() + context.grid.x_res - 1) / context.grid.x_res;
      row_ready_times[completed_rows] = std::chrono::duration<double>(collected - computation_start).count();
      if (opts.progressive_snapshots) {
        for (const auto& [label, target] : snapshot_targets) {
          if (target == completed_rows) snapshots.emplace(label, materialized_results);
        }
      }
    }
  };
  std::vector<PixelRow> materialized_pixels;
  if (method_context.execution_mode == ExecutionMode::Materialized) {
    auto materialization_start = std::chrono::steady_clock::now();
    materialized_pixels = make_pixels(method_context.grid);
    method_context.materialized_pixels = &materialized_pixels;
    execution.timing_seconds["method_input_materialization_seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - materialization_start).count();
  }
  double algorithm_elapsed = 0.0;
  if (method == "SLED") {
    MethodExecution method_execution = run_sled(method_context, consumer);
    for (const auto& [key, seconds] : method_execution.timing_seconds) {
      (void)key;
      algorithm_elapsed += seconds;
    }
    execution.extra = std::move(method_execution.extra);
    execution.progressive_metrics =
        std::move(method_execution.progressive_metrics);
    execution.timing_seconds.insert(method_execution.timing_seconds.begin(),
                                    method_execution.timing_seconds.end());
    auto release_start = std::chrono::steady_clock::now();
    release_unused_memory();
    execution.timing_seconds["sled_memory_release_seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - release_start).count();
  } else {
    ScopedTempDirectory temp_directory(task_dir / ".duckdb_tmp");
    auto setup_start = std::chrono::steady_clock::now();
    auto db = std::make_unique<DuckDB>();
    load_spatial(*db);
    configure_method_database(*db, context.threads, temp_directory.path());
    execution.timing_seconds["method_duckdb_setup_seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - setup_start).count();

    auto load_start = std::chrono::steady_clock::now();
    create_cells_table(*db, *method_context.materialized_records);
    execution.timing_seconds["method_cells_load_seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - load_start).count();

    MethodExecution method_execution;
    if (method == "scan") {
      method_execution = run_scan(*db, method_context, consumer);
    } else if (method == "spatial_join") {
      method_execution = run_spatial_join(*db, method_context, consumer);
    } else if (method == "rtree") {
      method_execution = run_rtree(*db, method_context, consumer);
    } else if (method == "april_duckdb") {
      method_execution = run_april_duckdb(*db, method_context, consumer);
    } else {
      throw std::runtime_error("unsupported method: " + method);
    }
    for (const auto& [key, seconds] : method_execution.timing_seconds) {
      if (key.find("_cleanup_seconds") == std::string::npos) {
        algorithm_elapsed += seconds;
      }
    }
    execution.extra = std::move(method_execution.extra);
    execution.timing_seconds.insert(method_execution.timing_seconds.begin(),
                                    method_execution.timing_seconds.end());
    auto release_start = std::chrono::steady_clock::now();
    db.reset();
    temp_directory.cleanup();
    release_unused_memory();
    execution.timing_seconds["method_database_release_seconds"] =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - release_start).count();
    execution.extra["duckdb_spill_directory"] = "method_local_temporary";
    execution.extra["duckdb_preserve_insertion_order"] = "false";
    execution.extra["duckdb_memory_limit"] = "default";
  }
  if (materialized_results.size() != context.grid.pixel_count) {
    throw std::runtime_error(method + " did not emit the complete dense grid");
  }
  double computation_elapsed = std::chrono::duration<double>(dense_ready - computation_start).count();
  MemoryStats memory_stats = memory_monitor.stop();
  double output_finalization_seconds = 0.0;
  if (!opts.discard_output) {
    auto output_finalization_start = std::chrono::steady_clock::now();
    ResultWriter writer(output_csv, output_json);
    writer.append_batch(materialized_results);
    writer.finish();
    for (const auto& [label, rows] : snapshots) {
      fs::path snapshot_csv = task_dir / ("progressive_" + label + ".csv");
      fs::path snapshot_json = task_dir / ("progressive_" + label + ".json");
      ResultWriter snapshot_writer(snapshot_csv, snapshot_json);
      snapshot_writer.append_batch(rows);
      snapshot_writer.finish();
      execution.extra["progressive_snapshot_" + label + "_csv"] = fs::absolute(snapshot_csv).string();
    }
    output_finalization_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - output_finalization_start).count();
  }

  if (opts.validation_grid) {
    output_finalization_seconds += write_validation_grid(
        task_dir / "validation_density.f64le", materialized_results,
        context.grid, execution.extra);
  }

  double end_to_end_elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                    end_to_end_start)
          .count();
  if (!execution.progressive_metrics.empty()) {
    for (auto& metric : execution.progressive_metrics) metric.compute_seconds = row_ready_times.at(metric.completed_rows);
    execution.extra["progressive_snapshot_source"] = opts.progressive_snapshots
        ? "live_row_consumer_copies_before_later_rows_are_computed" : "disabled";
    execution.extra["progressive_snapshot_copy_overhead_included"] = opts.progressive_snapshots ? "true" : "false";
    fs::path progressive_path = task_dir / "progressive_metrics.csv";
    write_progressive_metrics(progressive_path, execution.progressive_metrics);
    execution.extra["progressive_metrics_csv"] =
        fs::absolute(progressive_path).string();
    execution.extra["progressive_timing_scope"] =
        "continuous_wall_clock_common_input_ready_to_completed_rows_in_memory";
    for (const auto& metric : execution.progressive_metrics) {
      std::ostringstream seconds;
      seconds << std::setprecision(17) << metric.compute_seconds;
      execution.extra["progressive_time_to_" + metric.checkpoint_label +
                      "_seconds"] = seconds.str();
      execution.extra["progressive_completed_rows_" +
                      metric.checkpoint_label] =
          std::to_string(metric.completed_rows);
    }
  }
  execution.timing_seconds["method_result_collection_seconds"] =
      result_collection_seconds;
  execution.timing_seconds["method_result_finalization_seconds"] =
      output_finalization_seconds;
  std::string algorithm_key =
      method == "SLED" ? "sled_algorithm_seconds"
                       : method + "_algorithm_seconds";
  std::string total_key = method == "SLED" ? "sled_total_seconds" : method + "_total_seconds";
  execution.timing_seconds[algorithm_key] = algorithm_elapsed;
  execution.timing_seconds[total_key] = end_to_end_elapsed;

  RunSummary summary;
  summary.method = method;
  summary.dataset = dataset;
  summary.records_path = fs::absolute(context.records_path).string();
  summary.resolution = opts.resolution;
  summary.bbox = context.grid.bounds;
  summary.cell_count = context.cell_count;
  summary.pixel_count = context.grid.pixel_count;
  summary.total_candidate_pairs = total_candidates;
  summary.runtime_seconds = end_to_end_elapsed;
  summary.algorithm_seconds = algorithm_elapsed;
  summary.computation_seconds = computation_elapsed;
  summary.serialization_seconds = output_finalization_seconds;
  summary.initialization_seconds = std::stod(common_extra.at("initialization_seconds"));
  summary.duckdb_version = duckdb_library_version_string();
  summary.task_dir = fs::absolute(task_dir).string();
  summary.output_csv = opts.discard_output ? "" : fs::absolute(output_csv).string();
  summary.output_json = opts.discard_output ? "" : fs::absolute(output_json).string();
  summary.output_info = fs::absolute(output_info).string();
  summary.command_manifest = multi_method ? fs::absolute(command_manifest).string() : "";
  summary.extra = common_extra;
  for (const auto& [key, value] : execution.extra) {
    summary.extra[key] = value;
  }
  summary.extra["output_discarded"] = opts.discard_output ? "true" : "false";
  summary.extra["memory_strategy"] = "common_input_and_dense_output_with_batched_computation";
  summary.extra["E_total"] = std::to_string(context.total_edge_count);
  summary.extra["computation_timing_scope"] = "common_input_ready_to_dense_grid_ready_including_method_preprocessing";
  if (method == "spatial_join") {
    std::ostringstream average_edges;
    average_edges << std::setprecision(17)
                  << static_cast<double>(context.total_edge_count) /
                         static_cast<double>(context.cell_count);
    long double polygon_count = static_cast<long double>(context.cell_count);
    long double average_mbr_width = context.total_mbr_width / polygon_count;
    long double average_mbr_height = context.total_mbr_height / polygon_count;
    long double grid_width = static_cast<long double>(context.grid.bounds.x_max) -
                             static_cast<long double>(context.grid.bounds.x_min);
    long double grid_height = static_cast<long double>(context.grid.bounds.y_max) -
                              static_cast<long double>(context.grid.bounds.y_min);
    long double covered_columns = std::ceil(
        average_mbr_width * static_cast<long double>(context.grid.x_res) /
        grid_width);
    long double covered_rows = std::ceil(
        average_mbr_height * static_cast<long double>(context.grid.y_res) /
        grid_height);
    long double candidate_estimate =
        polygon_count * covered_columns * covered_rows;
    std::ostringstream average_width;
    std::ostringstream average_height;
    std::ostringstream estimate;
    average_width << std::setprecision(17) << average_mbr_width;
    average_height << std::setprecision(17) << average_mbr_height;
    estimate << std::fixed << std::setprecision(0) << candidate_estimate;
    summary.extra["spatial_join_candidate_pairs"] =
        std::to_string(summary.total_candidate_pairs);
    summary.extra["E_avg"] = average_edges.str();
    summary.extra["E_max"] = std::to_string(context.max_edges_per_record);
    summary.extra["E_total"] = std::to_string(context.total_edge_count);
    summary.extra["MBR_W_avg"] = average_width.str();
    summary.extra["MBR_H_avg"] = average_height.str();
    summary.extra["mbr_grid_candidate_estimate"] = estimate.str();
  }
  summary.timing_seconds = execution.timing_seconds;
  summary.memory = memory_stats;
  write_run_info(output_info, summary);
  return summary;
}

std::vector<RunSummary> run_baselines(const Options& opts, const ProjectPaths& paths, const char* argv0) {
  fs::path records_path = opts.records ? *opts.records : paths.repo_root / "dataset" / "processed" /
                                                    (opts.dataset + "_records.jsonl");
  std::string dataset = opts.dataset;
  if (opts.records && dataset == "nyc_taxi_zones") {
    std::string name = records_path.filename().string();
    const std::string suffix = "_records.jsonl";
    if (name.size() > suffix.size() && name.substr(name.size() - suffix.size()) == suffix) {
      dataset = name.substr(0, name.size() - suffix.size());
    } else {
      dataset = records_path.stem().string();
    }
  }

  auto initialization_start = std::chrono::steady_clock::now();
  DatasetInfo source_dataset_info = inspect_records(records_path);
  size_t selected_count =
      selected_record_count(source_dataset_info.cell_count, opts.dataset_ratio);
  DatasetInfo dataset_info =
      selected_count == source_dataset_info.cell_count
          ? source_dataset_info
          : inspect_records(records_path, selected_count);
  std::vector<CellRecord> common_records = read_records(records_path, selected_count);
  double initialization_seconds = std::chrono::duration<double>(
      std::chrono::steady_clock::now() - initialization_start).count();
  Bounds bounds = opts.bbox ? *opts.bbox : source_dataset_info.bounds;
  auto grid_res = parse_resolution(opts.resolution);
  auto [x_res, y_res] = grid_res;
  auto ri_res = parse_resolution(opts.ri_resolution);
  GridSpec grid = make_grid(bounds, x_res, y_res, opts.limit_pixels);
  if (opts.execution_mode == ExecutionMode::Materialized &&
      grid.pixel_count > kMaxMaterializedPixels) {
    throw std::runtime_error(
        "materialized execution requires " +
        std::to_string(grid.pixel_count) +
        " active pixels, exceeding the safety limit of " +
        std::to_string(kMaxMaterializedPixels) +
        "; use --execution-mode streaming or reduce --limit-pixels");
  }
  std::string run_name = opts.run_name.value_or(timestamp_name());
  fs::path out_dir = opts.out_dir.value_or(paths.pgv_root / "results");

  std::vector<std::string> methods = opts.method == "all" ? all_methods() : opts.methods;
  bool multi_method = methods.size() > 1;
  std::string mode_name = execution_mode_name(opts.execution_mode);
  std::string task_name = safe_path_component(run_name + "_" + dataset + "_" +
                                               opts.method + "_" + mode_name + "_" +
                                               opts.resolution + "_" +
                                               dataset_ratio_path_label(opts.dataset_ratio));
  fs::path command_task_dir = out_dir / task_name;
  fs::path command_manifest = command_task_dir / "command_manifest.txt";
  fs::path comparison_csv = command_task_dir / "comparison.csv";
  std::string command_method_arg = opts.method == "all" ? "all" : join_method_argument(methods);

  std::vector<std::string> source_parts = {argv0 ? argv0 : "pgv_baselines", "--dataset", dataset, "--resolution",
                                           opts.resolution, "--method", command_method_arg, "--ri-resolution",
                                           opts.ri_resolution, "--ri-variant", opts.ri_variant,
                                           "--execution-mode", mode_name,
                                           "--out-dir", out_dir.string(), "--run-name", run_name,
                                           "--dataset-ratio",
                                           format_dataset_ratio(opts.dataset_ratio)};
  if (opts.discard_output) source_parts.push_back("--discard-output");
  if (opts.validation_grid) source_parts.push_back("--validation-grid");
  if (opts.progressive_snapshots) source_parts.push_back("--progressive-snapshots");
  if (opts.compare) {
    source_parts.push_back("--compare");
  }
  if (opts.verify_plan) {
    source_parts.push_back("--verify-plan");
  }
  if (opts.verify_april_candidates) {
    source_parts.push_back("--verify-april-candidates");
  }
  if (opts.no_progress) {
    source_parts.push_back("--no-progress");
  }
  if (opts.limit_pixels) {
    source_parts.push_back("--limit-pixels");
    source_parts.push_back(std::to_string(*opts.limit_pixels));
  }
  if (!opts.progressive_checkpoints.empty()) {
    source_parts.push_back("--progressive-checkpoints");
    source_parts.push_back(
        join_progressive_checkpoints(opts.progressive_checkpoints));
  }
  source_parts.push_back("--threads");
  source_parts.push_back(std::to_string(opts.threads));
  if (opts.execution_mode == ExecutionMode::Streaming) {
    source_parts.push_back("--batch-rows");
    source_parts.push_back(std::to_string(opts.batch_rows));
  }
  if (opts.records) {
    source_parts.push_back("--records");
    source_parts.push_back(fs::absolute(records_path).string());
  }
  std::map<std::string, std::string> common_extra = {{"run_name", run_name},
                                                     {"method_argument", command_method_arg},
                                                     {"execution_mode", mode_name},
                                                     {"dataset_fingerprint",
                                                      dataset_info.fingerprint},
                                                     {"source_dataset_fingerprint",
                                                      source_dataset_info.fingerprint},
                                                     {"dataset_ratio_percent",
                                                      format_dataset_ratio(opts.dataset_ratio)},
                                                     {"dataset_selection_policy", "file_prefix"},
                                                     {"source_cell_count",
                                                      std::to_string(source_dataset_info.cell_count)},
                                                     {"selected_cell_count",
                                                      std::to_string(dataset_info.cell_count)},
                                                     {"input_format", "JSONL_GEOJSON"},
                                                     {"parsed_record_count",
                                                      std::to_string(dataset_info.cell_count)},
                                                     {"memory_strategy",
                                                      opts.execution_mode == ExecutionMode::Streaming
                                                          ? "bounded_streaming"
                                                          : "fully_materialized_in_memory"},
                                                     {"threads", std::to_string(opts.threads)},
                                                     {"batch_rows",
                                                      opts.execution_mode == ExecutionMode::Streaming
                                                          ? std::to_string(opts.batch_rows)
                                                          : "all"},
                                                     {"progressive_checkpoints",
                                                      join_progressive_checkpoints(
                                                          opts.progressive_checkpoints)},
                                                     {"command_task_dir", fs::absolute(command_task_dir).string()},
                                                     {"source_command", shell_join(source_parts)}};
  common_extra["initialization_seconds"] = [&] {
    std::ostringstream value; value << std::setprecision(17) << initialization_seconds; return value.str();
  }();
  MethodContext context{records_path,
                        dataset_info.fingerprint,
                        dataset_info.bounds,
                        grid,
                        ri_res,
                        dataset_info.cell_count,
                        dataset_info.coordinate_count,
                        dataset_info.total_edge_count,
                        dataset_info.max_edges_per_record,
                        dataset_info.total_mbr_width,
                        dataset_info.total_mbr_height,
                        opts.threads,
                        opts.batch_rows,
                        opts.execution_mode,
                        opts.progressive_checkpoints,
                        &common_records,
                        nullptr,
                        !opts.no_progress,
                        opts.verify_plan,
                        opts.verify_april_candidates};
  std::vector<RunSummary> summaries;
  for (const auto& method : methods) {
    summaries.push_back(run_method(method, opts, context, dataset, command_task_dir,
                                   multi_method, command_manifest, common_extra));
  }

  if (opts.compare && !summaries.empty()) {
    std::vector<ComparisonRow> comparisons;
    const auto& reference = summaries.front();
    comparisons.push_back(compare_csv(reference.method, reference.method, reference.output_csv, reference.output_csv));
    for (size_t i = 1; i < summaries.size(); ++i) {
      comparisons.push_back(compare_csv(reference.method, summaries[i].method, reference.output_csv, summaries[i].output_csv));
    }
    write_comparison_csv(comparison_csv, comparisons);
    print_comparison_table(comparisons, comparison_csv);
    for (const auto& row : comparisons) {
      if (!row.passed) {
        throw std::runtime_error("baseline outputs differ for " + row.method + " by abs=" +
                                 std::to_string(row.max_abs_diff) + ", rel=" + std::to_string(row.max_rel_diff));
      }
    }
  }

  if (multi_method) {
    std::map<std::string, std::string> manifest = {{"run_name", run_name},
                                                   {"dataset", dataset},
                                                   {"records_path", fs::absolute(records_path).string()},
                                                   {"dataset_fingerprint",
                                                    dataset_info.fingerprint},
                                                   {"source_dataset_fingerprint",
                                                    source_dataset_info.fingerprint},
                                                   {"dataset_ratio_percent",
                                                    format_dataset_ratio(opts.dataset_ratio)},
                                                   {"dataset_selection_policy", "file_prefix"},
                                                   {"source_cell_count",
                                                    std::to_string(source_dataset_info.cell_count)},
                                                   {"selected_cell_count",
                                                    std::to_string(dataset_info.cell_count)},
                                                   {"method_argument", command_method_arg},
                                                   {"resolution", opts.resolution},
                                                   {"ri_resolution", opts.ri_resolution},
                                                   {"ri_variant", opts.ri_variant},
                                                   {"execution_mode", mode_name},
                                                   {"bbox", json({{"x_min", bounds.x_min},
                                                                   {"y_min", bounds.y_min},
                                                                   {"x_max", bounds.x_max},
                                                                   {"y_max", bounds.y_max}})
                                                                .dump()},
                                                   {"threads", std::to_string(opts.threads)},
                                                   {"batch_rows",
                                                    opts.execution_mode == ExecutionMode::Streaming
                                                        ? std::to_string(opts.batch_rows)
                                                        : "all"},
                                                   {"compare", opts.compare ? "true" : "false"},
                                                   {"verify_plan", opts.verify_plan ? "true" : "false"},
                                                   {"verify_april_candidates",
                                                    opts.verify_april_candidates ? "true" : "false"},
                                                   {"limit_pixels",
                                                    opts.limit_pixels ? std::to_string(*opts.limit_pixels) : ""},
                                                   {"progressive_checkpoints",
                                                    join_progressive_checkpoints(
                                                        opts.progressive_checkpoints)},
                                                   {"command_task_dir", fs::absolute(command_task_dir).string()},
                                                   {"comparison_csv",
                                                    opts.compare ? fs::absolute(comparison_csv).string() : ""},
                                                   {"source_command", shell_join(source_parts)}};
    write_command_manifest(command_manifest, manifest, methods, summaries);
  }

  return summaries;
}

void run_self_test(const ProjectPaths& paths, const char* argv0) {
  fs::path temp = fs::temp_directory_path() / ("pgv-cpp-self-test-" + timestamp_name() + "-" +
                                               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  fs::path records_path = temp / "self_test_records.jsonl";
  write_self_test_records(records_path);

  Options opts;
  opts.dataset = "self_test";
  opts.records = records_path;
  opts.resolution = "2x2";
  opts.method = "all";
  opts.bbox = Bounds{0.0, 0.0, 2.0, 2.0};
  opts.out_dir = temp / "results";
  opts.run_name = "self_test";
  opts.compare = true;
  opts.verify_plan = false;
  opts.verify_april_candidates = true;
  opts.no_progress = true;
  opts.ri_resolution = "4x4";

  std::map<long long, double> expected = {{0, 2.0}, {1, 5.0}, {2, 2.0}, {3, 2.0}};
  for (ExecutionMode mode : {ExecutionMode::Streaming, ExecutionMode::Materialized}) {
    opts.execution_mode = mode;
    auto summaries = run_baselines(opts, paths, argv0);
    for (const auto& summary : summaries) {
      std::string timing_method = summary.method == "SLED" ? "sled" : summary.method;
      double method_algorithm =
          summary.timing_seconds.at(timing_method + "_algorithm_seconds");
      double method_total =
          summary.timing_seconds.at(timing_method + "_total_seconds");
      if (summary.runtime_seconds < summary.algorithm_seconds ||
          std::abs(summary.algorithm_seconds - method_algorithm) > 1e-12 ||
          std::abs(summary.runtime_seconds - method_total) > 1e-12) {
        throw std::runtime_error(summary.method +
                                 " end-to-end timing self-test failed");
      }
      if (summary.memory.rss_at_start_bytes == 0 ||
          summary.memory.peak_rss_bytes < summary.memory.rss_at_start_bytes ||
          summary.memory.peak_rss_bytes < summary.memory.rss_at_end_bytes ||
          summary.memory.peak_rss_delta_bytes !=
              summary.memory.peak_rss_bytes -
                  summary.memory.rss_at_start_bytes ||
          summary.memory.sampling_interval_ms != 10 ||
          summary.memory.source.empty() ||
          summary.memory.source == "unsupported") {
        throw std::runtime_error(summary.method +
                                 " memory-monitor self-test failed");
      }
      auto actual = read_density_csv(summary.output_csv);
      for (const auto& [pixel_id, expected_density] : expected) {
        double observed = actual[pixel_id];
        if (std::abs(observed - expected_density) > kCompareTolerance) {
          throw std::runtime_error(summary.method + " " + execution_mode_name(mode) +
                                   " self-test failed for pixel " +
                                   std::to_string(pixel_id) + ": expected " +
                                   std::to_string(expected_density) + ", got " +
                                   std::to_string(observed));
        }
      }
      if (summary.method == "scan") {
        verify_scan_candidate_counts(summary);
        long long expected_candidates =
            static_cast<long long>(summary.cell_count * summary.pixel_count);
        if (summary.total_candidate_pairs != expected_candidates) {
          throw std::runtime_error("scan total candidate count is not cells * pixels");
        }
      } else if (summary.method == "april_duckdb") {
        if (summary.extra.at("april_refinement_mode") !=
                "intersection_area_without_precheck" ||
            summary.extra.at("april_candidate_source") !=
                "internal_json_dataset" ||
            std::stoll(summary.extra.at("april_missing_exact_pairs")) != 0) {
          throw std::runtime_error(
              "APRIL internal-candidate self-test diagnostics are incorrect");
        }
      }
      if (summary.method != "spatial_join" &&
          (summary.extra.count("MBR_W_avg") != 0 ||
           summary.extra.count("MBR_H_avg") != 0 ||
           summary.extra.count("mbr_grid_candidate_estimate") != 0)) {
        throw std::runtime_error(
            "MBR grid-overlap statistics leaked into a non-spatial-join method");
      }
    }
  }

  fs::path spatial_stats_path = temp / "spatial_join_stats_records.jsonl";
  write_spatial_join_stats_test_records(spatial_stats_path);
  auto run_spatial_stats = [&](ExecutionMode mode,
                               std::optional<int> limit_pixels,
                               const std::string& run_name,
                               const Bounds& active_bounds) {
    Options stats_opts;
    stats_opts.dataset = "spatial_join_stats_test";
    stats_opts.records = spatial_stats_path;
    stats_opts.resolution = "4x4";
    stats_opts.method = "spatial_join";
    stats_opts.methods = {"spatial_join"};
    stats_opts.bbox = active_bounds;
    stats_opts.out_dir = temp / "spatial_join_stats_results";
    stats_opts.run_name = run_name;
    stats_opts.limit_pixels = limit_pixels;
    stats_opts.execution_mode = mode;
    stats_opts.threads = 1;
    stats_opts.no_progress = true;
    return run_baselines(stats_opts, paths, argv0).front();
  };

  RunSummary streaming_stats = run_spatial_stats(
      ExecutionMode::Streaming, std::nullopt, "spatial_stats_streaming",
      Bounds{0.0, 0.0, 4.0, 4.0});
  RunSummary materialized_stats = run_spatial_stats(
      ExecutionMode::Materialized, std::nullopt, "spatial_stats_materialized",
      Bounds{0.0, 0.0, 4.0, 4.0});
  RunSummary limited_stats = run_spatial_stats(
      ExecutionMode::Streaming, 1, "spatial_stats_limited",
      Bounds{0.0, 0.0, 4.0, 4.0});
  RunSummary custom_bbox_stats = run_spatial_stats(
      ExecutionMode::Streaming, 1, "spatial_stats_custom_bbox",
      Bounds{0.0, 0.0, 2.0, 4.0});
  for (const RunSummary* summary : {&streaming_stats, &materialized_stats,
                                    &limited_stats}) {
    if (summary->extra.at("E_total") != "16" ||
        summary->extra.at("E_max") != "8" ||
        std::abs(std::stod(summary->extra.at("E_avg")) - 8.0) > 1e-12 ||
        std::abs(std::stold(summary->extra.at("MBR_W_avg")) - 2.0L) > 1e-15L ||
        std::abs(std::stold(summary->extra.at("MBR_H_avg")) - 3.0L) > 1e-15L ||
        summary->extra.at("mbr_grid_candidate_estimate") != "12" ||
        std::stoll(summary->extra.at("spatial_join_candidate_pairs")) !=
            summary->total_candidate_pairs ||
        sum_csv_candidate_counts(summary->output_csv) !=
            summary->total_candidate_pairs) {
      throw std::runtime_error("spatial join complexity statistics self-test failed");
    }
  }
  if (custom_bbox_stats.extra.at("MBR_W_avg") != "2" ||
      custom_bbox_stats.extra.at("MBR_H_avg") != "3" ||
      custom_bbox_stats.extra.at("mbr_grid_candidate_estimate") != "24") {
    throw std::runtime_error(
        "spatial join MBR estimate did not use the active output bounding box");
  }
  if (streaming_stats.total_candidate_pairs !=
          materialized_stats.total_candidate_pairs ||
      limited_stats.total_candidate_pairs >= streaming_stats.total_candidate_pairs) {
    throw std::runtime_error(
        "spatial join candidate-pair limit or execution-mode self-test failed");
  }

  auto run_sled_comparison = [&](const fs::path& fixture_path,
                                 const std::string& fixture_name,
                                 const std::string& resolution,
                                 const Bounds& fixture_bounds,
                                 std::optional<int> limit_pixels = std::nullopt) {
    Options fixture_opts;
    fixture_opts.dataset = fixture_name;
    fixture_opts.records = fixture_path;
    fixture_opts.resolution = resolution;
    fixture_opts.method = "scan_SLED";
    fixture_opts.methods = {"scan", "SLED"};
    fixture_opts.bbox = fixture_bounds;
    fixture_opts.out_dir = temp / (fixture_name + "_results");
    fixture_opts.run_name = fixture_name;
    fixture_opts.limit_pixels = limit_pixels;
    fixture_opts.compare = true;
    fixture_opts.verify_plan = false;
    fixture_opts.threads = 1;
    fixture_opts.no_progress = true;
    return run_baselines(fixture_opts, paths, argv0);
  };

  auto run_april_comparison = [&](const fs::path& fixture_path,
                                  const std::string& fixture_name,
                                  const std::string& resolution,
                                  const Bounds& fixture_bounds,
                                  std::optional<int> limit_pixels = std::nullopt) {
    std::vector<RunSummary> latest;
    for (ExecutionMode mode : {ExecutionMode::Streaming,
                               ExecutionMode::Materialized}) {
      Options fixture_opts;
      fixture_opts.dataset = fixture_name;
      fixture_opts.records = fixture_path;
      fixture_opts.resolution = resolution;
      fixture_opts.method = "scan_april_duckdb";
      fixture_opts.methods = {"scan", "april_duckdb"};
      fixture_opts.bbox = fixture_bounds;
      fixture_opts.out_dir = temp / (fixture_name + "_april_results");
      fixture_opts.run_name = fixture_name;
      fixture_opts.limit_pixels = limit_pixels;
      fixture_opts.compare = true;
      fixture_opts.verify_april_candidates = true;
      fixture_opts.execution_mode = mode;
      fixture_opts.ri_resolution = "8x8";
      fixture_opts.threads = 1;
      fixture_opts.no_progress = true;
      latest = run_baselines(fixture_opts, paths, argv0);
    }
    return latest;
  };

  fs::path geometry_path = temp / "sled_geometry_records.jsonl";
  write_sled_geometry_test_records(geometry_path);
  run_sled_comparison(geometry_path, "sled_geometry_test", "4x4", Bounds{0.0, 0.0, 4.0, 4.0});
  auto april_geometry = run_april_comparison(
      geometry_path, "april_geometry_test", "4x4",
      Bounds{0.0, 0.0, 4.0, 4.0}, 6);
  auto april_geometry_summary = std::find_if(
      april_geometry.begin(), april_geometry.end(),
      [](const RunSummary& summary) { return summary.method == "april_duckdb"; });
  if (april_geometry_summary == april_geometry.end() ||
      std::stoll(april_geometry_summary->extra.at(
          "april_missing_exact_pairs")) != 0 ||
      std::stoll(april_geometry_summary->extra.at(
          "april_retained_pairs")) <= 0) {
    throw std::runtime_error(
        "APRIL internal filtering self-test failed");
  }

  fs::path duplicate_id_path = temp / "april_duplicate_id_records.jsonl";
  write_april_duplicate_id_test_records(duplicate_id_path);
  run_april_comparison(duplicate_id_path, "april_duplicate_id_test", "2x1",
                       Bounds{0.0, 0.0, 2.0, 1.0});

  fs::path shared_equal_path = temp / "sled_shared_equal_records.jsonl";
  write_sled_shared_boundary_test_records(shared_equal_path, 1.0, 1.0);
  auto shared_equal = run_sled_comparison(shared_equal_path, "sled_shared_equal_test", "2x1",
                                          Bounds{0.0, 0.0, 2.0, 1.0});
  auto equal_sled = std::find_if(shared_equal.begin(), shared_equal.end(), [](const RunSummary& summary) {
    return summary.method == "SLED";
  });
  if (equal_sled == shared_equal.end() || std::stoll(equal_sled->extra.at("sled_cancelled_edges")) <= 0) {
    throw std::runtime_error("SLED shared-boundary self-test expected at least one cancelled edge");
  }

  fs::path shared_weighted_path = temp / "sled_shared_weighted_records.jsonl";
  write_sled_shared_boundary_test_records(shared_weighted_path, 1.0, 2.0);
  run_sled_comparison(shared_weighted_path, "sled_shared_weighted_test", "2x1",
                      Bounds{0.0, 0.0, 2.0, 1.0});

  fs::path bbox_path = temp / "sled_bbox_records.jsonl";
  write_sled_bbox_test_records(bbox_path);
  run_sled_comparison(bbox_path, "sled_bbox_test", "4x4", Bounds{0.0, 0.0, 4.0, 4.0}, 6);

  fs::remove_all(temp);
  std::cout << "self-test passed\n";
}

}  // namespace pgv
