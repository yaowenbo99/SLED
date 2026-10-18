#pragma once

#include "types.h"

#include <vector>

namespace pgv {

std::vector<RunSummary> run_baselines(const Options& opts, const ProjectPaths& paths, const char* argv0);
void run_self_test(const ProjectPaths& paths, const char* argv0);

}  // namespace pgv
