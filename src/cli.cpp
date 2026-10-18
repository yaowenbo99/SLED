#include "cli.h"

#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace pgv {

namespace {

std::vector<std::string> parse_method_list(std::string value) {
  const std::set<std::string> valid_methods = {"scan", "spatial_join", "rtree", "april_duckdb", "SLED", "all"};
  std::vector<std::string> methods;
  std::set<std::string> seen;
  std::stringstream src(value);
  std::string token;
  while (std::getline(src, token, ',')) {
    if (token.empty()) {
      throw std::runtime_error("--method contains an empty method token");
    }
    if (!valid_methods.count(token)) {
      throw std::runtime_error("unsupported method: " + token);
    }
    if (!seen.insert(token).second) {
      throw std::runtime_error("--method contains duplicate method: " + token);
    }
    methods.push_back(token);
  }
  if (methods.empty()) {
    throw std::runtime_error("--method requires at least one method");
  }
  if (seen.count("all") && methods.size() > 1) {
    throw std::runtime_error("--method all cannot be combined with other methods");
  }
  if (methods.size() == 1 && methods.front() == "all") {
    return {"scan", "spatial_join", "rtree", "april_duckdb", "SLED"};
  }
  return methods;
}

std::string join_methods(const std::vector<std::string>& methods) {
  std::ostringstream out;
  for (size_t i = 0; i < methods.size(); ++i) {
    if (i) {
      out << '_';
    }
    out << methods[i];
  }
  return out.str();
}

double parse_dataset_ratio(const std::string& value) {
  size_t consumed = 0;
  double ratio = 0.0;
  try {
    ratio = std::stod(value, &consumed);
  } catch (const std::exception&) {
    throw std::runtime_error("--dataset-ratio must be a number in (0, 100]");
  }
  if (consumed != value.size() || !std::isfinite(ratio)) {
    throw std::runtime_error("--dataset-ratio must be a finite number in (0, 100]");
  }
  return ratio;
}

std::vector<double> parse_progressive_checkpoints(const std::string& value) {
  std::vector<double> checkpoints;
  std::stringstream source(value);
  std::string token;
  while (std::getline(source, token, ',')) {
    if (token.empty()) {
      throw std::runtime_error(
          "--progressive-checkpoints contains an empty percentage");
    }
    size_t consumed = 0;
    double percentage = 0.0;
    try {
      percentage = std::stod(token, &consumed);
    } catch (const std::exception&) {
      throw std::runtime_error(
          "--progressive-checkpoints must be comma-separated percentages in (0, 100]");
    }
    if (consumed != token.size() || !std::isfinite(percentage) ||
        percentage <= 0.0 || percentage > 100.0) {
      throw std::runtime_error(
          "--progressive-checkpoints must be comma-separated percentages in (0, 100]");
    }
    checkpoints.push_back(percentage);
  }
  if (checkpoints.empty()) {
    throw std::runtime_error("--progressive-checkpoints requires at least one percentage");
  }
  std::sort(checkpoints.begin(), checkpoints.end());
  auto duplicate = std::adjacent_find(checkpoints.begin(), checkpoints.end());
  if (duplicate != checkpoints.end()) {
    throw std::runtime_error("--progressive-checkpoints contains duplicate percentages");
  }
  return checkpoints;
}

}  // namespace

void print_usage() {
  std::cout
      << "Usage: sled [--dataset KEY] [--records PATH] [--resolution XxY]\n"
      << "  [--method scan|spatial_join|rtree|april_duckdb|SLED|all|scan,SLED] [--bbox xmin,ymin,xmax,ymax]\n"
      << "  [--ri-resolution XxY] [--out-dir PATH]\n"
      << "  [--run-name NAME] [--limit-pixels N]\n"
      << "  [--dataset-ratio PERCENT]\n"
      << "  [--progressive-checkpoints PERCENT,...] [--progressive-snapshots] (single streaming SLED run only)\n"
      << "  [--interactive-operation zoom|panning|updating|all]\n"
      << "  [--interactive-trials ID,...] [--sled-update-mode delta|full] [--discard-output] [--validation-grid]\n"
      << "  [--threads N] [--batch-rows N] [--execution-mode streaming|materialized]\n"
      << "  [--compare] [--verify-plan] [--verify-april-candidates] [--self-test]\n"
      << "       sled --config PATH\n";
}

void normalize_and_validate_options(Options& opts) {
  if (opts.interactive_operation) {
    const std::set<std::string> operations = {"zoom", "panning", "updating", "all"};
    if (!operations.count(*opts.interactive_operation)) {
      throw std::runtime_error(
          "--interactive-operation must be zoom, panning, updating, or all");
    }
    if (opts.verify_plan || opts.verify_april_candidates) {
      throw std::runtime_error(
          "interactive operation does not support plan/candidate verification");
    }
    if (opts.limit_pixels) {
      throw std::runtime_error("interactive operation does not support --limit-pixels");
    }
    if (opts.self_test) {
      throw std::runtime_error("interactive operation cannot be combined with --self-test");
    }
    if (opts.execution_mode != ExecutionMode::Streaming) {
      throw std::runtime_error("interactive operation supports only streaming execution mode");
    }
    if (!opts.method_explicit) {
      opts.method = "SLED";
    }
  }
  if (opts.sled_update_mode != "delta" && opts.sled_update_mode != "full") {
    throw std::runtime_error("--sled-update-mode must be delta or full");
  }
  if (!opts.interactive_trials.empty() && !opts.interactive_operation) {
    throw std::runtime_error("--interactive-trials requires --interactive-operation");
  }
  if (opts.validation_grid && opts.limit_pixels) {
    throw std::runtime_error("--validation-grid requires a complete grid without --limit-pixels");
  }
  if (opts.discard_output && opts.compare) {
    throw std::runtime_error("--discard-output cannot be combined with --compare");
  }
  parse_resolution(opts.resolution);
  auto ri_resolution = parse_resolution(opts.ri_resolution);
  bool requested_all = opts.method == "all";
  opts.methods = parse_method_list(opts.method);
  opts.method = requested_all ? "all" : join_methods(opts.methods);
  bool includes_april = std::find(opts.methods.begin(), opts.methods.end(),
                                  "april_duckdb") != opts.methods.end();
  if (includes_april) {
    int size = ri_resolution.first;
    if (size != ri_resolution.second || (size & (size - 1)) != 0) {
      throw std::runtime_error(
          "--ri-resolution must be a square power-of-two for april_duckdb");
    }
  }
  if (opts.verify_april_candidates && !includes_april) {
    throw std::runtime_error(
        "--verify-april-candidates requires april_duckdb");
  }
  if (opts.limit_pixels && *opts.limit_pixels <= 0) {
    throw std::runtime_error("--limit-pixels must be positive");
  }
  if (!std::isfinite(opts.dataset_ratio) || opts.dataset_ratio <= 0.0 ||
      opts.dataset_ratio > 100.0) {
    throw std::runtime_error("--dataset-ratio must be a finite number in (0, 100]");
  }
  if (opts.threads <= 0) {
    throw std::runtime_error("--threads must be positive");
  }
  if (opts.batch_rows <= 0) {
    throw std::runtime_error("--batch-rows must be positive");
  }
  if (opts.dataset.empty()) {
    throw std::runtime_error("--dataset must not be empty");
  }
  if (opts.run_name && opts.run_name->empty()) {
    throw std::runtime_error("--run-name must not be empty");
  }
  if (opts.ri_variant != "april_duckdb" && opts.ri_variant != "april") {
    throw std::runtime_error("--ri-variant supports only april_duckdb/april");
  }
  if (opts.progressive_snapshots && (opts.progressive_checkpoints.empty() || opts.discard_output)) {
    throw std::runtime_error("--progressive-snapshots requires checkpoints and exported output");
  }
  if (!opts.progressive_checkpoints.empty()) {
    for (double percentage : opts.progressive_checkpoints) {
      if (!std::isfinite(percentage) || percentage <= 0.0 ||
          percentage > 100.0) {
        throw std::runtime_error(
            "--progressive-checkpoints values must be in (0, 100]");
      }
    }
    if (!std::is_sorted(opts.progressive_checkpoints.begin(),
                        opts.progressive_checkpoints.end()) ||
        std::adjacent_find(opts.progressive_checkpoints.begin(),
                           opts.progressive_checkpoints.end()) !=
            opts.progressive_checkpoints.end()) {
      throw std::runtime_error(
          "--progressive-checkpoints values must be sorted and unique");
    }
    if (opts.interactive_operation) {
      throw std::runtime_error(
          "--progressive-checkpoints is not supported for interactive operations");
    }
    if (opts.methods.size() != 1 || opts.methods.front() != "SLED") {
      throw std::runtime_error(
          "--progressive-checkpoints requires --method SLED");
    }
    if (opts.execution_mode != ExecutionMode::Streaming) {
      throw std::runtime_error(
          "--progressive-checkpoints requires streaming execution mode");
    }
  }
}

Options parse_args(int argc, char** argv) {
  Options opts;
  bool saw_config = false;
  bool saw_normal_option = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    auto need_value = [&](const std::string& name) -> std::string {
      if (i + 1 >= argc) {
        throw std::runtime_error(name + " requires a value");
      }
      return argv[++i];
    };

    if (arg == "--config") {
      if (saw_config) {
        throw std::runtime_error("--config may only be specified once");
      }
      opts.config = fs::path(need_value(arg));
      saw_config = true;
    } else if (arg == "--dataset") {
      saw_normal_option = true;
      opts.dataset = need_value(arg);
    } else if (arg == "--records") {
      saw_normal_option = true;
      opts.records = fs::path(need_value(arg));
    } else if (arg == "--resolution") {
      saw_normal_option = true;
      opts.resolution = need_value(arg);
    } else if (arg == "--method") {
      saw_normal_option = true;
      opts.method = need_value(arg);
      opts.method_explicit = true;
    } else if (arg == "--interactive-operation") {
      saw_normal_option = true;
      opts.interactive_operation = need_value(arg);
    } else if (arg == "--progressive-snapshots") {
      saw_normal_option = true;
      opts.progressive_snapshots = true;
    } else if (arg == "--validation-grid") {
      saw_normal_option = true;
      opts.validation_grid = true;
    } else if (arg == "--discard-output") {
      saw_normal_option = true;
      opts.discard_output = true;
    } else if (arg == "--sled-update-mode") {
      saw_normal_option = true;
      opts.sled_update_mode = need_value(arg);
    } else if (arg == "--interactive-trials") {
      saw_normal_option = true;
      std::stringstream values(need_value(arg));
      std::string id;
      std::set<std::string> seen;
      while (std::getline(values, id, ',')) {
        if (id.empty() || !seen.insert(id).second)
          throw std::runtime_error("--interactive-trials requires unique nonempty IDs");
        opts.interactive_trials.push_back(id);
      }
      if (opts.interactive_trials.empty()) throw std::runtime_error("--interactive-trials requires IDs");
    } else if (arg == "--bbox") {
      saw_normal_option = true;
      opts.bbox = parse_bbox(need_value(arg));
    } else if (arg == "--out-dir") {
      saw_normal_option = true;
      opts.out_dir = fs::path(need_value(arg));
    } else if (arg == "--run-name") {
      saw_normal_option = true;
      opts.run_name = need_value(arg);
    } else if (arg == "--limit-pixels") {
      saw_normal_option = true;
      opts.limit_pixels = std::stoi(need_value(arg));
    } else if (arg == "--dataset-ratio") {
      saw_normal_option = true;
      opts.dataset_ratio = parse_dataset_ratio(need_value(arg));
    } else if (arg == "--progressive-checkpoints") {
      saw_normal_option = true;
      opts.progressive_checkpoints =
          parse_progressive_checkpoints(need_value(arg));
    } else if (arg == "--compare") {
      saw_normal_option = true;
      opts.compare = true;
    } else if (arg == "--verify-plan") {
      saw_normal_option = true;
      opts.verify_plan = true;
    } else if (arg == "--verify-april-candidates") {
      saw_normal_option = true;
      opts.verify_april_candidates = true;
    } else if (arg == "--threads") {
      saw_normal_option = true;
      opts.threads = std::stoi(need_value(arg));
    } else if (arg == "--batch-rows") {
      saw_normal_option = true;
      opts.batch_rows = std::stoi(need_value(arg));
    } else if (arg == "--execution-mode") {
      saw_normal_option = true;
      std::string value = need_value(arg);
      if (value == "streaming") {
        opts.execution_mode = ExecutionMode::Streaming;
      } else if (value == "materialized") {
        opts.execution_mode = ExecutionMode::Materialized;
      } else {
        throw std::runtime_error(
            "--execution-mode must be streaming or materialized");
      }
    } else if (arg == "--self-test") {
      saw_normal_option = true;
      opts.self_test = true;
    } else if (arg == "--no-progress") {
      saw_normal_option = true;
      opts.no_progress = true;
    } else if (arg == "--ri-resolution") {
      saw_normal_option = true;
      opts.ri_resolution = need_value(arg);
    } else if (arg == "--ri-variant") {
      saw_normal_option = true;
      opts.ri_variant = need_value(arg);
    } else if (arg == "--help" || arg == "-h") {
      print_usage();
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if (saw_config && saw_normal_option) {
    throw std::runtime_error("--config cannot be combined with normal run options or --self-test");
  }
  if (!saw_config) {
    normalize_and_validate_options(opts);
  }
  return opts;
}

}  // namespace pgv
