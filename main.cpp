#include "src/cli.h"
#include "src/config.h"
#include "src/interactive.h"
#include "src/runner.h"
#include "src/utils.h"

#include <exception>
#include <iomanip>
#include <iostream>

int main(int argc, char** argv) {
  try {
    pgv::Options opts = pgv::parse_args(argc, argv);
    pgv::ProjectPaths paths = pgv::find_project_paths(argv[0]);
    if (opts.config) {
      return pgv::run_configured_runs(*opts.config, paths, argv[0]);
    }
    if (opts.self_test) {
      pgv::run_self_test(paths, argv[0]);
      return 0;
    }
    if (opts.interactive_operation) {
      return pgv::run_interactive(opts, paths, argv[0]);
    }

    auto summaries = pgv::run_baselines(opts, paths, argv[0]);
    for (const auto& summary : summaries) {
      std::cout << summary.method << ": pixels=" << summary.pixel_count
                << ", candidates=" << summary.total_candidate_pairs << ", seconds=" << std::fixed
                << std::setprecision(6) << summary.runtime_seconds
                << ", algorithm_seconds=" << summary.algorithm_seconds
                << ", task_dir=" << summary.task_dir << "\n";
    }
    return 0;
  } catch (const std::exception& err) {
    std::cerr << "error: " << err.what() << "\n";
    return 1;
  }
}
