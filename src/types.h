#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pgv {

constexpr double kCompareTolerance = 1e-9;
constexpr double kCompareRelativeTolerance = 1e-12;
constexpr size_t kMaxStreamingBatchPixels = 65536;
constexpr size_t kMaxMaterializedPixels = 262144;

enum class ExecutionMode {
  Streaming,
  Materialized,
};

inline const char* execution_mode_name(ExecutionMode mode) {
  return mode == ExecutionMode::Streaming ? "streaming" : "materialized";
}

struct Bounds {
  double x_min = 0.0;
  double y_min = 0.0;
  double x_max = 0.0;
  double y_max = 0.0;
};

struct CellRecord {
  long long polygon_index = 0;
  std::string cell_id;
  double weight = 0.0;
  std::string geometry_json;
};

struct PixelRow {
  int u = 0;
  int v = 0;
  long long pixel_id = 0;
  double x_min = 0.0;
  double y_min = 0.0;
  double x_max = 0.0;
  double y_max = 0.0;
};

struct GridSpec {
  Bounds bounds;
  int x_res = 0;
  int y_res = 0;
  double x_step = 0.0;
  double y_step = 0.0;
  size_t pixel_count = 0;

  PixelRow pixel_at(size_t pixel_id) const;
  size_t max_batch_pixels(int batch_rows) const;
};

struct DatasetInfo {
  Bounds bounds;
  size_t cell_count = 0;
  size_t coordinate_count = 0;
  std::uint64_t total_edge_count = 0;
  std::uint64_t max_edges_per_record = 0;
  long double total_mbr_width = 0.0L;
  long double total_mbr_height = 0.0L;
  std::string fingerprint;
};

struct ResultRow {
  int u = 0;
  int v = 0;
  long long pixel_id = 0;
  double density = 0.0;
  long long candidate_count = 0;
};

struct MemoryStats {
  std::uint64_t rss_at_start_bytes = 0;
  std::uint64_t peak_rss_bytes = 0;
  std::uint64_t peak_rss_delta_bytes = 0;
  std::uint64_t rss_at_end_bytes = 0;
  int sampling_interval_ms = 10;
  std::string source;
};

struct RunSummary {
  std::string method;
  std::string dataset;
  std::string records_path;
  std::string resolution;
  Bounds bbox;
  size_t cell_count = 0;
  size_t pixel_count = 0;
  long long total_candidate_pairs = 0;
  double runtime_seconds = 0.0;
  double algorithm_seconds = 0.0;
  double computation_seconds = 0.0;
  double serialization_seconds = 0.0;
  double initialization_seconds = 0.0;
  std::string duckdb_version;
  std::string task_dir;
  std::string output_csv;
  std::string output_json;
  std::string output_info;
  std::string command_manifest;
  std::map<std::string, std::string> extra;
  std::map<std::string, double> timing_seconds;
  MemoryStats memory;
};

struct ComparisonRow {
  std::string reference_method;
  std::string method;
  double max_abs_diff = 0.0;
  double max_rel_diff = 0.0;
  long long max_abs_pixel_id = -1;
  double reference_density = 0.0;
  double method_density = 0.0;
  bool passed = true;
};

struct ProgressiveMetric {
  std::string checkpoint_label;
  size_t completed_rows = 0;
  size_t total_rows = 0;
  double completion_fraction = 0.0;
  double compute_seconds = 0.0;
};

struct Options {
  std::optional<std::filesystem::path> config;
  std::optional<std::string> interactive_operation;
  bool method_explicit = false;
  std::string dataset = "nyc_taxi_zones";
  std::optional<std::filesystem::path> records;
  std::string resolution = "1600x1200";
  std::string method = "SLED";
  std::vector<std::string> methods = {"SLED"};
  std::optional<Bounds> bbox;
  std::optional<std::filesystem::path> out_dir;
  std::optional<std::string> run_name;
  std::optional<int> limit_pixels;
  double dataset_ratio = 100.0;
  bool compare = false;
  bool verify_plan = false;
  bool verify_april_candidates = false;
  int threads = 1;
  int batch_rows = 16;
  ExecutionMode execution_mode = ExecutionMode::Streaming;
  bool self_test = false;
  bool no_progress = false;
  bool discard_output = false;
  bool validation_grid = false;
  bool progressive_snapshots = false;
  std::string sled_update_mode = "delta";
  std::vector<std::string> interactive_trials;
  std::string ri_resolution = "512x512";
  std::string ri_variant = "april_duckdb";
  std::vector<double> progressive_checkpoints;
};

struct MethodContext {
  std::filesystem::path records_path;
  std::string dataset_fingerprint;
  Bounds dataset_bounds;
  GridSpec grid;
  std::pair<int, int> ri_resolution;
  size_t cell_count = 0;
  size_t coordinate_count = 0;
  std::uint64_t total_edge_count = 0;
  std::uint64_t max_edges_per_record = 0;
  long double total_mbr_width = 0.0L;
  long double total_mbr_height = 0.0L;
  int threads = 1;
  int batch_rows = 16;
  ExecutionMode execution_mode = ExecutionMode::Streaming;
  std::vector<double> progressive_checkpoints;
  const std::vector<CellRecord>* materialized_records = nullptr;
  const std::vector<PixelRow>* materialized_pixels = nullptr;
  bool show_progress = true;
  bool verify_plan = false;
  bool verify_april_candidates = false;
};

struct MethodExecution {
  std::map<std::string, std::string> extra;
  std::map<std::string, double> timing_seconds;
  std::vector<ProgressiveMetric> progressive_metrics;
};

using ResultBatchConsumer = std::function<void(const std::vector<ResultRow>&)>;

struct ProjectPaths {
  std::filesystem::path repo_root;
  std::filesystem::path pgv_root;
};

}  // namespace pgv
