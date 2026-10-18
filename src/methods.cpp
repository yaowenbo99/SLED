#include "methods.h"

#include "april.h"
#include "progress.h"
#include "utils.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace pgv {

namespace {

using Clock = std::chrono::steady_clock;
constexpr size_t kMaxExactRefinementPairs = 256;
constexpr size_t kMaxSpatialJoinBatchPixels = 8192;

double elapsed_seconds(Clock::time_point start) {
  return std::chrono::duration<double>(Clock::now() - start).count();
}

void add_timing(std::map<std::string, double>& timings,
                const std::string& key,
                Clock::time_point start) {
  timings[key] += elapsed_seconds(start);
}

size_t batch_capacity(const MethodContext& context) {
  if (context.execution_mode == ExecutionMode::Materialized) {
    return std::max<size_t>(1, context.grid.pixel_count);
  }
  return std::min(context.grid.max_batch_pixels(context.batch_rows),
                  kMaxStreamingBatchPixels);
}

void add_batch_metadata(MethodExecution& execution,
                        const MethodContext& context,
                        size_t capacity = 0) {
  if (capacity == 0) {
    capacity = batch_capacity(context);
  }
  size_t batches = (context.grid.pixel_count + capacity - 1) / capacity;
  execution.extra["batch_rows"] =
      context.execution_mode == ExecutionMode::Materialized
          ? "all"
          : std::to_string(context.batch_rows);
  execution.extra["requested_batch_rows"] =
      context.execution_mode == ExecutionMode::Materialized
          ? "all"
          : std::to_string(context.batch_rows);
  execution.extra["effective_batch_pixels"] =
      std::to_string(std::min(capacity, context.grid.pixel_count));
  execution.extra["batch_count"] = std::to_string(batches);
  execution.extra["peak_batch_pixels"] =
      std::to_string(std::min(capacity, context.grid.pixel_count));
  execution.extra["memory_strategy"] =
      context.execution_mode == ExecutionMode::Materialized
          ? "fully_materialized_in_memory"
          : "bounded_streaming_with_duckdb_spill";
}

const PixelRow& materialized_pixel(const MethodContext& context, size_t pixel_id) {
  if (!context.materialized_pixels || pixel_id >= context.materialized_pixels->size()) {
    throw std::runtime_error("materialized execution is missing output pixels");
  }
  return (*context.materialized_pixels)[pixel_id];
}

PixelRow pixel_at(const MethodContext& context, size_t pixel_id) {
  return context.execution_mode == ExecutionMode::Materialized
             ? materialized_pixel(context, pixel_id)
             : context.grid.pixel_at(pixel_id);
}

std::string scan_sql(const std::string& envelope) {
  return R"SQL(
    WITH query_pixel AS (
      SELECT )SQL" + envelope + R"SQL( AS geom
    )
    SELECT
      COALESCE(SUM(
        CASE
          WHEN ST_Intersects(c.geom, q.geom)
          THEN c.weight * ST_Area(ST_Intersection(c.geom, q.geom))
          ELSE 0.0
        END
      ), 0.0) AS density,
      COUNT(*) AS candidate_count
    FROM cells AS c
    CROSS JOIN query_pixel AS q
  )SQL";
}

void verify_scan_plan(DuckDB& db, const PixelRow& pixel) {
  std::string envelope =
      envelope_expr(pixel.x_min, pixel.y_min, pixel.x_max, pixel.y_max);
  std::string plan = first_string(db, "EXPLAIN " + scan_sql(envelope));
  if (plan.find("RTREE_INDEX_SCAN") != std::string::npos ||
      plan.find("SPATIAL_JOIN") != std::string::npos) {
    throw std::runtime_error(
        "scan baseline unexpectedly used a spatial index or spatial join");
  }
}

void verify_spatial_join_plan(DuckDB& db) {
  std::string plan = first_string(db, R"SQL(
    EXPLAIN
    SELECT COUNT(*)
    FROM cells AS c
    JOIN pixels AS p
      ON ST_Intersects(c.geom, p.geom);
  )SQL");
  if (plan.find("SPATIAL_JOIN") == std::string::npos) {
    throw std::runtime_error(
        "DuckDB plan did not contain SPATIAL_JOIN for spatial_join baseline");
  }
}

void verify_rtree_plan(DuckDB& db, const PixelRow& pixel) {
  std::string env = envelope_expr(pixel.x_min, pixel.y_min, pixel.x_max, pixel.y_max);
  std::string plan = first_string(
      db, "EXPLAIN SELECT COUNT(*) FROM cells WHERE ST_Intersects(geom, " + env + ");");
  if (plan.find("RTREE_INDEX_SCAN") == std::string::npos) {
    throw std::runtime_error(
        "DuckDB plan did not contain RTREE_INDEX_SCAN for rtree baseline");
  }
}

void load_candidate_chunk(
    DuckDB& db,
    const std::vector<std::pair<long long, long long>>& pairs,
    size_t begin,
    size_t end,
    bool manage_transaction = true) {
  if (begin > end || end > pairs.size()) {
    throw std::runtime_error("invalid APRIL candidate chunk");
  }
  db.exec("DROP TABLE IF EXISTS refinement_candidate_pairs;");
  db.exec("CREATE TEMP TABLE refinement_candidate_pairs("
          "polygon_index BIGINT, pixel_id BIGINT);");
  PreparedStatement insert(db.connection(),
                           "INSERT INTO refinement_candidate_pairs VALUES (?, ?);");
  if (manage_transaction) db.exec("BEGIN TRANSACTION;");
  try {
    for (size_t index = begin; index < end; ++index) {
      const auto& [polygon_index, pixel_id] = pairs[index];
      insert.clear();
      insert.bind(1, polygon_index);
      insert.bind(2, pixel_id);
      insert.execute();
    }
    if (manage_transaction) db.exec("COMMIT;");
  } catch (...) {
    if (manage_transaction) {
      try {
        db.exec("ROLLBACK;");
      } catch (...) {
      }
    }
    throw;
  }
}

void load_candidate_chunk(
    DuckDB& db,
    const std::vector<std::pair<long long, long long>>& pairs,
    bool manage_transaction = true) {
  load_candidate_chunk(db, pairs, 0, pairs.size(), manage_transaction);
}

std::vector<ResultRow> refine_candidate_chunk(DuckDB& db) {
  return rows_from_query(db, R"SQL(
    SELECT
      p.u,
      p.v,
      p.pixel_id,
      SUM(c.weight * ST_Area(ST_Intersection(c.geom, p.geom))) AS density,
      COUNT(*) AS candidate_count
    FROM refinement_candidate_pairs AS cp
    JOIN cells AS c
      ON c.polygon_index = cp.polygon_index
    JOIN pixels AS p
      ON p.pixel_id = cp.pixel_id
    GROUP BY p.u, p.v, p.pixel_id
    ORDER BY p.pixel_id;
  )SQL");
}

std::vector<std::pair<long long, long long>> candidate_pairs_from_query(
    DuckDB& db,
    const std::string& sql) {
  duckdb_result result = db.query(sql);
  if (duckdb_column_count(&result) != 2) {
    duckdb_destroy_result(&result);
    throw std::runtime_error("candidate query did not return two columns");
  }
  idx_t row_count = duckdb_row_count(&result);
  std::vector<std::pair<long long, long long>> pairs;
  pairs.reserve(static_cast<size_t>(row_count));
  for (idx_t row = 0; row < row_count; ++row) {
    pairs.emplace_back(duckdb_value_int64(&result, 0, row),
                       duckdb_value_int64(&result, 1, row));
  }
  duckdb_destroy_result(&result);
  return pairs;
}

std::pair<long long, long long> audit_april_batch(DuckDB& db) {
  duckdb_result result = db.query(R"SQL(
    WITH exact_pairs AS (
      SELECT c.polygon_index, p.pixel_id
      FROM cells AS c
      JOIN pixels AS p
        ON ST_Intersects(c.geom, p.geom)
    ), candidate_batch AS (
      SELECT cp.polygon_index, cp.pixel_id
      FROM refinement_candidate_pairs AS cp
      JOIN pixels AS p
        ON p.pixel_id = cp.pixel_id
    ), missing AS (
      SELECT COUNT(*) AS count
      FROM exact_pairs AS e
      LEFT JOIN candidate_batch AS cp
        ON cp.polygon_index = e.polygon_index
       AND cp.pixel_id = e.pixel_id
      WHERE cp.polygon_index IS NULL
    ), false_positives AS (
      SELECT COUNT(*) AS count
      FROM candidate_batch AS cp
      LEFT JOIN exact_pairs AS e
        ON e.polygon_index = cp.polygon_index
       AND e.pixel_id = cp.pixel_id
      WHERE e.polygon_index IS NULL
    )
    SELECT missing.count, false_positives.count
    FROM missing, false_positives;
  )SQL");
  long long missing = duckdb_value_int64(&result, 0, 0);
  long long false_positives = duckdb_value_int64(&result, 1, 0);
  duckdb_destroy_result(&result);
  return {missing, false_positives};
}

}  // namespace

MethodExecution run_scan(DuckDB& db,
                         const MethodContext& context,
                         const ResultBatchConsumer& consumer) {
  MethodExecution execution;
  add_batch_metadata(execution, context);
  execution.extra["database_isolation"] = "method";
  execution.extra["scan_query_mode"] = "per_pixel_full_table_scan";
  execution.extra["scan_spatial_index"] = "none";
  execution.extra["scan_polygons_per_pixel"] = std::to_string(context.cell_count);
  PhaseProgress phases("scan", 2, context.show_progress);
  phases.step("scan polygons for each pixel");
  ProgressBar progress("scan pixels", context.grid.pixel_count, context.show_progress);
  size_t capacity = batch_capacity(context);
  bool plan_verified = false;
  db.exec("DROP INDEX IF EXISTS cells_geom_rtree;");
  PreparedStatement query(db.connection(),
                          scan_sql("ST_MakeEnvelope(?, ?, ?, ?)"));

  for (size_t start_pixel = 0; start_pixel < context.grid.pixel_count;
       start_pixel += capacity) {
    size_t count = std::min(capacity, context.grid.pixel_count - start_pixel);
    std::vector<ResultRow> rows;
    rows.reserve(count);
    for (size_t pixel_id = start_pixel; pixel_id < start_pixel + count; ++pixel_id) {
      auto metadata_start = Clock::now();
      PixelRow pixel = pixel_at(context, pixel_id);
      add_timing(execution.timing_seconds, "scan_pixel_metadata_seconds", metadata_start);
      if (context.verify_plan && !plan_verified) {
        verify_scan_plan(db, pixel);
        plan_verified = true;
      }

      auto query_start = Clock::now();
      query.clear();
      query.bind(1, pixel.x_min);
      query.bind(2, pixel.y_min);
      query.bind(3, pixel.x_max);
      query.bind(4, pixel.y_max);
      duckdb_result result = query.execute_query();
      if (duckdb_row_count(&result) != 1 || duckdb_column_count(&result) != 2) {
        duckdb_destroy_result(&result);
        throw std::runtime_error("scan query did not return one aggregate row");
      }
      double density = duckdb_value_double(&result, 0, 0);
      long long candidate_count = duckdb_value_int64(&result, 1, 0);
      duckdb_destroy_result(&result);
      add_timing(execution.timing_seconds, "scan_polygon_queries_seconds", query_start);
      if (candidate_count != static_cast<long long>(context.cell_count)) {
        throw std::runtime_error(
            "scan did not evaluate every polygon for pixel " +
            std::to_string(pixel.pixel_id) + ": expected " +
            std::to_string(context.cell_count) + ", got " +
            std::to_string(candidate_count));
      }
      rows.push_back(
          {pixel.u, pixel.v, pixel.pixel_id, density, candidate_count});
      progress.update(pixel_id + 1);
    }
    consumer(rows);
    release_unused_memory();
  }
  progress.finish();
  phases.step("cleanup");
  execution.timing_seconds["scan_cleanup_seconds"] += 0.0;
  return execution;
}

MethodExecution run_spatial_join(DuckDB& db,
                                 const MethodContext& context,
                                 const ResultBatchConsumer& consumer,
                                 bool manage_candidate_transactions) {
  MethodExecution execution;
  size_t capacity = batch_capacity(context);
  if (context.execution_mode == ExecutionMode::Streaming) {
    capacity = std::min(capacity, kMaxSpatialJoinBatchPixels);
  }
  add_batch_metadata(execution, context, capacity);
  execution.extra["database_isolation"] = "method";
  execution.extra["spatial_join_batch_cap_pixels"] =
      context.execution_mode == ExecutionMode::Streaming
          ? std::to_string(kMaxSpatialJoinBatchPixels)
          : "all";
  execution.extra["spatial_join_refinement_chunk_pairs"] =
      std::to_string(kMaxExactRefinementPairs);
  PhaseProgress phases("spatial_join", 2, context.show_progress);
  phases.step("enumerate and refine pixel batches");
  ProgressBar progress("spatial_join pixels", context.grid.pixel_count,
                       context.show_progress);
  bool plan_verified = false;
  size_t refinement_chunk_count = 0;

  for (size_t start_pixel = 0; start_pixel < context.grid.pixel_count;
       start_pixel += capacity) {
    size_t count = std::min(capacity, context.grid.pixel_count - start_pixel);
    auto start = Clock::now();
    create_pixels_batch_table(db, context.grid, start_pixel, count);
    add_timing(execution.timing_seconds,
               "spatial_join_pixel_batch_materialization_seconds", start);
    if (context.verify_plan && !plan_verified) {
      verify_spatial_join_plan(db);
      plan_verified = true;
    }

    start = Clock::now();
    auto candidates = candidate_pairs_from_query(db, R"SQL(
      SELECT c.polygon_index, p.pixel_id
      FROM cells AS c
      JOIN pixels AS p
        ON ST_Intersects(c.geom, p.geom)
      ORDER BY p.pixel_id, c.polygon_index;
    )SQL");
    add_timing(execution.timing_seconds,
               "spatial_join_candidate_enumeration_seconds", start);

    std::vector<ResultRow> rows;
    rows.reserve(count);
    for (size_t pixel_id = start_pixel; pixel_id < start_pixel + count;
         ++pixel_id) {
      PixelRow pixel = pixel_at(context, pixel_id);
      rows.push_back({pixel.u, pixel.v, pixel.pixel_id, 0.0, 0});
    }
    for (size_t candidate_start = 0;
         candidate_start < candidates.size();
         candidate_start += kMaxExactRefinementPairs) {
      size_t candidate_end = std::min(
          candidate_start + kMaxExactRefinementPairs, candidates.size());
      start = Clock::now();
      load_candidate_chunk(db, candidates, candidate_start, candidate_end,
                           manage_candidate_transactions);
      auto refined = refine_candidate_chunk(db);
      add_timing(execution.timing_seconds,
                 "spatial_join_aggregate_refine_seconds", start);
      for (const ResultRow& row : refined) {
        if (row.pixel_id < static_cast<long long>(start_pixel) ||
            row.pixel_id >= static_cast<long long>(start_pixel + count)) {
          throw std::runtime_error(
              "spatial join refinement returned a pixel outside the active batch");
        }
        ResultRow& destination =
            rows[static_cast<size_t>(row.pixel_id) - start_pixel];
        destination.density += row.density;
        destination.candidate_count += row.candidate_count;
      }
      ++refinement_chunk_count;
    }
    consumer(rows);
    auto cleanup_start = Clock::now();
    std::vector<std::pair<long long, long long>>().swap(candidates);
    db.exec("DROP TABLE IF EXISTS refinement_candidate_pairs;");
    db.exec("DROP TABLE IF EXISTS pixels;");
    release_unused_memory();
    add_timing(execution.timing_seconds, "spatial_join_cleanup_seconds",
               cleanup_start);
    progress.update(start_pixel + count);
  }
  progress.finish();
  phases.step("cleanup");
  execution.extra["spatial_join_refinement_chunk_count"] =
      std::to_string(refinement_chunk_count);
  return execution;
}

MethodExecution run_rtree(DuckDB& db,
                          const MethodContext& context,
                          const ResultBatchConsumer& consumer,
                          bool manage_index) {
  MethodExecution execution;
  add_batch_metadata(execution, context);
  PhaseProgress phases("rtree", 3, context.show_progress);
  phases.step(manage_index ? "build R-tree index" : "reuse R-tree index");
  auto start = Clock::now();
  if (manage_index) {
    db.exec("DROP INDEX IF EXISTS cells_geom_rtree;");
    db.exec("CREATE INDEX cells_geom_rtree ON cells USING RTREE (geom);");
    add_timing(execution.timing_seconds, "rtree_index_build_seconds", start);
  }

  if (context.verify_plan && context.grid.pixel_count > 0) {
    verify_rtree_plan(db, pixel_at(context, 0));
  }

  phases.step("query pixels");
  ProgressBar progress("rtree pixels", context.grid.pixel_count, context.show_progress);
  std::vector<ResultRow> output_batch;
  output_batch.reserve(batch_capacity(context));
  for (size_t pixel_id = 0; pixel_id < context.grid.pixel_count; ++pixel_id) {
    auto metadata_start = Clock::now();
    PixelRow pixel = pixel_at(context, pixel_id);
    add_timing(execution.timing_seconds, "rtree_pixel_metadata_seconds", metadata_start);
    std::string env =
        envelope_expr(pixel.x_min, pixel.y_min, pixel.x_max, pixel.y_max);
    auto query_start = Clock::now();
    duckdb_result result = db.query(
        "SELECT COALESCE(SUM(weight * ST_Area(ST_Intersection(geom, " + env +
        "))), 0.0), COUNT(*) FROM cells WHERE ST_Intersects(geom, " + env + ");");
    double density = duckdb_value_double(&result, 0, 0);
    long long candidate_count = duckdb_value_int64(&result, 1, 0);
    duckdb_destroy_result(&result);
    add_timing(execution.timing_seconds, "rtree_pixel_queries_seconds", query_start);
    output_batch.push_back(
        {pixel.u, pixel.v, pixel.pixel_id, density, candidate_count});
    if (output_batch.size() == output_batch.capacity()) {
      consumer(output_batch);
      output_batch.clear();
      release_unused_memory();
    }
    progress.update(pixel_id + 1);
  }
  if (!output_batch.empty()) {
    consumer(output_batch);
  }
  progress.finish();

  phases.step(manage_index ? "drop R-tree index" : "retain R-tree index");
  auto cleanup_start = Clock::now();
  if (manage_index) {
    db.exec("DROP INDEX IF EXISTS cells_geom_rtree;");
  }
  add_timing(execution.timing_seconds, "rtree_cleanup_seconds", cleanup_start);
  return execution;
}

MethodExecution run_april_duckdb(DuckDB& db,
                                 const MethodContext& context,
                                 const ResultBatchConsumer& consumer,
                                 const AprilIndex* reusable_index,
                                 const std::vector<std::uint8_t>* excluded_polygons,
                                 const AprilKernelStats* reusable_stats,
                                 bool manage_candidate_transactions) {
  MethodExecution execution;
  add_batch_metadata(execution, context);
  execution.extra["ri_variant"] = "april_duckdb";
  execution.extra["ri_resolution_cli"] =
      std::to_string(context.ri_resolution.first) + "x" +
      std::to_string(context.ri_resolution.second);
  execution.extra["april_candidate_source"] = "internal_json_dataset";
  execution.extra["april_refinement_mode"] =
      "intersection_area_without_precheck";
  execution.extra["april_candidate_verification"] =
      context.verify_april_candidates ? "enabled" : "disabled";
  execution.extra["april_refinement_chunk_pairs"] =
      std::to_string(kMaxExactRefinementPairs);
  PhaseProgress phases("april_duckdb", 3, context.show_progress);
  AprilKernelStats stats = reusable_stats ? *reusable_stats : AprilKernelStats{};
  AprilKernelTimings kernel_timings;

  phases.step(reusable_index ? "reuse polygon ALL/F intervals"
                             : "build polygon ALL/F intervals");
  std::optional<AprilIndex> owned_index;
  if (!reusable_index) {
    owned_index.emplace(AprilIndex::build(context, stats, kernel_timings));
    reusable_index = &*owned_index;
  }
  const AprilIndex& index = *reusable_index;

  phases.step("filter and refine pixel batches");
  ProgressBar progress("april_duckdb pixels", context.grid.pixel_count,
                       context.show_progress);
  size_t capacity = batch_capacity(context);
  size_t refinement_chunk_count = 0;

  for (size_t start_pixel = 0; start_pixel < context.grid.pixel_count;
       start_pixel += capacity) {
    size_t count = std::min(capacity, context.grid.pixel_count - start_pixel);
    auto start = Clock::now();
    create_pixels_batch_table(db, context.grid, start_pixel, count);
    add_timing(execution.timing_seconds,
               "april_pixel_batch_materialization_seconds", start);

    AprilBatchCandidates candidates = index.candidates_for_batch(
        context.grid, start_pixel, count, stats, kernel_timings,
        excluded_polygons);
    if (context.verify_april_candidates) {
      start = Clock::now();
      load_candidate_chunk(db, candidates.pairs,
                           manage_candidate_transactions);
      auto [missing, false_positives] = audit_april_batch(db);
      stats.missing_exact_pairs += missing;
      stats.false_positive_pairs += false_positives;
      add_timing(execution.timing_seconds,
                 "april_candidate_verification_seconds", start);
      if (missing > 0) {
        throw std::runtime_error(
            "APRIL candidate verification found " + std::to_string(missing) +
            " missing intersecting pairs in the current pixel batch");
      }
    }

    std::vector<ResultRow> rows;
    rows.reserve(count);
    for (size_t pixel_id = start_pixel; pixel_id < start_pixel + count;
         ++pixel_id) {
      PixelRow pixel = pixel_at(context, pixel_id);
      rows.push_back(
          {pixel.u, pixel.v, pixel.pixel_id, 0.0, 0});
    }
    for (size_t candidate_start = 0;
         candidate_start < candidates.pairs.size();
         candidate_start += kMaxExactRefinementPairs) {
      size_t candidate_end = std::min(
          candidate_start + kMaxExactRefinementPairs,
          candidates.pairs.size());
      start = Clock::now();
      load_candidate_chunk(
          db, candidates.pairs, candidate_start, candidate_end,
          manage_candidate_transactions);
      add_timing(execution.timing_seconds,
                 "april_candidate_loading_seconds", start);

      start = Clock::now();
      auto refined = refine_candidate_chunk(db);
      add_timing(execution.timing_seconds,
                 "april_intersection_area_seconds", start);
      for (const ResultRow& row : refined) {
        if (row.pixel_id < static_cast<long long>(start_pixel) ||
            row.pixel_id >= static_cast<long long>(start_pixel + count)) {
          throw std::runtime_error(
              "APRIL refinement returned a pixel outside the active batch");
        }
        ResultRow& destination =
            rows[static_cast<size_t>(row.pixel_id) - start_pixel];
        destination.density += row.density;
        destination.candidate_count += row.candidate_count;
      }
      ++refinement_chunk_count;
    }

    auto cleanup_start = Clock::now();
    std::vector<std::pair<long long, long long>>().swap(candidates.pairs);
    db.exec("DROP TABLE IF EXISTS refinement_candidate_pairs;");
    db.exec("DROP TABLE IF EXISTS pixels;");
    release_unused_memory();
    add_timing(execution.timing_seconds, "april_cleanup_seconds", cleanup_start);
    consumer(rows);
    progress.update(start_pixel + count);
  }
  progress.finish();

  phases.step("cleanup");
  auto cleanup_start = Clock::now();
  db.exec("DROP TABLE IF EXISTS refinement_candidate_pairs;");
  add_timing(execution.timing_seconds, "april_cleanup_seconds", cleanup_start);

  execution.extra["april_dataset_fingerprint"] = context.dataset_fingerprint;
  execution.extra["april_input_polygons"] =
      std::to_string(stats.input_polygons);
  execution.extra["april_polygon_all_cells"] =
      std::to_string(stats.polygon_all_cells);
  execution.extra["april_polygon_full_cells"] =
      std::to_string(stats.polygon_full_cells);
  execution.extra["april_polygon_all_intervals"] =
      std::to_string(stats.polygon_all_intervals);
  execution.extra["april_polygon_full_intervals"] =
      std::to_string(stats.polygon_full_intervals);
  execution.extra["april_pixel_approximations"] =
      std::to_string(stats.pixel_approximations);
  execution.extra["april_pixel_all_intervals"] =
      std::to_string(stats.pixel_all_intervals);
  execution.extra["april_pixel_full_intervals"] =
      std::to_string(stats.pixel_full_intervals);
  execution.extra["april_mbr_candidates"] =
      std::to_string(stats.mbr_candidates);
  execution.extra["april_rejected_pairs"] =
      std::to_string(stats.rejected_pairs);
  execution.extra["april_accepted_pairs"] =
      std::to_string(stats.accepted_pairs);
  execution.extra["april_inconclusive_pairs"] =
      std::to_string(stats.inconclusive_pairs);
  std::uint64_t retained_pairs = stats.accepted_pairs + stats.inconclusive_pairs;
  execution.extra["april_retained_pairs"] = std::to_string(retained_pairs);
  execution.extra["april_total_candidates"] =
      std::to_string(retained_pairs);
  execution.extra["april_refinement_chunk_count"] =
      std::to_string(refinement_chunk_count);
  execution.extra["april_raster_resolution"] =
      std::to_string(index.resolution()) + "x" +
      std::to_string(index.resolution());
  execution.extra["april_hilbert_order"] =
      std::to_string(index.hilbert_order());
  const Bounds& domain = index.domain();
  execution.extra["april_domain"] =
      std::to_string(domain.x_min) + "," + std::to_string(domain.y_min) +
      "," + std::to_string(domain.x_max) + "," +
      std::to_string(domain.y_max);
  if (context.verify_april_candidates) {
    execution.extra["april_missing_exact_pairs"] =
        std::to_string(stats.missing_exact_pairs);
    execution.extra["april_false_positive_pairs"] =
        std::to_string(stats.false_positive_pairs);
  }
  execution.timing_seconds["april_geometry_adaptation_seconds"] =
      kernel_timings.geometry_adaptation_seconds;
  execution.timing_seconds["april_polygon_approximation_seconds"] =
      kernel_timings.polygon_approximation_seconds;
  execution.timing_seconds["april_pixel_approximation_seconds"] =
      kernel_timings.pixel_approximation_seconds;
  execution.timing_seconds["april_mbr_enumeration_seconds"] =
      kernel_timings.mbr_enumeration_seconds;
  execution.timing_seconds["april_interval_filtering_seconds"] =
      kernel_timings.interval_filter_seconds;
  return execution;
}

}  // namespace pgv
