#pragma once

#include "types.h"

#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pgv {

DatasetInfo inspect_records(const std::filesystem::path& path,
                            std::optional<size_t> max_records = std::nullopt);
std::vector<CellRecord> read_records(const std::filesystem::path& path,
                                     std::optional<size_t> max_records = std::nullopt);
void for_each_record(const std::filesystem::path& path,
                     const std::function<void(const CellRecord&)>& consumer,
                     std::optional<size_t> max_records = std::nullopt);
GridSpec make_grid(const Bounds& bounds,
                   int x_res,
                   int y_res,
                   std::optional<int> limit_pixels);
std::vector<PixelRow> make_pixels(const GridSpec& grid);

class ResultWriter {
 public:
  ResultWriter(const std::filesystem::path& csv_path,
               const std::filesystem::path& json_path);
  ResultWriter(const ResultWriter&) = delete;
  ResultWriter& operator=(const ResultWriter&) = delete;
  ~ResultWriter();

  void append(const ResultRow& row);
  void append_batch(const std::vector<ResultRow>& rows);
  void finish();
  size_t row_count() const { return row_count_; }
  long long total_candidates() const { return total_candidates_; }

 private:
  std::ofstream csv_;
  std::ofstream json_;
  bool first_json_row_ = true;
  bool finished_ = false;
  size_t row_count_ = 0;
  long long total_candidates_ = 0;
};

// Export densities after computation for complete-grid numerical validation.
// Fixed IEEE-754 binary64, little endian, row-major/top-down pixel_id order.
double write_validation_grid(const std::filesystem::path& path,
                              const std::vector<ResultRow>& rows,
                              const GridSpec& grid,
                              std::map<std::string, std::string>& metadata);

void write_run_info(const std::filesystem::path& path, const RunSummary& summary);
void write_progressive_metrics(
    const std::filesystem::path& path,
    const std::vector<ProgressiveMetric>& metrics);
std::map<long long, double> read_density_csv(const std::filesystem::path& path);
ComparisonRow compare_csv(const std::string& reference_method,
                          const std::string& method,
                          const std::filesystem::path& left,
                          const std::filesystem::path& right);
void write_comparison_csv(const std::filesystem::path& path, const std::vector<ComparisonRow>& rows);
std::pair<std::filesystem::path, std::filesystem::path> output_paths(
    const std::filesystem::path& command_task_dir,
    const std::string& method,
    bool multi_method);
void write_command_manifest(const std::filesystem::path& path,
                            const std::map<std::string, std::string>& info,
                            const std::vector<std::string>& methods,
                            const std::vector<RunSummary>& summaries);
void write_self_test_records(const std::filesystem::path& path);
void write_spatial_join_stats_test_records(const std::filesystem::path& path);
void write_april_duplicate_id_test_records(const std::filesystem::path& path);
void write_sled_geometry_test_records(const std::filesystem::path& path);
void write_sled_shared_boundary_test_records(const std::filesystem::path& path,
                                             double left_weight,
                                             double right_weight);
void write_sled_bbox_test_records(const std::filesystem::path& path);

}  // namespace pgv
