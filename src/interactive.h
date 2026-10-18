#pragma once

#include "types.h"
#include "utils.h"

namespace pgv {

int run_interactive(const Options& opts,
                    const ProjectPaths& paths,
                    const char* argv0);

}  // namespace pgv
