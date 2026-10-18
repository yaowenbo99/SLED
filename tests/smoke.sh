#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="${1:-$ROOT/build/sled}"
APRIL_RECORDS="$ROOT/tests/fixtures/april_internal_records.jsonl"
RATIO_RECORDS="$ROOT/tests/fixtures/dataset_ratio_records.jsonl"
TEMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TEMP_DIR"' EXIT
INTERACTIVE_GEOMETRY_RECORDS="$ROOT/tests/fixtures/sled_interactive_geometry_records.jsonl"

compare_result_csv() {
  paste "$1" "$2" | awk -F '\t' '
    NR == 1 {
      if ($1 != $2) exit 1
      next
    }
    {
      split($1, left, ",")
      split($2, right, ",")
      if (left[1] != right[1] || left[2] != right[2] ||
          left[3] != right[3] || left[5] != right[5]) exit 1
      difference = left[4] - right[4]
      if (difference < 0) difference = -difference
      left_abs = left[4] < 0 ? -left[4] : left[4]
      right_abs = right[4] < 0 ? -right[4] : right[4]
      scale = left_abs > right_abs ? left_abs : right_abs
      relative = scale > 0 ? difference / scale : 0
      if (difference > 1e-9 && relative > 1e-12) exit 1
    }
  '
}

"$BIN" --self-test
"$BIN" \
  --dataset synthetic \
  --records "$RATIO_RECORDS" \
  --resolution 16x12 \
  --method scan,spatial_join \
  --compare \
  --threads 1 \
  --limit-pixels 32 \
  --run-name smoke_scan_join \
  --out-dir "$ROOT/build/results" \
  --no-progress
test -f "$ROOT/build/results/smoke_scan_join_synthetic_scan_spatial_join_streaming_16x12_ratio100/comparison.csv"
"$BIN" \
  --dataset synthetic \
  --records "$RATIO_RECORDS" \
  --resolution 16x12 \
  --method scan,rtree \
  --compare \
  --threads 1 \
  --limit-pixels 32 \
  --run-name smoke_scan_rtree \
  --out-dir "$ROOT/build/results" \
  --no-progress
test -f "$ROOT/build/results/smoke_scan_rtree_synthetic_scan_rtree_streaming_16x12_ratio100/comparison.csv"
"$BIN" \
  --dataset synthetic \
  --records "$RATIO_RECORDS" \
  --resolution 16x12 \
  --method scan,SLED \
  --compare \
  --threads 1 \
  --limit-pixels 32 \
  --run-name smoke_scan_sled \
  --out-dir "$ROOT/build/results" \
  --no-progress
test -f "$ROOT/build/results/smoke_scan_sled_synthetic_scan_SLED_streaming_16x12_ratio100/comparison.csv"
grep -q '^scan,SLED,' "$ROOT/build/results/smoke_scan_sled_synthetic_scan_SLED_streaming_16x12_ratio100/comparison.csv"
"$BIN" \
  --dataset april_internal_fixture \
  --records "$APRIL_RECORDS" \
  --bbox 0,0,2,2 \
  --resolution 2x2 \
  --method scan,april_duckdb \
  --verify-april-candidates \
  --ri-resolution 8x8 \
  --compare \
  --threads 1 \
  --run-name smoke_scan_april \
  --out-dir "$ROOT/build/results" \
  --no-progress
"$BIN" \
  --dataset synthetic \
  --records "$RATIO_RECORDS" \
  --resolution 16x12 \
  --method SLED \
  --limit-pixels 2 \
  --run-name smoke_sled_progress \
  --out-dir "$ROOT/build/results" \
  >${TEMP_DIR}/pgv-sled-progress.out 2>${TEMP_DIR}/pgv-sled-progress.err
grep -q 'SLED rows' ${TEMP_DIR}/pgv-sled-progress.err
grep -q 'SLED phases done wall_elapsed=' ${TEMP_DIR}/pgv-sled-progress.err
grep -q 'algorithm_seconds=' ${TEMP_DIR}/pgv-sled-progress.out
"$BIN" \
  --dataset synthetic \
  --records "$RATIO_RECORDS" \
  --resolution 8x4 \
  --method SLED \
  --progressive-checkpoints 25,50,75,100 \
  --run-name smoke_progressive_checkpoints \
  --out-dir "$ROOT/build/results" \
  --no-progress
progressive_dir="$ROOT/build/results/smoke_progressive_checkpoints_synthetic_SLED_streaming_8x4_ratio100"
test -f "$progressive_dir/progressive_metrics.csv"
test "$(wc -l < "$progressive_dir/progressive_metrics.csv" | tr -d ' ')" -eq 6
grep -q '^first_row,1,4,0.25,' "$progressive_dir/progressive_metrics.csv"
grep -q '^25_percent,1,4,0.25,' "$progressive_dir/progressive_metrics.csv"
grep -q '^50_percent,2,4,0.5,' "$progressive_dir/progressive_metrics.csv"
grep -q '^75_percent,3,4,0.75,' "$progressive_dir/progressive_metrics.csv"
grep -q '^100_percent,4,4,1,' "$progressive_dir/progressive_metrics.csv"
grep -q '^progressive_timing_scope=continuous_wall_clock_common_input_ready_to_completed_rows_in_memory$' \
  "$progressive_dir/run_info.txt"
awk -F= '
  /^progressive_time_to_100_percent_seconds=/ { checkpoint=$2 }
  /^computation_seconds=/ { algorithm=$2 }
  END { difference=checkpoint-algorithm; if (difference<0) difference=-difference; exit difference>1e-12 }
' "$progressive_dir/run_info.txt"
if "$BIN" --method spatial_join --progressive-checkpoints 50 --no-progress \
    >${TEMP_DIR}/pgv-invalid-progressive-method.out 2>${TEMP_DIR}/pgv-invalid-progressive-method.err; then
  echo "expected progressive checkpoints with spatial_join to fail" >&2
  exit 1
fi
if "$BIN" --method SLED --execution-mode materialized \
    --progressive-checkpoints 50 --no-progress \
    >${TEMP_DIR}/pgv-invalid-progressive-mode.out 2>${TEMP_DIR}/pgv-invalid-progressive-mode.err; then
  echo "expected progressive checkpoints in materialized mode to fail" >&2
  exit 1
fi
if "$BIN" --method scan,,rtree --no-progress >${TEMP_DIR}/pgv-invalid-empty.out 2>${TEMP_DIR}/pgv-invalid-empty.err; then
  echo "expected --method scan,,rtree to fail" >&2
  exit 1
fi
if "$BIN" --method scan,scan --no-progress >${TEMP_DIR}/pgv-invalid-duplicate.out 2>${TEMP_DIR}/pgv-invalid-duplicate.err; then
  echo "expected --method scan,scan to fail" >&2
  exit 1
fi
if "$BIN" --method all,rtree --no-progress >${TEMP_DIR}/pgv-invalid-all.out 2>${TEMP_DIR}/pgv-invalid-all.err; then
  echo "expected --method all,rtree to fail" >&2
  exit 1
fi
if "$BIN" --method spatial_join --batch-rows 0 --no-progress >${TEMP_DIR}/pgv-invalid-batch.out 2>${TEMP_DIR}/pgv-invalid-batch.err; then
  echo "expected --batch-rows 0 to fail" >&2
  exit 1
fi
if "$BIN" --method scan --execution-mode invalid --no-progress >${TEMP_DIR}/pgv-invalid-mode.out 2>${TEMP_DIR}/pgv-invalid-mode.err; then
  echo "expected invalid --execution-mode to fail" >&2
  exit 1
fi
for invalid_ratio in 0 -1 100.1 nan invalid; do
  if "$BIN" --method scan --dataset-ratio "$invalid_ratio" --no-progress \
      >${TEMP_DIR}/pgv-invalid-ratio.out 2>${TEMP_DIR}/pgv-invalid-ratio.err; then
    echo "expected --dataset-ratio $invalid_ratio to fail" >&2
    exit 1
  fi
done
if "$BIN" --interactive-operation invalid --no-progress >${TEMP_DIR}/pgv-invalid-interactive.out 2>${TEMP_DIR}/pgv-invalid-interactive.err; then
  echo "expected invalid interactive operation to fail" >&2
  exit 1
fi
if "$BIN" --interactive-operation zoom --method sled --no-progress >${TEMP_DIR}/pgv-invalid-interactive-method.out 2>${TEMP_DIR}/pgv-invalid-interactive-method.err; then
  echo "expected lowercase interactive SLED method to fail" >&2
  exit 1
fi
for incompatible in limit materialized self_test verify_plan verify_april; do
  case "$incompatible" in
    limit) arguments=(--interactive-operation zoom --limit-pixels 1) ;;
    materialized) arguments=(--interactive-operation zoom --execution-mode materialized) ;;
    self_test) arguments=(--interactive-operation zoom --self-test) ;;
    verify_plan) arguments=(--interactive-operation zoom --verify-plan) ;;
    verify_april) arguments=(--interactive-operation zoom --method april_duckdb --verify-april-candidates) ;;
  esac
  if "$BIN" "${arguments[@]}" --no-progress >${TEMP_DIR}/pgv-invalid-interactive-option.out 2>${TEMP_DIR}/pgv-invalid-interactive-option.err; then
    echo "expected interactive operation with $incompatible to fail" >&2
    exit 1
  fi
done
if "$BIN" --method april_duckdb --april-candidates removed.txt --no-progress >${TEMP_DIR}/pgv-removed-april-option.out 2>${TEMP_DIR}/pgv-removed-april-option.err; then
  echo "expected removed --april-candidates option to fail" >&2
  exit 1
fi
if "$BIN" --method april_duckdb --ri-resolution 8x4 --no-progress >${TEMP_DIR}/pgv-invalid-april-rect.out 2>${TEMP_DIR}/pgv-invalid-april-rect.err; then
  echo "expected non-square APRIL resolution to fail" >&2
  exit 1
fi
if "$BIN" --method april_duckdb --ri-resolution 6x6 --no-progress >${TEMP_DIR}/pgv-invalid-april-power.out 2>${TEMP_DIR}/pgv-invalid-april-power.err; then
  echo "expected non-power-of-two APRIL resolution to fail" >&2
  exit 1
fi
for removed_method in exact BAG SBOE sled; do
  if "$BIN" --method "$removed_method" --no-progress >${TEMP_DIR}/pgv-removed.out 2>${TEMP_DIR}/pgv-removed.err; then
    echo "expected --method $removed_method to fail" >&2
    exit 1
  fi
done

"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --resolution 2x2 \
  --method all \
  --dataset-ratio 40 \
  --ri-resolution 8x8 \
  --compare \
  --threads 1 \
  --run-name smoke_ratio \
  --out-dir "$ROOT/build/results" \
  --no-progress
"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --resolution 2x2 \
  --method all \
  --dataset-ratio 40 \
  --ri-resolution 8x8 \
  --execution-mode materialized \
  --compare \
  --threads 1 \
  --run-name smoke_ratio \
  --out-dir "$ROOT/build/results" \
  --no-progress
ratio_stream_dir="$ROOT/build/results/smoke_ratio_dataset_ratio_fixture_all_streaming_2x2_ratio40"
ratio_materialized_dir="$ROOT/build/results/smoke_ratio_dataset_ratio_fixture_all_materialized_2x2_ratio40"
test -f "$ratio_stream_dir/comparison.csv"
test -f "$ratio_materialized_dir/comparison.csv"
grep -q '^dataset_ratio_percent=40$' "$ratio_stream_dir/command_manifest.txt"
grep -q '^dataset_selection_policy=file_prefix$' "$ratio_stream_dir/command_manifest.txt"
grep -q -- '--dataset-ratio 40' "$ratio_stream_dir/command_manifest.txt"
for method in scan spatial_join rtree april_duckdb SLED; do
  compare_result_csv "$ratio_stream_dir/$method/pixels.csv" \
    "$ratio_materialized_dir/$method/pixels.csv"
done
ratio_scan_info="$ratio_stream_dir/scan/run_info.txt"
grep -q '^dataset_ratio_percent=40$' "$ratio_scan_info"
grep -q '^dataset_selection_policy=file_prefix$' "$ratio_scan_info"
grep -q '^source_cell_count=5$' "$ratio_scan_info"
grep -q '^selected_cell_count=2$' "$ratio_scan_info"
grep -q '^cell_count=2$' "$ratio_scan_info"
grep -q '^parsed_record_count=2$' "$ratio_scan_info"
grep -q '^bbox={"x_max":10.0,"x_min":0.0,"y_max":10.0,"y_min":0.0}$' \
  "$ratio_scan_info"
selected_fingerprint="$(sed -n 's/^dataset_fingerprint=//p' "$ratio_scan_info")"
source_fingerprint="$(sed -n 's/^source_dataset_fingerprint=//p' "$ratio_scan_info")"
test -n "$selected_fingerprint"
test -n "$source_fingerprint"
test "$selected_fingerprint" != "$source_fingerprint"
awk -F, 'NR > 1 { if ($5 != 2) exit 1; sum += $4 } END { if (sum != 3) exit 1 }' \
  "$ratio_stream_dir/scan/pixels.csv"
ratio_spatial_info="$ratio_stream_dir/spatial_join/run_info.txt"
grep -q '^E_avg=4$' "$ratio_spatial_info"
grep -q '^E_max=4$' "$ratio_spatial_info"
grep -q '^E_total=8$' "$ratio_spatial_info"

for ratio_case in 100 21 0.1; do
  "$BIN" \
    --dataset dataset_ratio_fixture \
    --records "$RATIO_RECORDS" \
    --resolution 1x1 \
    --method scan \
    --dataset-ratio "$ratio_case" \
    --run-name "smoke_ratio_$ratio_case" \
    --out-dir "$ROOT/build/results" \
    --no-progress
done
grep -q '^selected_cell_count=5$' \
  "$ROOT/build/results/smoke_ratio_100_dataset_ratio_fixture_scan_streaming_1x1_ratio100/run_info.txt"
grep -q '^selected_cell_count=1$' \
  "$ROOT/build/results/smoke_ratio_21_dataset_ratio_fixture_scan_streaming_1x1_ratio21/run_info.txt"
grep -q '^selected_cell_count=1$' \
  "$ROOT/build/results/smoke_ratio_0.1_dataset_ratio_fixture_scan_streaming_1x1_ratio0p1/run_info.txt"
"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --bbox 0,0,4,4 \
  --resolution 1x1 \
  --method scan \
  --dataset-ratio 40 \
  --run-name smoke_ratio_bbox \
  --out-dir "$ROOT/build/results" \
  --no-progress
grep -q '^bbox={"x_max":4.0,"x_min":0.0,"y_max":4.0,"y_min":0.0}$' \
  "$ROOT/build/results/smoke_ratio_bbox_dataset_ratio_fixture_scan_streaming_1x1_ratio40/run_info.txt"

"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --resolution 4x4 \
  --interactive-operation all \
  --run-name smoke_interactive \
  --out-dir "$ROOT/build/results" \
  --no-progress
interactive_dir="$ROOT/build/results/smoke_interactive_dataset_ratio_fixture_SLED_interactive_all_streaming_4x4_ratio100"
interactive_results="$interactive_dir/interactive_results.csv"
test -f "$interactive_results"
test -f "$interactive_dir/interactive_run_info.txt"
test "$(wc -l < "$interactive_results" | tr -d ' ')" -eq 14
awk -F, '
  function abs(value) { return value < 0 ? -value : value }
  NR == 1 { next }
  $1 == "zoom" {
    expected_width = 10 * $3
    expected_min = (10 - expected_width) / 2
    if (abs($8 - expected_min) > 1e-12 || abs($9 - expected_min) > 1e-12 ||
        abs(($10 - $8) - expected_width) > 1e-12 ||
        abs(($11 - $9) - expected_width) > 1e-12) exit 1
    ++zoom
  }
  $1 == "panning" {
    if ($4 < 1 || $4 > 5 || $8 < 0 || $9 < 0 || $10 > 10 || $11 > 10 ||
        abs(($10 - $8) - 5) > 1e-12 || abs(($11 - $9) - 5) > 1e-12) exit 1
    ++pan
  }
  $1 == "updating" {
    if ($5 != 10 && $5 != 20 && $5 != 30 && $5 != 40) exit 1
    ++update
  }
  { if ($12 < $14 || $14 <= 0 || $15 < 0 || $16 != "v2_dense_memory" || $17 <= 0 || $18 <= 0) exit 1 }
  END { if (zoom != 4 || pan != 5 || update != 4) exit 1 }
' "$interactive_results"
grep -q '^random_seed=42$' "$interactive_dir/interactive_run_info.txt"
grep -q '^trial_count=13$' "$interactive_dir/interactive_run_info.txt"
grep -q '^interactive_preprocessing_seconds=' "$interactive_dir/interactive_run_info.txt"
grep -q '^interactive_initial_density_seconds=' "$interactive_dir/interactive_run_info.txt"
grep -q '^deleted_polygon_indices=\[2\]$' \
  "$interactive_dir/updating/delete_10/run_info.txt"
grep -q '^deleted_polygon_indices=\[2,3\]$' \
  "$interactive_dir/updating/delete_40/run_info.txt"
for info in "$interactive_dir"/{zoom,panning,updating}/*/run_info.txt; do
  awk -F= '
    /^runtime_seconds=/ { runtime=$2 }
    /^algorithm_seconds=/ { algorithm=$2 }
    /^memory_peak_rss_bytes=/ { peak=$2 }
    END { if (runtime < algorithm || peak <= 0) exit 1 }
  ' "$info"
done

"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --bbox 3.75,3.75,6.25,6.25 \
  --resolution 4x4 \
  --method SLED \
  --run-name interactive_direct_zoom \
  --out-dir "$ROOT/build/results" \
  --no-progress
compare_result_csv \
  "$interactive_dir/zoom/zoom_0p25/pixels.csv" \
  "$ROOT/build/results/interactive_direct_zoom_dataset_ratio_fixture_SLED_streaming_4x4_ratio100/pixels.csv"
"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --bbox 3.7757776647726948,3.1951569692734871,8.7757776647726953,8.1951569692734871 \
  --resolution 4x4 \
  --method SLED \
  --run-name interactive_direct_pan \
  --out-dir "$ROOT/build/results" \
  --no-progress
compare_result_csv \
  "$interactive_dir/panning/pan_01/pixels.csv" \
  "$ROOT/build/results/interactive_direct_pan_dataset_ratio_fixture_SLED_streaming_4x4_ratio100/pixels.csv"
awk 'NR != 3' "$RATIO_RECORDS" > ${TEMP_DIR}/pgv-interactive-delete10.jsonl
"$BIN" \
  --dataset interactive_delete10 \
  --records ${TEMP_DIR}/pgv-interactive-delete10.jsonl \
  --bbox 0,0,10,10 \
  --resolution 4x4 \
  --method SLED \
  --run-name interactive_direct_delete10 \
  --out-dir "$ROOT/build/results" \
  --no-progress
compare_result_csv \
  "$interactive_dir/updating/delete_10/pixels.csv" \
  "$ROOT/build/results/interactive_direct_delete10_interactive_delete10_SLED_streaming_4x4_ratio100/pixels.csv"
awk 'NR != 3 && NR != 4' "$RATIO_RECORDS" > ${TEMP_DIR}/pgv-interactive-delete40.jsonl
"$BIN" \
  --dataset interactive_delete40 \
  --records ${TEMP_DIR}/pgv-interactive-delete40.jsonl \
  --bbox 0,0,10,10 \
  --resolution 4x4 \
  --method SLED \
  --run-name interactive_direct_delete40 \
  --out-dir "$ROOT/build/results" \
  --no-progress
compare_result_csv \
  "$interactive_dir/updating/delete_40/pixels.csv" \
  "$ROOT/build/results/interactive_direct_delete40_interactive_delete40_SLED_streaming_4x4_ratio100/pixels.csv"

"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --bbox 0,0,4,4 \
  --resolution 2x2 \
  --method SLED \
  --interactive-operation zoom \
  --dataset-ratio 40 \
  --run-name smoke_interactive_bbox \
  --out-dir "$ROOT/build/results" \
  --no-progress
interactive_bbox_dir="$ROOT/build/results/smoke_interactive_bbox_dataset_ratio_fixture_SLED_interactive_zoom_streaming_2x2_ratio40"
grep -q '^selected_cell_count=2$' "$interactive_bbox_dir/interactive_run_info.txt"
grep -q '^bbox={"x_max":4.0,"x_min":0.0,"y_max":4.0,"y_min":0.0}$' \
  "$interactive_bbox_dir/zoom/zoom_1/run_info.txt"

"$BIN" \
  --dataset sled_interactive_geometry \
  --records "$INTERACTIVE_GEOMETRY_RECORDS" \
  --bbox 0,0,4,4 \
  --resolution 4x4 \
  --interactive-operation updating \
  --run-name smoke_interactive_geometry \
  --out-dir "$ROOT/build/results" \
  --no-progress
interactive_geometry_dir="$ROOT/build/results/smoke_interactive_geometry_sled_interactive_geometry_SLED_interactive_updating_streaming_4x4_ratio100"
grep -q '^deleted_polygon_indices=\[2\]$' \
  "$interactive_geometry_dir/updating/delete_10/run_info.txt"
awk 'NR != 3' "$INTERACTIVE_GEOMETRY_RECORDS" > ${TEMP_DIR}/pgv-interactive-geometry-delete10.jsonl
"$BIN" \
  --dataset sled_interactive_geometry_delete10 \
  --records ${TEMP_DIR}/pgv-interactive-geometry-delete10.jsonl \
  --bbox 0,0,4,4 \
  --resolution 4x4 \
  --method SLED \
  --run-name interactive_geometry_direct_delete10 \
  --out-dir "$ROOT/build/results" \
  --no-progress
compare_result_csv \
  "$interactive_geometry_dir/updating/delete_10/pixels.csv" \
  "$ROOT/build/results/interactive_geometry_direct_delete10_sled_interactive_geometry_delete10_SLED_streaming_4x4_ratio100/pixels.csv"

"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --resolution 2x2 \
  --interactive-operation all \
  --method all \
  --compare \
  --threads 1 \
  --batch-rows 1 \
  --run-name smoke_interactive_methods \
  --out-dir "$ROOT/build/results" \
  --no-progress
interactive_summary="$ROOT/build/results/smoke_interactive_methods_dataset_ratio_fixture_all_interactive_comparison_all_streaming_2x2_ratio100"
test -f "$interactive_summary/interactive_results.csv"
test -f "$interactive_summary/interactive_comparison.csv"
test -f "$interactive_summary/command_manifest.txt"
test "$(wc -l < "$interactive_summary/interactive_results.csv" | tr -d ' ')" -eq 66
test "$(wc -l < "$interactive_summary/interactive_comparison.csv" | tr -d ' ')" -eq 66
head -n 1 "$interactive_summary/interactive_results.csv" | grep -q '^method,operation,trial_id,'
awk -F, 'NR > 1 && $10 != "true" { exit 1 }' \
  "$interactive_summary/interactive_comparison.csv"
for method in scan spatial_join rtree april_duckdb SLED; do
  method_dir="$ROOT/build/results/smoke_interactive_methods_dataset_ratio_fixture_${method}_interactive_all_streaming_2x2_ratio100"
  test -f "$method_dir/interactive_results.csv"
  test -f "$method_dir/interactive_run_info.txt"
  test "$(wc -l < "$method_dir/interactive_results.csv" | tr -d ' ')" -eq 14
  grep -q '^interactive_preprocessing_seconds=' "$method_dir/interactive_run_info.txt"
done
rtree_interactive_dir="$ROOT/build/results/smoke_interactive_methods_dataset_ratio_fixture_rtree_interactive_all_streaming_2x2_ratio100"
grep -q '^interactive_rtree_index_build_seconds=' "$rtree_interactive_dir/interactive_run_info.txt"
if grep -q '^timing_rtree_index_build_seconds=' "$rtree_interactive_dir"/{zoom,panning,updating}/*/run_info.txt; then
  echo "interactive R-tree trial unexpectedly rebuilt its index" >&2
  exit 1
fi
april_interactive_dir="$ROOT/build/results/smoke_interactive_methods_dataset_ratio_fixture_april_duckdb_interactive_all_streaming_2x2_ratio100"
grep -q '^interactive_april_index_build_seconds=' "$april_interactive_dir/interactive_run_info.txt"
scan_interactive_dir="$ROOT/build/results/smoke_interactive_methods_dataset_ratio_fixture_scan_interactive_all_streaming_2x2_ratio100"
awk -F, 'NR > 1 && $5 != 4 { exit 1 }' \
  "$scan_interactive_dir/updating/delete_10/pixels.csv"
awk -F, 'NR > 1 && $5 != 3 { exit 1 }' \
  "$scan_interactive_dir/updating/delete_40/pixels.csv"
"$BIN" \
  --dataset interactive_delete10 \
  --records ${TEMP_DIR}/pgv-interactive-delete10.jsonl \
  --bbox 0,0,10,10 \
  --resolution 2x2 \
  --method spatial_join \
  --run-name interactive_spatial_direct_delete10 \
  --out-dir "$ROOT/build/results" \
  --no-progress
spatial_interactive_dir="$ROOT/build/results/smoke_interactive_methods_dataset_ratio_fixture_spatial_join_interactive_all_streaming_2x2_ratio100"
compare_result_csv \
  "$spatial_interactive_dir/updating/delete_10/pixels.csv" \
  "$ROOT/build/results/interactive_spatial_direct_delete10_interactive_delete10_spatial_join_streaming_2x2_ratio100/pixels.csv"
"$BIN" \
  --dataset synthetic \
  --records "$RATIO_RECORDS" \
  --resolution 16x12 \
  --method spatial_join \
  --limit-pixels 32 \
  --batch-rows 1 \
  --run-name smoke_batch_1 \
  --out-dir "$ROOT/build/results" \
  --no-progress
"$BIN" \
  --dataset synthetic \
  --records "$RATIO_RECORDS" \
  --resolution 16x12 \
  --method spatial_join \
  --limit-pixels 32 \
  --batch-rows 64 \
  --run-name smoke_batch_64 \
  --out-dir "$ROOT/build/results" \
  --no-progress
compare_result_csv \
  "$ROOT/build/results/smoke_batch_1_synthetic_spatial_join_streaming_16x12_ratio100/pixels.csv" \
  "$ROOT/build/results/smoke_batch_64_synthetic_spatial_join_streaming_16x12_ratio100/pixels.csv"
"$BIN" \
  --dataset april_internal_fixture \
  --records "$APRIL_RECORDS" \
  --bbox 0,0,2,2 \
  --resolution 2x2 \
  --method all \
  --verify-april-candidates \
  --ri-resolution 8x8 \
  --compare \
  --run-name smoke \
  --out-dir "$ROOT/build/results" \
  --no-progress
"$BIN" \
  --dataset april_internal_fixture \
  --records "$APRIL_RECORDS" \
  --bbox 0,0,2,2 \
  --resolution 2x2 \
  --method all \
  --verify-april-candidates \
  --ri-resolution 8x8 \
  --execution-mode materialized \
  --batch-rows 1 \
  --compare \
  --run-name smoke \
  --out-dir "$ROOT/build/results" \
  --no-progress
for method in scan spatial_join rtree april_duckdb SLED; do
  timing_method="$method"
  if test "$method" = SLED; then
    timing_method="sled"
  fi
  streaming_dir="$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/$method"
  materialized_dir="$ROOT/build/results/smoke_april_internal_fixture_all_materialized_2x2_ratio100/$method"
  for info in "$streaming_dir/run_info.txt" "$materialized_dir/run_info.txt"; do
    test -f "$info"
    grep -q '^--- timing_breakdown ---$' "$info"
    grep -q '^algorithm_seconds=' "$info"
    grep -q '^timing_algorithm_seconds=' "$info"
    grep -q '^timing_total_seconds=' "$info"
    grep -q "^timing_${timing_method}_algorithm_seconds=" "$info"
    grep -q "^timing_${timing_method}_total_seconds=" "$info"
    grep -q '^timing_method_result_collection_seconds=' "$info"
    grep -q '^timing_method_result_finalization_seconds=' "$info"
    grep -q '^--- memory_usage ---$' "$info"
    grep -q '^memory_measurement_scope=method_setup_algorithm_cleanup$' "$info"
    grep -Eq '^memory_measurement_source=(mach_task_info|linux_proc_statm)$' "$info"
    awk -F= '
      /^memory_rss_at_method_start_bytes=/ { start=$2 }
      /^memory_peak_rss_bytes=/ { peak=$2 }
      /^memory_peak_rss_mib=/ { mib=$2 }
      /^memory_peak_rss_delta_bytes=/ { delta=$2 }
      /^memory_rss_at_method_end_bytes=/ { end=$2 }
      END {
        expected_mib = peak / 1048576.0
        if (start <= 0 || peak < start || peak < end ||
            delta != peak - start ||
            (mib - expected_mib > 1e-9) ||
            (expected_mib - mib > 1e-9)) exit 1
      }
    ' "$info"
    awk -F= -v method="$timing_method" '
      $1 == "runtime_seconds" { runtime=$2 }
      $1 == "algorithm_seconds" { algorithm=$2 }
      $1 == "timing_total_seconds" { timing_total=$2 }
      $1 == "timing_algorithm_seconds" { timing_algorithm=$2 }
      $1 == "timing_" method "_total_seconds" { method_total=$2 }
      $1 == "timing_" method "_algorithm_seconds" { method_algorithm=$2 }
      END {
        if (runtime < algorithm ||
            runtime - timing_total > 1e-12 || timing_total - runtime > 1e-12 ||
            runtime - method_total > 1e-12 || method_total - runtime > 1e-12 ||
            algorithm - timing_algorithm > 1e-12 || timing_algorithm - algorithm > 1e-12 ||
            algorithm - method_algorithm > 1e-12 || method_algorithm - algorithm > 1e-12) exit 1
      }
    ' "$info"
  done
  grep -q '^timing_method_input_materialization_seconds=' \
    "$materialized_dir/run_info.txt"
  compare_result_csv "$streaming_dir/pixels.csv" "$materialized_dir/pixels.csv"
done
grep -q '^sled_normalization_mode=exact_coordinate_edges$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/SLED/run_info.txt"
grep -q '^timing_sled_fragment_accumulation_seconds=' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/SLED/run_info.txt"
grep -q '^timing_sled_total_seconds=' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/SLED/run_info.txt"
grep -q '^sled_edge_storage=flat_sorted_vector$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/SLED/run_info.txt"
grep -q '^memory_strategy=common_input_and_dense_output_with_batched_computation$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/spatial_join/run_info.txt"
grep -q '^batch_rows=16$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/spatial_join/run_info.txt"
grep -q '^timing_spatial_join_pixel_batch_materialization_seconds=' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/spatial_join/run_info.txt"
spatial_join_info="$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/spatial_join/run_info.txt"
grep -q '^E_avg=4$' "$spatial_join_info"
grep -q '^E_max=4$' "$spatial_join_info"
grep -q '^E_total=8$' "$spatial_join_info"
grep -q '^MBR_W_avg=2$' "$spatial_join_info"
grep -q '^MBR_H_avg=2$' "$spatial_join_info"
grep -q '^mbr_grid_candidate_estimate=8$' "$spatial_join_info"
if grep -q '^MBR_W_avg=' \
    "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/scan/run_info.txt"; then
  echo "MBR grid-overlap statistics must be spatial-join-only" >&2
  exit 1
fi
spatial_join_candidates="$(sed -n 's/^spatial_join_candidate_pairs=//p' "$spatial_join_info")"
spatial_join_total="$(sed -n 's/^total_candidate_pairs=//p' "$spatial_join_info")"
test "$spatial_join_candidates" -eq "$spatial_join_total"
grep -q '^execution_mode=streaming$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/scan/run_info.txt"
grep -q '^execution_mode=materialized$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_materialized_2x2_ratio100/scan/run_info.txt"
grep -q '^batch_rows=all$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_materialized_2x2_ratio100/spatial_join/run_info.txt"
grep -q '^scan_query_mode=per_pixel_full_table_scan$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/scan/run_info.txt"
grep -q '^scan_spatial_index=none$' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/scan/run_info.txt"
april_info="$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/april_duckdb/run_info.txt"
grep -q '^april_candidate_source=internal_json_dataset$' "$april_info"
grep -q '^april_refinement_mode=intersection_area_without_precheck$' "$april_info"
grep -q '^april_retained_pairs=' "$april_info"
grep -q '^april_missing_exact_pairs=0$' "$april_info"
grep -q '^timing_april_intersection_area_seconds=' "$april_info"
scan_info="$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/scan/run_info.txt"
cell_count="$(sed -n 's/^cell_count=//p' "$scan_info")"
pixel_count="$(sed -n 's/^pixel_count=//p' "$scan_info")"
candidate_total="$(sed -n 's/^total_candidate_pairs=//p' "$scan_info")"
test "$candidate_total" -eq "$((cell_count * pixel_count))"
awk -F, -v expected="$cell_count" 'NR > 1 && $5 != expected { exit 1 }' \
  "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/scan/pixels.csv"
test "$(grep -c '\"pixel_id\"' "$ROOT/build/results/smoke_april_internal_fixture_all_streaming_2x2_ratio100/SLED/pixels.json")" -eq 4

"$BIN" \
  --dataset april_internal_fixture \
  --records "$APRIL_RECORDS" \
  --bbox 0,0,2,2 \
  --resolution 257x256 \
  --method spatial_join \
  --batch-rows 1000 \
  --run-name smoke_batch_cap \
  --out-dir "$ROOT/build/results" \
  --no-progress
batch_cap_dir="$ROOT/build/results/smoke_batch_cap_april_internal_fixture_spatial_join_streaming_257x256_ratio100"
grep -q '^requested_batch_rows=1000$' "$batch_cap_dir/run_info.txt"
grep -q '^effective_batch_pixels=8192$' "$batch_cap_dir/run_info.txt"
grep -q '^peak_batch_pixels=8192$' "$batch_cap_dir/run_info.txt"
grep -q '^batch_count=9$' "$batch_cap_dir/run_info.txt"
grep -q '^spatial_join_batch_cap_pixels=8192$' "$batch_cap_dir/run_info.txt"
grep -q '^spatial_join_refinement_chunk_pairs=256$' "$batch_cap_dir/run_info.txt"

"$BIN" \
  --dataset april_internal_fixture \
  --records "$APRIL_RECORDS" \
  --bbox 0,0,2,2 \
  --resolution 257x256 \
  --method spatial_join \
  --execution-mode materialized \
  --run-name smoke_batch_cap_materialized \
  --out-dir "$ROOT/build/results" \
  --no-progress
compare_result_csv "$batch_cap_dir/pixels.csv" \
  "$ROOT/build/results/smoke_batch_cap_materialized_april_internal_fixture_spatial_join_materialized_257x256_ratio100/pixels.csv"

if "$BIN" \
    --dataset april_internal_fixture \
    --records "$APRIL_RECORDS" \
    --bbox 0,0,2,2 \
    --resolution 513x512 \
    --method SLED \
    --execution-mode materialized \
    --no-progress >${TEMP_DIR}/pgv-materialized-limit.out 2>${TEMP_DIR}/pgv-materialized-limit.err; then
  echo "expected unsafe materialized run to fail before allocation" >&2
  exit 1
fi
grep -q 'exceeding the safety limit of 262144' ${TEMP_DIR}/pgv-materialized-limit.err

"$BIN" \
  --dataset april_internal_fixture \
  --records "$APRIL_RECORDS" \
  --bbox 0,0,2,2 \
  --resolution 513x512 \
  --method SLED \
  --execution-mode streaming \
  --run-name smoke_large_streaming \
  --out-dir "$ROOT/build/results" \
  --no-progress
test -f "$ROOT/build/results/smoke_large_streaming_april_internal_fixture_SLED_streaming_513x512_ratio100/pixels.csv"
awk -F= '
  /^runtime_seconds=/ { runtime=$2 }
  /^algorithm_seconds=/ { algorithm=$2 }
  END { if (runtime <= algorithm) exit 1 }
' "$ROOT/build/results/smoke_large_streaming_april_internal_fixture_SLED_streaming_513x512_ratio100/run_info.txt"

if find "$ROOT/build/results" -type d -name '.duckdb_tmp' -print -quit | grep -q .; then
  echo "DuckDB temporary spill directory was not cleaned" >&2
  exit 1
fi

CONFIG_FIXTURES="$ROOT/tests/fixtures"
"$BIN" --config "$CONFIG_FIXTURES/multi_run_config.json" \
  >${TEMP_DIR}/pgv-config-success.out 2>${TEMP_DIR}/pgv-config-success.err
grep -q '^\[1/3\] config_spatial$' ${TEMP_DIR}/pgv-config-success.out
grep -q 'algorithm_seconds=' ${TEMP_DIR}/pgv-config-success.out
grep -q '^3,config_all,success,' ${TEMP_DIR}/pgv-config-success.out
test -f "$ROOT/build/config-results/config_all_april_internal_fixture_all_streaming_2x2_ratio100/comparison.csv"
grep -q '^execution_mode=materialized$' \
  "$ROOT/build/config-results/config_scan_sled_april_internal_fixture_scan_SLED_materialized_2x2_ratio100/scan/run_info.txt"

"$BIN" \
  --dataset april_internal_fixture \
  --records "$APRIL_RECORDS" \
  --bbox 0,0,2,2 \
  --resolution 2x2 \
  --method spatial_join \
  --threads 1 \
  --batch-rows 1 \
  --run-name direct_spatial \
  --out-dir "$ROOT/build/config-results" \
  --no-progress
compare_result_csv \
  "$ROOT/build/config-results/config_spatial_april_internal_fixture_spatial_join_streaming_2x2_ratio100/pixels.csv" \
  "$ROOT/build/config-results/direct_spatial_april_internal_fixture_spatial_join_streaming_2x2_ratio100/pixels.csv"

"$BIN" --config "$CONFIG_FIXTURES/dataset_ratio_config.json" \
  >${TEMP_DIR}/pgv-config-ratio.out 2>${TEMP_DIR}/pgv-config-ratio.err
"$BIN" \
  --dataset dataset_ratio_fixture \
  --records "$RATIO_RECORDS" \
  --resolution 2x2 \
  --method spatial_join \
  --dataset-ratio 40 \
  --threads 1 \
  --batch-rows 1 \
  --run-name direct_ratio \
  --out-dir "$ROOT/build/config-results" \
  --no-progress
compare_result_csv \
  "$ROOT/build/config-results/config_ratio_dataset_ratio_fixture_spatial_join_streaming_2x2_ratio40/pixels.csv" \
  "$ROOT/build/config-results/direct_ratio_dataset_ratio_fixture_spatial_join_streaming_2x2_ratio40/pixels.csv"
grep -q '^dataset_ratio_percent=40$' \
  "$ROOT/build/config-results/config_ratio_dataset_ratio_fixture_spatial_join_streaming_2x2_ratio40/run_info.txt"

if "$BIN" --config "$CONFIG_FIXTURES/runtime_failure_config.json" \
    >${TEMP_DIR}/pgv-config-failure.out 2>${TEMP_DIR}/pgv-config-failure.err; then
  echo "expected a configuration containing a failed run to exit nonzero" >&2
  exit 1
fi
grep -q '^1,config_expected_failure,failed,' ${TEMP_DIR}/pgv-config-failure.out
grep -q '^2,config_after_failure,success,' ${TEMP_DIR}/pgv-config-failure.out
test -f "$ROOT/build/config-failure-results/config_after_failure_april_internal_fixture_spatial_join_streaming_2x2_ratio100/pixels.csv"

for invalid_config in \
  invalid_empty_config.json \
  invalid_schema_config.json \
  invalid_unknown_config.json \
  invalid_type_config.json \
  invalid_ratio_type_config.json \
  invalid_duplicate_name_config.json; do
  if "$BIN" --config "$CONFIG_FIXTURES/$invalid_config" \
      >${TEMP_DIR}/pgv-config-invalid.out 2>${TEMP_DIR}/pgv-config-invalid.err; then
    echo "expected $invalid_config to fail validation" >&2
    exit 1
  fi
done
if "$BIN" --config "$CONFIG_FIXTURES/multi_run_config.json" --threads 2 \
    >${TEMP_DIR}/pgv-config-mixed.out 2>${TEMP_DIR}/pgv-config-mixed.err; then
  echo "expected --config mixed with normal options to fail" >&2
  exit 1
fi
