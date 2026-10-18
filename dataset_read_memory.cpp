#include "src/io.h"
#include "src/memory.h"
#include "src/utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct Options {
  std::string dataset = "nyc_taxi_zones";
  std::optional<fs::path> records_path;
  double dataset_ratio = 100.0;
  bool help = false;
};

void print_usage(const char* argv0) {
  std::cout << "Usage: " << argv0 << " [options]\n\n"
            << "Options:\n"
            << "  --dataset KEY          Dataset key used to resolve dataset/processed/<KEY>_records.jsonl\n"
            << "  --records PATH         Explicit canonical JSONL records path\n"
            << "  --dataset-ratio P      Percentage of records to materialize, in (0, 100]\n"
            << "  -h, --help             Show this help message\n";
}

double parse_ratio(const std::string& value) {
  std::size_t pos = 0;
  double ratio = std::stod(value, &pos);
  if (pos != value.size() || !std::isfinite(ratio) || ratio <= 0.0 ||
      ratio > 100.0) {
    throw std::runtime_error("--dataset-ratio must be a number in (0, 100]");
  }
  return ratio;
}

Options parse_args(int argc, char** argv) {
  Options opts;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto require_value = [&](const std::string& flag) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(flag + " requires a value");
      }
      return argv[++i];
    };

    if (arg == "-h" || arg == "--help") {
      opts.help = true;
    } else if (arg == "--dataset") {
      opts.dataset = require_value(arg);
      if (opts.dataset.empty()) {
        throw std::runtime_error("--dataset cannot be empty");
      }
    } else if (arg == "--records") {
      opts.records_path = fs::path(require_value(arg));
    } else if (arg == "--dataset-ratio") {
      opts.dataset_ratio = parse_ratio(require_value(arg));
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }
  return opts;
}

std::size_t selected_record_count(std::size_t source_count, double ratio) {
  if (source_count == 0 || ratio >= 100.0) {
    return source_count;
  }
  long double scaled = static_cast<long double>(source_count) *
                       static_cast<long double>(ratio) / 100.0L;
  std::size_t selected = static_cast<std::size_t>(std::floor(scaled));
  return std::max<std::size_t>(1, std::min(source_count, selected));
}

fs::path resolve_records_path(const Options& opts, const char* argv0) {
  fs::path path;
  if (opts.records_path) {
    path = *opts.records_path;
  } else {
    pgv::ProjectPaths paths = pgv::find_project_paths(argv0);
    path = paths.repo_root / "dataset" / "processed" /
           (opts.dataset + "_records.jsonl");
  }
  return fs::absolute(path).lexically_normal();
}

std::uint64_t delta_or_zero(std::uint64_t after, std::uint64_t before) {
  return after >= before ? after - before : 0;
}

double bytes_to_mib(std::uint64_t bytes) {
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void print_report(const Options& opts, const fs::path& records_path,
                  const pgv::DatasetInfo& source_info,
                  const pgv::DatasetInfo& selected_info,
                  const pgv::MemoryStats& memory,
                  std::uint64_t jsonl_size_bytes) {
  std::uint64_t read_delta =
      delta_or_zero(memory.rss_at_end_bytes, memory.rss_at_start_bytes);
  double average_edges = selected_info.cell_count == 0
                             ? 0.0
                             : static_cast<double>(selected_info.total_edge_count) /
                                   static_cast<double>(selected_info.cell_count);

  std::cout << std::fixed << std::setprecision(6)
            << "dataset=" << opts.dataset << "\n"
            << "records_path=" << records_path.string() << "\n"
            << "source_cell_count=" << source_info.cell_count << "\n"
            << "selected_cell_count=" << selected_info.cell_count << "\n"
            << "jsonl_size_bytes=" << jsonl_size_bytes << "\n"
            << "coordinate_count=" << selected_info.coordinate_count << "\n"
            << "E_total=" << selected_info.total_edge_count << "\n"
            << "E_avg=" << average_edges << "\n"
            << "memory_rss_before_bytes=" << memory.rss_at_start_bytes << "\n"
            << "memory_rss_after_bytes=" << memory.rss_at_end_bytes << "\n"
            << "memory_peak_rss_bytes=" << memory.peak_rss_bytes << "\n"
            << "memory_read_delta_bytes=" << read_delta << "\n"
            << "memory_peak_delta_bytes=" << memory.peak_rss_delta_bytes << "\n"
            << "memory_peak_rss_mib=" << bytes_to_mib(memory.peak_rss_bytes)
            << "\n"
            << "memory_peak_delta_mib="
            << bytes_to_mib(memory.peak_rss_delta_bytes) << "\n"
            << "memory_measurement_source=" << memory.source << "\n"
            << "measurement_scope=dataset_read_materialization_only\n";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Options opts = parse_args(argc, argv);
    if (opts.help) {
      print_usage(argv[0]);
      return 0;
    }

    fs::path records_path = resolve_records_path(opts, argv[0]);
    if (!fs::exists(records_path)) {
      throw std::runtime_error("records file does not exist: " +
                               records_path.string());
    }

    std::uint64_t jsonl_size_bytes =
        static_cast<std::uint64_t>(fs::file_size(records_path));
    pgv::DatasetInfo source_info = pgv::inspect_records(records_path);
    std::size_t selected_count =
        selected_record_count(source_info.cell_count, opts.dataset_ratio);
    pgv::DatasetInfo selected_info =
        selected_count == source_info.cell_count
            ? source_info
            : pgv::inspect_records(records_path, selected_count);

    pgv::release_unused_memory();
    pgv::MemoryMonitor monitor;
    std::vector<pgv::CellRecord> records =
        pgv::read_records(records_path, selected_count);
    volatile std::size_t retained_record_count = records.size();
    (void)retained_record_count;
    pgv::MemoryStats memory = monitor.stop();

    print_report(opts, records_path, source_info, selected_info, memory,
                 jsonl_size_bytes);
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "error: " << ex.what() << "\n";
    return 1;
  }
}
