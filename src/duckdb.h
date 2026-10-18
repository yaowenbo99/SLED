#pragma once

#include "types.h"

#include <duckdb.h>

#include <filesystem>
#include <string>
#include <vector>

namespace pgv {

class DuckDB {
 public:
  DuckDB();
  DuckDB(const DuckDB&) = delete;
  DuckDB& operator=(const DuckDB&) = delete;
  ~DuckDB();

  duckdb_connection connection() const { return conn_; }
  void exec(const std::string& sql) const;
  duckdb_result query(const std::string& sql) const;

 private:
  duckdb_database db_ = nullptr;
  duckdb_connection conn_ = nullptr;
};

class PreparedStatement {
 public:
  PreparedStatement(duckdb_connection conn, const std::string& sql);
  PreparedStatement(const PreparedStatement&) = delete;
  PreparedStatement& operator=(const PreparedStatement&) = delete;
  ~PreparedStatement();

  void clear();
  void bind(int index, const std::string& value);
  void bind(int index, double value);
  void bind(int index, long long value);
  void execute();
  duckdb_result execute_query();

 private:
  duckdb_prepared_statement stmt_ = nullptr;
};

std::string first_string(DuckDB& db, const std::string& sql);
void load_spatial(DuckDB& db);
void configure_method_database(DuckDB& db,
                               int threads,
                               const std::filesystem::path& temp_directory);
std::string duckdb_library_version_string();
void create_cells_table(DuckDB& db,
                        const std::filesystem::path& records_path,
                        size_t max_records);
void create_cells_table(DuckDB& db, const std::vector<CellRecord>& records);
void create_pixels_batch_table(DuckDB& db,
                               const GridSpec& grid,
                               size_t start_pixel,
                               size_t pixel_count);
std::vector<ResultRow> rows_from_query(DuckDB& db, const std::string& sql);

}  // namespace pgv
