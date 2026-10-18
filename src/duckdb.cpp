#include "duckdb.h"

#include "io.h"
#include "utils.h"

#include <functional>
#include <filesystem>
#include <stdexcept>

namespace pgv {

DuckDB::DuckDB() {
  if (duckdb_open(nullptr, &db_) == DuckDBError) {
    throw std::runtime_error("failed to open in-memory DuckDB database");
  }
  if (duckdb_connect(db_, &conn_) == DuckDBError) {
    duckdb_close(&db_);
    throw std::runtime_error("failed to connect to DuckDB database");
  }
}

DuckDB::~DuckDB() {
  if (conn_) {
    duckdb_disconnect(&conn_);
  }
  if (db_) {
    duckdb_close(&db_);
  }
}

void DuckDB::exec(const std::string& sql) const {
  duckdb_result result;
  if (duckdb_query(conn_, sql.c_str(), &result) == DuckDBError) {
    std::string message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(message + "\nSQL:\n" + sql);
  }
  duckdb_destroy_result(&result);
}

duckdb_result DuckDB::query(const std::string& sql) const {
  duckdb_result result;
  if (duckdb_query(conn_, sql.c_str(), &result) == DuckDBError) {
    std::string message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(message + "\nSQL:\n" + sql);
  }
  return result;
}

PreparedStatement::PreparedStatement(duckdb_connection conn, const std::string& sql) {
  if (duckdb_prepare(conn, sql.c_str(), &stmt_) == DuckDBError) {
    std::string error = duckdb_prepare_error(stmt_);
    duckdb_destroy_prepare(&stmt_);
    throw std::runtime_error(error + "\nSQL:\n" + sql);
  }
}

PreparedStatement::~PreparedStatement() {
  if (stmt_) {
    duckdb_destroy_prepare(&stmt_);
  }
}

void PreparedStatement::clear() {
  if (duckdb_clear_bindings(stmt_) == DuckDBError) {
    throw std::runtime_error("failed to clear DuckDB prepared statement bindings");
  }
}

void PreparedStatement::bind(int index, const std::string& value) {
  if (duckdb_bind_varchar(stmt_, static_cast<idx_t>(index), value.c_str()) == DuckDBError) {
    throw std::runtime_error("failed to bind string parameter");
  }
}

void PreparedStatement::bind(int index, double value) {
  if (duckdb_bind_double(stmt_, static_cast<idx_t>(index), value) == DuckDBError) {
    throw std::runtime_error("failed to bind double parameter");
  }
}

void PreparedStatement::bind(int index, long long value) {
  if (duckdb_bind_int64(stmt_, static_cast<idx_t>(index), value) == DuckDBError) {
    throw std::runtime_error("failed to bind int64 parameter");
  }
}

void PreparedStatement::execute() {
  duckdb_result result = execute_query();
  duckdb_destroy_result(&result);
}

duckdb_result PreparedStatement::execute_query() {
  duckdb_result result;
  if (duckdb_execute_prepared(stmt_, &result) == DuckDBError) {
    std::string message = duckdb_result_error(&result);
    duckdb_destroy_result(&result);
    throw std::runtime_error(message);
  }
  return result;
}

std::string first_string(DuckDB& db, const std::string& sql) {
  duckdb_result result = db.query(sql);
  if (duckdb_row_count(&result) == 0 || duckdb_column_count(&result) == 0) {
    duckdb_destroy_result(&result);
    return "";
  }
  idx_t value_col = duckdb_column_count(&result) > 1 ? 1 : 0;
  char* raw = duckdb_value_varchar(&result, value_col, 0);
  std::string value = raw ? raw : "";
  if (raw) {
    duckdb_free(raw);
  }
  duckdb_destroy_result(&result);
  return value;
}

void load_spatial(DuckDB& db) {
  try {
    db.exec("LOAD spatial;");
  } catch (const std::exception&) {
    db.exec("INSTALL spatial;");
    db.exec("LOAD spatial;");
  }
}

void configure_method_database(DuckDB& db,
                               int threads,
                               const std::filesystem::path& temp_directory) {
  if (threads <= 0) {
    throw std::runtime_error("DuckDB thread count must be positive");
  }
  std::filesystem::create_directories(temp_directory);
  std::string escaped = std::filesystem::absolute(temp_directory).string();
  size_t position = 0;
  while ((position = escaped.find('\'', position)) != std::string::npos) {
    escaped.insert(position, 1, '\'');
    position += 2;
  }
  db.exec("SET threads=" + std::to_string(threads) + ";");
  db.exec("SET preserve_insertion_order=false;");
  db.exec("SET temp_directory='" + escaped + "';");
}

std::string duckdb_library_version_string() {
  const char* version = duckdb_library_version();
  return version ? version : "";
}

namespace {

void create_cells_table_impl(
    DuckDB& db,
    const std::function<void(const std::function<void(const CellRecord&)>&)>& produce) {
  db.exec("DROP TABLE IF EXISTS cells;");
  db.exec("CREATE TABLE cells(polygon_index BIGINT, cell_id VARCHAR, weight DOUBLE, geom GEOMETRY);");
  PreparedStatement insert(
      db.connection(),
      "INSERT INTO cells VALUES (?, ?, ?, ST_GeomFromGeoJSON(?));");
  db.exec("BEGIN TRANSACTION;");
  try {
    produce([&](const CellRecord& record) {
      insert.clear();
      insert.bind(1, record.polygon_index);
      insert.bind(2, record.cell_id);
      insert.bind(3, record.weight);
      insert.bind(4, record.geometry_json);
      insert.execute();
    });
    db.exec("COMMIT;");
  } catch (...) {
    try {
      db.exec("ROLLBACK;");
    } catch (...) {
    }
    throw;
  }
}

}  // namespace

void create_cells_table(DuckDB& db,
                        const std::filesystem::path& records_path,
                        size_t max_records) {
  create_cells_table_impl(db, [&](const auto& consume) {
    for_each_record(records_path, consume, max_records);
  });
}

void create_cells_table(DuckDB& db, const std::vector<CellRecord>& records) {
  create_cells_table_impl(db, [&](const auto& consume) {
    for (const auto& record : records) {
      consume(record);
    }
  });
}

void create_pixels_batch_table(DuckDB& db,
                               const GridSpec& grid,
                               size_t start_pixel,
                               size_t pixel_count) {
  if (start_pixel > grid.pixel_count || pixel_count > grid.pixel_count - start_pixel) {
    throw std::runtime_error("pixel batch is outside the configured grid");
  }
  size_t end_pixel = start_pixel + pixel_count;
  db.exec("DROP TABLE IF EXISTS pixels;");
  db.exec(R"SQL(
    CREATE TEMP TABLE pixels AS
    WITH coordinates AS (
      SELECT
        CAST(pixel_id % )SQL" + std::to_string(grid.x_res) + R"SQL( AS INTEGER) AS u,
        CAST(pixel_id // )SQL" + std::to_string(grid.x_res) + R"SQL( AS INTEGER) AS v,
        CAST(pixel_id AS BIGINT) AS pixel_id
      FROM range()SQL" + std::to_string(start_pixel) + ", " + std::to_string(end_pixel) + R"SQL() AS ids(pixel_id)
    ), bounds AS (
      SELECT
        u,
        v,
        pixel_id,
        )SQL" + sql_float(grid.bounds.x_min) + " + u * " + sql_float(grid.x_step) + R"SQL( AS x_min,
        )SQL" + sql_float(grid.bounds.y_max) + " - (v + 1) * " + sql_float(grid.y_step) + R"SQL( AS y_min,
        )SQL" + sql_float(grid.bounds.x_min) + " + (u + 1) * " + sql_float(grid.x_step) + R"SQL( AS x_max,
        )SQL" + sql_float(grid.bounds.y_max) + " - v * " + sql_float(grid.y_step) + R"SQL( AS y_max
      FROM coordinates
    )
    SELECT
      u,
      v,
      pixel_id,
      x_min,
      y_min,
      x_max,
      y_max,
      ST_MakeEnvelope(x_min, y_min, x_max, y_max) AS geom
    FROM bounds;
  )SQL");
}

std::vector<ResultRow> rows_from_query(DuckDB& db, const std::string& sql) {
  duckdb_result result = db.query(sql);
  std::vector<ResultRow> rows;
  idx_t row_count = duckdb_row_count(&result);
  rows.reserve(static_cast<size_t>(row_count));
  for (idx_t i = 0; i < row_count; ++i) {
    rows.push_back({duckdb_value_int32(&result, 0, i), duckdb_value_int32(&result, 1, i),
                    duckdb_value_int64(&result, 2, i), duckdb_value_double(&result, 3, i),
                    duckdb_value_int64(&result, 4, i)});
  }
  duckdb_destroy_result(&result);
  return rows;
}

}  // namespace pgv
