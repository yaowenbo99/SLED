#pragma once

#include "types.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace pgv {

std::string sql_float(double value);
std::string envelope_expr(double x_min, double y_min, double x_max, double y_max);
std::string shell_join(const std::vector<std::string>& parts);
std::string timestamp_name();
std::string safe_path_component(const std::string& value);
void release_unused_memory();
std::pair<int, int> parse_resolution(const std::string& value);
Bounds parse_bbox(const std::string& value);
ProjectPaths find_project_paths(const char* argv0);

}  // namespace pgv
