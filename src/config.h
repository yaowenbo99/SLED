#pragma once

#include "types.h"

#include <filesystem>
#include <vector>

namespace pgv {

std::vector<Options> load_configured_runs(const std::filesystem::path& config_path);
int run_configured_runs(const std::filesystem::path& config_path,
                        const ProjectPaths& paths,
                        const char* argv0);

}  // namespace pgv
