# SLED: Efficient Polygon-to-Grid Visualization for Spatial Data Exploration


This repository is the implementation of our research paper "SLED: Efficient Polygon-to-Grid Visualization for Spatial Data Exploration". 
Based on our theoretical analysis in this paper, 
this method can further reduce the time complexity for generating Polygon-to-Grid visualization.


## Build

Use a C++17 compiler, `make` (or CMake 3.20+), and Python 3.9+.
The program uses DuckDB **v1.5.3** with its Spatial extension,
Boost.Geometry **1.85.0**, and nlohmann/json **3.11.3**. The JSON single header is
vendored with its original notices. 
```sh
# Run from this repository's root. Downloads dependencies, not datasets.
make bootstrap
make -j2
python3 -m pip install -r requirements.txt
python3 scripts/validate.py --executable build/sled --output build/validation.json
```

DuckDB's Spatial extension is loaded by the baseline methods and self-tests.

For an offline build, provide an already prepared dependency directory containing
`duckdb/include/duckdb.h`, `duckdb/lib/libduckdb.dylib` (macOS) or
`libduckdb.so` (Linux), and `boost/include/boost/geometry.hpp`:

```sh
make THIRD_PARTY_DIR=/path/to/dependencies BUILD_DIR=/path/to/build -j2
```

The CMake build is:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```


## Quick start (no real data required)

```sh
./build/sled \
  --records tests/fixtures/sled_interactive_geometry_records.jsonl \
  --dataset synthetic --resolution 8x8 --method all --ri-resolution 8x8 \
  --compare --validation-grid --out-dir results/quick --no-progress

./build/sled --config configs/example.json
```

| Command-line method | Implementation |
|---|---|
| `SLED` | SLED, the default method |
| `scan` | Exact intersection scan, primarily a correctness reference |
| `spatial_join` | DuckDB Spatial Join (SJSJoin in the paper) |
| `rtree` | Spatial-index R-tree method (SISr in the paper) |
| `april_duckdb` | APRIL raster interval filter with DuckDB exact refinement (SJSAPRIL) |
| `all` | All five methods above |

A comma-separated `--method scan,SLED` selects a subset. `--compare` validates
method grids; it is intended for correctness checks. The paper's performance
scheduler runs one method per process, three independent processes per condition.
Its default four-method matrix excludes the expensive Scan reference; explicitly
add `--methods scan,spatial_join,rtree,april_duckdb,SLED` to include it.

`./build/sled --help` lists the CLI. The default resolution is 1600×1200. Common options include explicit `--records`,
`--resolution`, `--bbox xmin,ymin,xmax,ymax`, `--dataset-ratio`, `--threads`,
`--out-dir`, and `--run-name`. Use absolute input and output paths to run the
binary from another working directory. Automatic dataset discovery is confined
to this package's optional `dataset/processed/` directory; it never searches a
neighboring project for data.

## Data and experiments

See [EXPERIMENTS.md](docs/EXPERIMENTS.md) for the correspondence to Table 3 and
Figures 15–24, external input names, complete commands, timing boundaries,
outputs, and validation coverage. Every input line must contain `cell_id`, a
GeoJSON `geometry` (`Polygon` or `MultiPolygon`), and numeric `weight.value`.

```json
{"cell_id":"square","geometry":{"type":"Polygon","coordinates":[[[0,0],[1,0],[1,1],[0,1],[0,0]]]},"weight":{"value":2}}
```


## Layout

- `src/`, `main.cpp`: algorithms, CLI, configuration, input/output, memory, timing.
- `dataset_read_memory.cpp`: separate input materialization memory measurement.
- `scripts/`: dependency setup, validation, campaign, progressive runs, data statistics.
- `tests/fixtures/`: synthetic inputs and valid/invalid configuration examples.
- `configs/example.json`: a complete synthetic configuration.
- `third_party/nlohmann/`: preserved JSON dependency and license.
