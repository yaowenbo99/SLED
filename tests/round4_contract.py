#!/usr/bin/env python3
"""Contract and numerical regressions for the SIGMOD round-4 benchmark.

No external Python packages. Every fixture compares complete grids; timings are
checked structurally rather than asserting a speedup or a machine-specific bound.
"""
import csv
import json
import math
from pathlib import Path
import subprocess
import struct
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BIN = Path(sys.argv[1] if len(sys.argv) > 1 else ROOT / "build/sled").resolve()


def info(path):
    return dict(line.split("=", 1) for line in path.read_text().splitlines() if "=" in line)


def pixels(path):
    with path.open() as stream:
        return list(csv.DictReader(stream))


def equivalent(left, right, candidates=False):
    assert len(left) == len(right)
    for a, b in zip(left, right):
        assert (a["u"], a["v"], a["pixel_id"]) == (b["u"], b["v"], b["pixel_id"])
        assert math.isclose(float(a["density"]), float(b["density"]), rel_tol=1e-12, abs_tol=1e-9), (a, b)
        if candidates:
            assert a["candidate_count"] == b["candidate_count"]


with tempfile.TemporaryDirectory(prefix="pgv-round4-contract-") as work:
    work = Path(work)
    fixture = ROOT / "tests/fixtures/sled_interactive_geometry_records.jsonl"
    records = [json.loads(line) for line in fixture.read_text().splitlines()]
    # Concave polygon adds nonconvex and grid-vertex intersections to the fixture's
    # holes, multipolygons, zero/negative weights and grid-aligned vertical edges.
    records.append({"cell_id": "concave", "geometry": {"type": "Polygon", "coordinates": [[[0,0],[4,0],[4,1],[1,1],[1,4],[0,4],[0,0]]]}, "weight": {"value": 3}})
    fixture = work / "geometry.jsonl"
    fixture.write_text("".join(json.dumps(row) + "\n" for row in records))

    def run(name, *args, source=fixture, validation=True):
        output = work / name
        command = [str(BIN), "--records", str(source), "--dataset", "contract", "--resolution", "8x8", "--ri-resolution", "8x8", "--no-progress", "--out-dir", str(output), "--run-name", name, *args]
        if validation:
            command.append("--validation-grid")
        result = subprocess.run(command, text=True, capture_output=True)
        assert result.returncode == 0, result.stdout + result.stderr
        paths = sorted(output.rglob("run_info.txt"))
        assert paths
        result_infos = [info(path) for path in paths]
        for meta in result_infos:
            assert meta["timing_contract"] == "v2_dense_memory"
            assert meta["common_input_state"] == "CellRecord_geometry_JSON_in_memory"
            assert meta["result_state"] == "complete_dense_grid_in_memory"
            compute, serialization, runtime = map(float, (meta["computation_seconds"], meta["serialization_seconds"], meta["runtime_seconds"]))
            assert 0 < compute <= runtime
            assert 0 <= serialization <= runtime
            assert compute + serialization <= runtime + 1e-9
            assert float(meta["initialization_seconds"]) > 0
            binary_values = None
            if validation:
                path = Path(meta["validation_grid_path"])
                raw = path.read_bytes()
                assert meta["validation_grid_format"] == "ieee754_float64_le"
                assert meta["validation_grid_order"] == "row_major_top_down_pixel_id"
                shape = json.loads(meta["validation_grid_shape"])
                assert len(raw) == math.prod(shape) * 8 == int(meta["validation_grid_bytes"])
                assert math.prod(shape) == int(meta["pixel_count"]) == int(meta["validation_grid_pixel_count"])
                assert 0 < float(meta["validation_grid_export_seconds"]) <= serialization
                binary_values = [value[0] for value in struct.iter_unpack("<d", raw)]
                assert all(math.isfinite(value) for value in binary_values)
            if meta["output_discarded"] == "true":
                assert not meta["output_csv"]
                assert serialization > 0 if validation else serialization == 0
                assert not (Path(meta["task_dir"]) / "pixels.csv").exists()
            else:
                rows = pixels(Path(meta["output_csv"]))
                assert len(rows) == int(meta["pixel_count"])
                if binary_values is not None:
                    assert binary_values == [float(row["density"]) for row in rows]
        return output, result_infos

    _, static = run("static", "--method", "all", "--compare", "--bbox", "0,0,4,4")
    assert len({item["initialization_seconds"] for item in static}) == 1
    # Viewport entirely inside a polygon: both boundary edges lie outside it.
    enclosing = work / "enclosing.jsonl"
    enclosing.write_text(json.dumps({"cell_id":"enclosing", "geometry":{"type":"Polygon", "coordinates":[[[-10,-10],[10,-10],[10,10],[-10,10],[-10,-10]]]}, "weight":{"value":2}}) + "\n")
    _, enclosed = run("enclosed", "--method", "all", "--compare", "--bbox", "-1,-1,1,1", source=enclosing)
    for meta in enclosed:
        assert all(math.isclose(float(row["density"]), .125, abs_tol=1e-12) for row in pixels(Path(meta["output_csv"])))

    selected = "zoom_0p25,zoom_0p5,zoom_0p75,zoom_1,pan_01,pan_02,pan_03,pan_04,pan_05,delete_10,delete_20,delete_30,delete_40"
    delta_root, delta = run("delta", "--method", "all", "--compare", "--interactive-operation", "all", "--interactive-trials", selected)
    assert len(delta) == 65
    full_root, full = run("full", "--method", "SLED", "--interactive-operation", "updating", "--sled-update-mode", "full")
    delta_parent = next(info(path) for path in delta_root.rglob("interactive_run_info.txt") if info(path)["method"] == "SLED")
    full_parent = info(next(full_root.rglob("interactive_run_info.txt")))
    assert float(delta_parent["interactive_initial_density_seconds"]) > 0
    assert float(full_parent["interactive_initial_density_seconds"]) == 0
    assert float(delta_parent["interactive_preprocessing_seconds"]) > 0
    assert len(full) == 4
    for f in full:
        d = next(meta for meta in delta if meta["method"] == "SLED" and meta["trial_id"] == f["trial_id"])
        assert d["sled_update_mode"] == "delta" and f["sled_update_mode"] == "full"
        equivalent(pixels(Path(d["output_csv"])), pixels(Path(f["output_csv"])), candidates=True)
        removed = set(json.loads(d["deleted_polygon_indices"]))
        remaining = work / "remaining.jsonl"
        remaining.write_text("".join(json.dumps(row) + "\n" for i, row in enumerate(records) if i not in removed))
        bbox = json.loads(d["bbox"])
        _, rebuilt = run("rebuilt_" + f["trial_id"], "--method", "scan,SLED", "--compare", "--bbox", ",".join(str(bbox[k]) for k in ("x_min", "y_min", "x_max", "y_max")), source=remaining)
        for meta in rebuilt:
            equivalent(pixels(Path(d["output_csv"])), pixels(Path(meta["output_csv"])))

    out, progressive = run("progressive", "--method", "SLED", "--progressive-checkpoints", "25,50,75,100", "--progressive-snapshots")
    meta = progressive[0]
    assert meta["progressive_snapshot_source"] == "live_row_consumer_copies_before_later_rows_are_computed"
    final = pixels(Path(meta["output_csv"]))
    with Path(meta["progressive_metrics_csv"]).open() as stream:
        checkpoints = list(csv.DictReader(stream))
    times = [float(item["compute_seconds"]) for item in checkpoints]
    assert times == sorted(times)
    assert math.isclose(times[-1], float(meta["computation_seconds"]), abs_tol=1e-12)
    for label in ("first_row", "25_percent", "50_percent", "75_percent", "100_percent"):
        snapshot = pixels(Path(meta[f"progressive_snapshot_{label}_csv"]))
        assert len(snapshot) == int(meta[f"progressive_completed_rows_{label}"]) * 8
        equivalent(snapshot, final[:len(snapshot)], candidates=True)

    run("discard", "--method", "all", "--discard-output", validation=False)
    _, discarded = run("discard_grid", "--method", "all", "--discard-output")
    _, discarded_interactive = run("discard_interactive", "--method", "all", "--interactive-operation", "all", "--interactive-trials", selected, "--discard-output")
    for group in (discarded, *[[m for m in discarded_interactive if m["trial_id"] == trial] for trial in selected.split(",")]):
        reference = next(m for m in group if m["method"] == "scan")
        values = [v[0] for v in struct.iter_unpack("<d", Path(reference["validation_grid_path"]).read_bytes())]
        for meta in group:
            observed = [v[0] for v in struct.iter_unpack("<d", Path(meta["validation_grid_path"]).read_bytes())]
            assert len(values) == len(observed)
            assert all(abs(a-b) <= 1e-9 + 1e-12 * abs(a) for a,b in zip(values, observed))
    # Dense output is allowed past the separate materialized-input safety cap.
    _, large = run("large_streaming", "--method", "SLED", "--discard-output", "--resolution", "513x512")
    assert int(large[0]["pixel_count"]) > 262144

print("round4 timing, f64le validation grids, live snapshots, viewport, full/delta and geometry regressions passed")
