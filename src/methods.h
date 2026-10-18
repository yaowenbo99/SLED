#pragma once

#include "duckdb.h"
#include "april.h"
#include "types.h"

namespace pgv {

MethodExecution run_scan(DuckDB& db,
                         const MethodContext& context,
                         const ResultBatchConsumer& consumer);
MethodExecution run_spatial_join(DuckDB& db,
                                 const MethodContext& context,
                                 const ResultBatchConsumer& consumer,
                                 bool manage_candidate_transactions = true);
MethodExecution run_rtree(DuckDB& db,
                          const MethodContext& context,
                          const ResultBatchConsumer& consumer,
                          bool manage_index = true);
MethodExecution run_april_duckdb(DuckDB& db,
                                 const MethodContext& context,
                                 const ResultBatchConsumer& consumer,
                                 const AprilIndex* reusable_index = nullptr,
                                 const std::vector<std::uint8_t>* excluded_polygons = nullptr,
                                 const AprilKernelStats* reusable_stats = nullptr,
                                 bool manage_candidate_transactions = true);

}  // namespace pgv
