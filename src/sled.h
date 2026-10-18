#pragma once

#include "types.h"

namespace pgv {

MethodExecution run_sled(const MethodContext& context,
                         const ResultBatchConsumer& consumer);

}  // namespace pgv
