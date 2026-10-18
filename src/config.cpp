#include "config.h"

#include "cli.h"
#include "runner.h"
#include "utils.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace pgv {
namespace {

const std::set<std::string>& option_fields() {
  static const std::set<std::string> fields = {
      "dataset", "records", "resolution", "method", "bbox", "out_dir",
      "run_name", "limit_pixels", "dataset_ratio", "threads", "batch_rows", "execution_mode",
      "compare", "verify_plan", "verify_april_candidates", "no_progress",
      "ri_resolution", "ri_variant", "progressive_checkpoints", "discard_output", "progressive_snapshots", "validation_grid"};
  return fields;
}

void reject_unknown_fields(const json& object,
                           const std::set<std::string>& allowed,
                           const std::string& context) {
  for (auto it = object.begin(); it != object.end(); ++it) {
    if (!allowed.count(it.key())) {
      throw std::runtime_error(context + " contains unknown field: " + it.key());
    }
  }
}

std::string require_string(const json& value,
                           const std::string& field,
                           const std::string& context) {
  if (!value.is_string()) {
    throw std::runtime_error(context + "." + field + " must be a string");
  }
  return value.get<std::string>();
}

int require_integer(const json& value,
                    const std::string& field,
                    const std::string& context) {
  if (!value.is_number_integer() && !value.is_number_unsigned()) {
    throw std::runtime_error(context + "." + field + " must be an integer");
  }
  try {
    return value.get<int>();
  } catch (const std::exception&) {
    throw std::runtime_error(context + "." + field + " is outside the supported integer range");
  }
}

bool require_boolean(const json& value,
                     const std::string& field,
                     const std::string& context) {
  if (!value.is_boolean()) {
    throw std::runtime_error(context + "." + field + " must be a boolean");
  }
  return value.get<bool>();
}

fs::path resolve_config_path(const std::string& value,
                             const fs::path& config_dir,
                             const std::string& field,
                             const std::string& context) {
  if (value.empty()) {
    throw std::runtime_error(context + "." + field + " must not be empty");
  }
  fs::path path(value);
  if (path.is_relative()) {
    path = config_dir / path;
  }
  return fs::absolute(path).lexically_normal();
}

void apply_options_object(Options& opts,
                          const json& object,
                          const fs::path& config_dir,
                          const std::string& context) {
  if (!object.is_object()) {
    throw std::runtime_error(context + " must be an object");
  }
  reject_unknown_fields(object, option_fields(), context);

  for (auto it = object.begin(); it != object.end(); ++it) {
    const std::string& field = it.key();
    const json& value = it.value();
    if (field == "dataset") {
      opts.dataset = require_string(value, field, context);
    } else if (field == "records") {
      opts.records = resolve_config_path(require_string(value, field, context), config_dir,
                                         field, context);
    } else if (field == "resolution") {
      opts.resolution = require_string(value, field, context);
    } else if (field == "method") {
      opts.method = require_string(value, field, context);
    } else if (field == "bbox") {
      if (!value.is_array() || value.size() != 4) {
        throw std::runtime_error(context + ".bbox must be [xmin, ymin, xmax, ymax]");
      }
      Bounds bounds;
      double* parts[] = {&bounds.x_min, &bounds.y_min, &bounds.x_max, &bounds.y_max};
      for (size_t i = 0; i < 4; ++i) {
        if (!value[i].is_number()) {
          throw std::runtime_error(context + ".bbox values must be numbers");
        }
        *parts[i] = value[i].get<double>();
        if (!std::isfinite(*parts[i])) {
          throw std::runtime_error(context + ".bbox values must be finite");
        }
      }
      if (!(bounds.x_min < bounds.x_max && bounds.y_min < bounds.y_max)) {
        throw std::runtime_error(context + ".bbox must satisfy xmin<xmax and ymin<ymax");
      }
      opts.bbox = bounds;
    } else if (field == "out_dir") {
      opts.out_dir = resolve_config_path(require_string(value, field, context), config_dir,
                                         field, context);
    } else if (field == "run_name") {
      opts.run_name = require_string(value, field, context);
    } else if (field == "limit_pixels") {
      opts.limit_pixels = require_integer(value, field, context);
    } else if (field == "dataset_ratio") {
      if (!value.is_number()) {
        throw std::runtime_error(context + ".dataset_ratio must be a number");
      }
      opts.dataset_ratio = value.get<double>();
    } else if (field == "threads") {
      opts.threads = require_integer(value, field, context);
    } else if (field == "batch_rows") {
      opts.batch_rows = require_integer(value, field, context);
    } else if (field == "execution_mode") {
      std::string mode = require_string(value, field, context);
      if (mode == "streaming") {
        opts.execution_mode = ExecutionMode::Streaming;
      } else if (mode == "materialized") {
        opts.execution_mode = ExecutionMode::Materialized;
      } else {
        throw std::runtime_error(context + ".execution_mode must be streaming or materialized");
      }
    } else if (field == "compare") {
      opts.compare = require_boolean(value, field, context);
    } else if (field == "verify_plan") {
      opts.verify_plan = require_boolean(value, field, context);
    } else if (field == "verify_april_candidates") {
      opts.verify_april_candidates = require_boolean(value, field, context);
    } else if (field == "progressive_snapshots") {
      opts.progressive_snapshots = require_boolean(value, field, context);
    } else if (field == "validation_grid") {
      opts.validation_grid = require_boolean(value, field, context);
    } else if (field == "discard_output") {
      opts.discard_output = require_boolean(value, field, context);
    } else if (field == "no_progress") {
      opts.no_progress = require_boolean(value, field, context);
    } else if (field == "ri_resolution") {
      opts.ri_resolution = require_string(value, field, context);
    } else if (field == "ri_variant") {
      opts.ri_variant = require_string(value, field, context);
    } else if (field == "progressive_checkpoints") {
      if (!value.is_array() || value.empty()) {
        throw std::runtime_error(
            context + ".progressive_checkpoints must be a non-empty array");
      }
      opts.progressive_checkpoints.clear();
      for (const auto& checkpoint : value) {
        if (!checkpoint.is_number()) {
          throw std::runtime_error(
              context + ".progressive_checkpoints values must be numbers");
        }
        opts.progressive_checkpoints.push_back(checkpoint.get<double>());
      }
      std::sort(opts.progressive_checkpoints.begin(),
                opts.progressive_checkpoints.end());
      auto duplicate = std::adjacent_find(opts.progressive_checkpoints.begin(),
                                          opts.progressive_checkpoints.end());
      if (duplicate != opts.progressive_checkpoints.end()) {
        throw std::runtime_error(
            context + ".progressive_checkpoints contains duplicate percentages");
      }
    }
  }
}

std::string summary_methods(const std::vector<RunSummary>& summaries) {
  std::ostringstream out;
  for (size_t i = 0; i < summaries.size(); ++i) {
    if (i) {
      out << ',';
    }
    out << summaries[i].method << "(end_to_end=" << std::fixed
        << std::setprecision(6) << summaries[i].runtime_seconds
        << "s,algorithm=" << summaries[i].algorithm_seconds << "s)";
  }
  return out.str();
}

std::string one_line(std::string value) {
  for (char& ch : value) {
    if (ch == '\n' || ch == '\r' || ch == '\t') {
      ch = ' ';
    }
  }
  return value;
}

}  // namespace

std::vector<Options> load_configured_runs(const fs::path& config_path) {
  fs::path absolute_path = fs::absolute(config_path).lexically_normal();
  std::ifstream input(absolute_path);
  if (!input) {
    throw std::runtime_error("cannot open configuration file: " + absolute_path.string());
  }

  json document;
  try {
    input >> document;
  } catch (const json::exception& err) {
    throw std::runtime_error("invalid JSON configuration " + absolute_path.string() +
                             ": " + err.what());
  }
  if (!document.is_object()) {
    throw std::runtime_error("configuration root must be an object");
  }
  reject_unknown_fields(document, {"schema_version", "defaults", "runs"},
                        "configuration root");
  if (!document.contains("schema_version") ||
      (!document["schema_version"].is_number_integer() &&
       !document["schema_version"].is_number_unsigned()) ||
      document["schema_version"].get<long long>() != 1) {
    throw std::runtime_error("configuration schema_version must be integer 1");
  }
  if (!document.contains("runs") || !document["runs"].is_array() ||
      document["runs"].empty()) {
    throw std::runtime_error("configuration runs must be a non-empty array");
  }

  json defaults = json::object();
  if (document.contains("defaults")) {
    defaults = document["defaults"];
    if (!defaults.is_object()) {
      throw std::runtime_error("configuration defaults must be an object");
    }
    reject_unknown_fields(defaults, option_fields(), "configuration defaults");
  }

  fs::path config_dir = absolute_path.parent_path();
  std::vector<Options> runs;
  runs.reserve(document["runs"].size());
  std::set<std::string> normalized_names;
  for (size_t index = 0; index < document["runs"].size(); ++index) {
    const json& run = document["runs"][index];
    std::string context = "configuration runs[" + std::to_string(index) + "]";
    if (!run.is_object()) {
      throw std::runtime_error(context + " must be an object");
    }
    reject_unknown_fields(run, option_fields(), context);

    Options opts;
    apply_options_object(opts, defaults, config_dir, "configuration defaults");
    apply_options_object(opts, run, config_dir, context);
    if (!opts.run_name || opts.run_name->empty()) {
      throw std::runtime_error(context + ".run_name is required and must not be empty");
    }
    std::string normalized_name = safe_path_component(*opts.run_name);
    if (!normalized_names.insert(normalized_name).second) {
      throw std::runtime_error("configuration run_name is not unique after path normalization: " +
                               *opts.run_name);
    }
    opts.config.reset();
    opts.self_test = false;
    normalize_and_validate_options(opts);
    runs.push_back(std::move(opts));
  }
  return runs;
}

int run_configured_runs(const fs::path& config_path,
                        const ProjectPaths& paths,
                        const char* argv0) {
  std::vector<Options> runs = load_configured_runs(config_path);
  struct Status {
    std::string name;
    bool passed = false;
    std::vector<RunSummary> summaries;
    std::string error;
  };
  std::vector<Status> statuses;
  statuses.reserve(runs.size());
  bool any_failed = false;

  for (size_t i = 0; i < runs.size(); ++i) {
    const std::string name = *runs[i].run_name;
    std::cout << '[' << (i + 1) << '/' << runs.size() << "] " << name << '\n' << std::flush;
    Status status;
    status.name = name;
    try {
      status.summaries = run_baselines(runs[i], paths, argv0);
      status.passed = true;
      for (const auto& summary : status.summaries) {
        std::cout << "  " << summary.method << ": pixels=" << summary.pixel_count
                  << ", candidates=" << summary.total_candidate_pairs
                  << ", seconds=" << std::fixed << std::setprecision(6)
                  << summary.runtime_seconds
                  << ", algorithm_seconds=" << summary.algorithm_seconds
                  << ", task_dir=" << summary.task_dir << '\n';
      }
    } catch (const std::exception& err) {
      any_failed = true;
      status.error = one_line(err.what());
      std::cerr << "  failed: " << status.error << '\n';
    }
    statuses.push_back(std::move(status));
  }

  std::cout << "\nconfiguration_summary\n";
  std::cout << "index,run_name,status,details\n";
  for (size_t i = 0; i < statuses.size(); ++i) {
    const auto& status = statuses[i];
    std::cout << (i + 1) << ',' << status.name << ','
              << (status.passed ? "success" : "failed") << ',';
    if (status.passed) {
      std::cout << "methods=" << summary_methods(status.summaries);
    } else {
      std::cout << "error=" << status.error;
    }
    std::cout << '\n';
  }
  return any_failed ? 1 : 0;
}

}  // namespace pgv
