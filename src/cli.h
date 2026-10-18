#pragma once

#include "types.h"

namespace pgv {

Options parse_args(int argc, char** argv);
void normalize_and_validate_options(Options& opts);
void print_usage();

}  // namespace pgv
