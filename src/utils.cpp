#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#elif defined(__GLIBC__)
#include <malloc.h>
#endif

namespace fs = std::filesystem;

namespace pgv {

std::string sql_float(double value) {
  if (!std::isfinite(value)) {
    throw std::runtime_error("non-finite SQL float literal");
  }
  std::ostringstream out;
  out << std::setprecision(17) << value;
  std::string literal = out.str();
  if (literal.find_first_of("eE") == std::string::npos) {
    literal += "e0";
  }
  return literal;
}

std::string envelope_expr(double x_min, double y_min, double x_max, double y_max) {
  return "ST_MakeEnvelope(" + sql_float(x_min) + ", " + sql_float(y_min) + ", " +
         sql_float(x_max) + ", " + sql_float(y_max) + ")";
}

std::string shell_join(const std::vector<std::string>& parts) {
  std::ostringstream out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) {
      out << ' ';
    }
    out << parts[i];
  }
  return out.str();
}

std::string timestamp_name() {
  auto now = std::chrono::system_clock::now();
  std::time_t time = std::chrono::system_clock::to_time_t(now);
  std::tm local_time{};
#if defined(_WIN32)
  localtime_s(&local_time, &time);
#else
  localtime_r(&time, &local_time);
#endif
  std::ostringstream out;
  out << std::put_time(&local_time, "%Y%m%d-%H%M%S");
  return out.str();
}

std::string safe_path_component(const std::string& value) {
  std::string cleaned;
  for (char ch : value) {
    if (std::isalnum(static_cast<unsigned char>(ch)) || ch == '-' || ch == '_' || ch == '.') {
      cleaned.push_back(ch);
    } else {
      cleaned.push_back('_');
    }
  }
  while (!cleaned.empty() && cleaned.front() == '_') {
    cleaned.erase(cleaned.begin());
  }
  while (!cleaned.empty() && cleaned.back() == '_') {
    cleaned.pop_back();
  }
  return cleaned.empty() ? "run" : cleaned;
}

void release_unused_memory() {
#if defined(__APPLE__)
  malloc_zone_pressure_relief(nullptr, 0);
#elif defined(__GLIBC__)
  malloc_trim(0);
#endif
}

std::pair<int, int> parse_resolution(const std::string& value) {
  auto pos = value.find('x');
  if (pos == std::string::npos) {
    pos = value.find('X');
  }
  if (pos == std::string::npos) {
    throw std::runtime_error("resolution must use format XxY");
  }
  int x = std::stoi(value.substr(0, pos));
  int y = std::stoi(value.substr(pos + 1));
  if (x <= 0 || y <= 0) {
    throw std::runtime_error("resolution dimensions must be positive");
  }
  return {x, y};
}

Bounds parse_bbox(const std::string& value) {
  std::vector<double> parts;
  std::stringstream src(value);
  std::string item;
  while (std::getline(src, item, ',')) {
    parts.push_back(std::stod(item));
  }
  if (parts.size() != 4 || !(parts[0] < parts[2] && parts[1] < parts[3])) {
    throw std::runtime_error("bbox must use xmin,ymin,xmax,ymax with xmin<xmax and ymin<ymax");
  }
  return {parts[0], parts[1], parts[2], parts[3]};
}

ProjectPaths find_project_paths(const char* argv0) {
  // Discover only this package, never a neighboring checkout's dataset directory.
  std::vector<fs::path> starts;
  if (argv0 && *argv0) {
    starts.push_back(fs::absolute(argv0).parent_path());
  }
  starts.push_back(fs::current_path());
  for (const auto& start : starts) {
    for (auto candidate = fs::absolute(start); !candidate.empty(); candidate = candidate.parent_path()) {
      if (fs::exists(candidate / "src" / "sled.cpp") && fs::exists(candidate / "CMakeLists.txt")) {
        return {candidate, candidate};
      }
      if (candidate == candidate.parent_path()) break;
    }
  }
  // Explicit --records and --out-dir work even with a standalone executable.
  return {fs::current_path(), fs::current_path()};
}

}  // namespace pgv
