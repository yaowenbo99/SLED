#!/usr/bin/env python3
"""Reproducible, sequential SLED and baseline measurement campaign.

Examples (initialize only after the measurement binary has been validated)::
  python3 scripts/campaign.py init --preset full --data-dir /path/to/processed --output results/full
  python3 scripts/campaign.py run --manifest results/full/manifest.json --dry-run
  python3 scripts/campaign.py run --manifest results/full/manifest.json --resume
  python3 scripts/campaign.py aggregate --manifest results/full/manifest.json

No legacy timing field is accepted as a substitute for computation_seconds.
Each process has a separate directory, and interrupted attempts are preserved.
"""
from __future__ import annotations

import argparse
import contextlib
import csv
import datetime as dt
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys
import time

PGV = Path(__file__).resolve().parents[1]
ROOT = PGV
CONTRACT = "v2_dense_memory"
SCHEMA = 1
DATASETS = {
    "uk_pcon_2024_boundaries": "Election",
    "nyc_planimetric_hydrography": "Hydrography",
    "nyc_parks_permit_areas": "Parks",
    "sla_cadastral_land_parcel": "Parcels",
}
SUPPORTED_METHODS = ("scan", "spatial_join", "rtree", "april_duckdb", "SLED")
METHODS = ("spatial_join", "rtree", "april_duckdb", "SLED")
FAST_BASELINE = dict(zip(DATASETS, ("april_duckdb", "april_duckdb", "april_duckdb", "spatial_join")))
RESOLUTIONS = ("1280x960", "1600x1200", "1920x1440", "2240x1680")
TRIALS = {
    "zoom": [("zoom_0p25", 0.25), ("zoom_0p5", 0.5), ("zoom_0p75", 0.75), ("zoom_1", 1.0)],
    "panning": [(f"pan_{i:02d}", i) for i in range(1, 6)],
    "updating": [(f"delete_{i}", i) for i in (10, 20, 30, 40)],
}
DEADLINE_TRIALS = {"zoom": "zoom_0p5", "panning": "pan_01", "updating": "delete_10"}


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False)


def digest(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def file_hash(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def source_files():
    paths = [PGV / "CMakeLists.txt", PGV / "main.cpp"]
    paths += sorted((PGV / "src").glob("*.cpp"))
    paths += sorted((PGV / "src").glob("*.h"))
    return {str(p.relative_to(ROOT)): file_hash(p) for p in paths}


def atomic_json(path, value):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True, allow_nan=False) + "\n")
    temporary.replace(path)


def read_json(path):
    return json.loads(Path(path).read_text())


def read_info(path):
    return dict(line.split("=", 1) for line in Path(path).read_text().splitlines() if "=" in line)


def positive(value, key):
    n = float(value)
    if not math.isfinite(n) or n <= 0:
        raise ValueError(f"{key} must be finite and positive: {value}")
    return n


def nonnegative(value, key):
    n = float(value)
    if not math.isfinite(n) or n < 0:
        raise ValueError(f"{key} must be finite and nonnegative: {value}")
    return n


def case(dataset, method, resolution, ratio, operation="static", trial="static", x=None, variant="standard"):
    spec = dict(dataset=dataset, method=method, resolution=resolution, dataset_ratio_percent=ratio,
                operation=operation, trial_id=trial, variant=variant, x=x)
    spec["case_id"] = digest(spec)[:20]
    return spec


def make_jobs(preset="full", repetitions=3, datasets=None, methods=None):
    datasets = list(datasets or DATASETS)
    default = "4x4" if preset == "smoke" else "1600x1200"
    jobs = []
    for repetition in range(1, repetitions + 1):
        for dataset in datasets:
            selected_methods = list(methods or ((FAST_BASELINE.get(dataset, "spatial_join"), "SLED") if preset == "deadline" else METHODS))
            # Rotate method order deterministically across independent repetitions.
            offset = (repetition - 1) % len(selected_methods)
            selected_methods = selected_methods[offset:] + selected_methods[:offset]
            static = [(default, 100)]
            if preset == "full":
                static += [(r, 100) for r in RESOLUTIONS if r != default]
                static += [(default, r) for r in (25, 50, 75)]
            for resolution, ratio in static:
                families = ["static"]
                if ratio == 100:
                    families.append("resolution")
                if resolution == default:
                    families.append("dataset")
                if resolution == default and ratio == 100:
                    families.append("default")
                for method in selected_methods:
                    obs = case(dataset, method, resolution, ratio)
                    obs["families"] = families
                    jobs.append({"repetition": repetition, "cases": [obs], "operation": "static"})
            if preset != "smoke":
                for operation, trials in TRIALS.items():
                    chosen = trials if preset == "full" else [(t, x) for t, x in trials if t == DEADLINE_TRIALS[operation]]
                    for method in selected_methods:
                        variant = "delta" if method == "SLED" and operation == "updating" else "standard"
                        observations = [case(dataset, method, default, 100, operation, t, x, variant) for t, x in chosen]
                        for obs in observations:
                            obs["families"] = ["interaction", operation]
                        jobs.append({"repetition": repetition, "operation": operation, "cases": observations})
                if "SLED" in selected_methods:
                    chosen = TRIALS["updating"] if preset == "full" else [("delete_10", 10)]
                    observations = [case(dataset, "SLED", default, 100, "updating", t, x, "full") for t, x in chosen]
                    for obs in observations:
                        obs["families"] = ["full-update"]
                    jobs.append({"repetition": repetition, "operation": "updating", "cases": observations})
    for job in jobs:
        job["job_id"] = "r%02d_%s" % (job["repetition"], digest(job)[:16])
    return jobs


def initialize(args):
    output = args.output.resolve()
    manifest_path = output / "manifest.json"
    if manifest_path.exists():
        raise ValueError(f"Manifest exists; use run --resume: {manifest_path}")
    datasets = args.datasets.split(",") if args.datasets else list(DATASETS)
    if args.preset == "smoke":
        datasets = ["dataset_ratio"]
    unknown = set(datasets) - set(DATASETS) - {"dataset_ratio"}
    if unknown:
        raise ValueError(f"Unknown datasets: {sorted(unknown)}")
    methods = args.methods.split(",") if args.methods else None
    if methods and (set(methods) - set(SUPPORTED_METHODS) or len(set(methods)) != len(methods)):
        raise ValueError("Methods must be distinct names from " + ",".join(SUPPORTED_METHODS))
    if args.repetitions != 3:
        raise ValueError("The registered round-four protocol requires exactly three repetitions")
    executable = args.executable.resolve()
    validation_path = args.validation_report.resolve()
    validation = read_json(validation_path)
    executable_sha256 = file_hash(executable)
    if validation.get("passed") is not True or validation.get("binary_sha256") != executable_sha256:
        raise ValueError("Validation report must confirm this exact executable; validate it before initializing a campaign")
    if args.preset != "smoke" and args.data_dir is None:
        raise ValueError("--data-dir is required for real datasets")
    inputs = {}
    for dataset in datasets:
        records = PGV / "tests/fixtures/dataset_ratio_records.jsonl" if dataset == "dataset_ratio" else args.data_dir.resolve() / f"{dataset}_records.jsonl"
        inputs[dataset] = {"path": str(records), "sha256": file_hash(records), "bytes": records.stat().st_size}
    sources = source_files()
    manifest = dict(schema_version=SCHEMA, created_at=utc_now(), preset=args.preset,
                    timing_contract=CONTRACT, repetitions=args.repetitions, output_directory=str(output),
                    executable=str(executable), executable_sha256=executable_sha256, source_files=sources,
                    source_sha256=digest(sources), dataset_inputs=inputs,
                    pipeline_source_sha256=file_hash(__file__),
                    validation_evidence={"path": str(validation_path), "sha256": file_hash(validation_path), "report": validation},
                    protocol={"threads": 1, "batch_rows": 16, "ri_resolution": "512x512",
                              "interaction_seed": 42,
                              "output_policy": "discard_after_dense_materialization", "subset_policy": "file_prefix",
                              "validation_grid": "ieee754_float64_le_complete_row_major_grid",
                              "validation_absolute_tolerance": 1e-9, "validation_relative_tolerance": 1e-12,
                              "static_scope": "input_ready_to_complete_dense_grid_including_method_preparation",
                              "interactive_scope": "prepared_state_to_complete_dense_grid_including_update_merge",
                              "summary": "median of three independent process repetitions; min/max range",
                              "series_order": "deterministic cyclic rotation by repetition"},
                    environment={"platform": platform.platform(), "machine": platform.machine(), "python": sys.version},
                    jobs=make_jobs(args.preset, args.repetitions, datasets, methods))
    manifest["manifest_sha256"] = digest(manifest)
    atomic_json(manifest_path, manifest)
    print(canonical({"manifest": str(manifest_path), "processes": len(manifest["jobs"]),
                     "observations": sum(len(j["cases"]) for j in manifest["jobs"]), "preset": args.preset}))


def load_manifest(path):
    manifest = read_json(path)
    check = dict(manifest)
    claimed = check.pop("manifest_sha256")
    if digest(check) != claimed or manifest["schema_version"] != SCHEMA or manifest["timing_contract"] != CONTRACT:
        raise ValueError("Manifest is modified or uses an unsupported timing contract")
    return manifest


def command_for(manifest, job, attempt):
    c = job["cases"][0]
    cmd = [manifest["executable"], "--records", manifest["dataset_inputs"][c["dataset"]]["path"],
           "--dataset", c["dataset"], "--resolution", c["resolution"], "--method", c["method"],
           "--dataset-ratio", str(c["dataset_ratio_percent"]), "--threads", "1", "--batch-rows", "16",
           "--ri-resolution", "512x512", "--execution-mode", "streaming", "--no-progress", "--discard-output", "--validation-grid",
           "--out-dir", str(attempt), "--run-name", job["job_id"]]
    if job["operation"] != "static":
        cmd += ["--interactive-operation", job["operation"]]
        if manifest["preset"] == "deadline":
            cmd += ["--interactive-trials", ",".join(c["trial_id"] for c in job["cases"])]
        if c["method"] == "SLED" and job["operation"] == "updating":
            cmd += ["--sled-update-mode", c["variant"]]
    return cmd


def selected_jobs(manifest, args):
    families = set(args.family.split(",")) if args.family else set()
    datasets = set(args.datasets.split(",")) if args.datasets else set()
    methods = set(args.methods.split(",")) if args.methods else set()
    for job in manifest["jobs"]:
        c = job["cases"][0]
        if families and not any(families.intersection(o["families"]) for o in job["cases"]):
            continue
        if datasets and c["dataset"] not in datasets:
            continue
        if methods and c["method"] not in methods:
            continue
        yield job


def validate_binary_and_inputs(manifest):
    if file_hash(manifest["executable"]) != manifest["executable_sha256"]:
        raise ValueError("Executable changed; initialize a new campaign instead of mixing binaries")
    if source_files() != manifest["source_files"]:
        raise ValueError("C++ sources changed; initialize a new campaign")
    for entry in manifest["dataset_inputs"].values():
        if file_hash(entry["path"]) != entry["sha256"]:
            raise ValueError(f"Dataset changed: {entry['path']}")


def collect_attempt(manifest, job, attempt):
    infos = []
    for path in sorted(Path(attempt).rglob("run_info.txt")):
        info = read_info(path)
        if "computation_seconds" in info or "runtime_seconds" in info:
            infos.append((path, info))
    observations = []
    for spec in job["cases"]:
        candidates = [(p, d) for p, d in infos if d.get("method") == spec["method"] and
                      d.get("interactive_trial_id", "static") == spec["trial_id"]]
        if len(candidates) != 1:
            raise ValueError(f"{job['job_id']}/{spec['trial_id']}: expected one run_info, found {len(candidates)}")
        path, d = candidates[0]
        if d.get("timing_contract") != CONTRACT:
            raise ValueError(f"Legacy/missing timing contract in {path}; legacy timings cannot be imported")
        if d.get("common_input_state") != "CellRecord_geometry_JSON_in_memory" or d.get("result_state") != "complete_dense_grid_in_memory":
            raise ValueError(f"Input/output state violates the registered timing scope in {path}")
        if d.get("output_discarded") != "true":
            raise ValueError(f"Campaign output policy changed in {path}")
        for key in ("dataset", "resolution"):
            if d.get(key) != str(spec[key]):
                raise ValueError(f"Wrong {key} in {path}: {d.get(key)} != {spec[key]}")
        if float(d.get("dataset_ratio_percent", "100")) != spec["dataset_ratio_percent"]:
            raise ValueError(f"Wrong dataset ratio in {path}")
        if spec["operation"] != "static" and d.get("interactive_operation") != spec["operation"]:
            raise ValueError(f"Wrong interactive operation in {path}")
        if spec["variant"] in ("full", "delta") and d.get("sled_update_mode") != spec["variant"]:
            raise ValueError(f"Wrong SLED update mode in {path}")
        fingerprint = d.get("dataset_fingerprint")
        if not fingerprint:
            raise ValueError(f"Missing dataset fingerprint in {path}")
        computation = positive(d["computation_seconds"], "computation_seconds")
        serialization = nonnegative(d["serialization_seconds"], "serialization_seconds")
        memory = nonnegative(d["memory_peak_rss_bytes"], "memory_peak_rss_bytes") / (1024 ** 2)
        pixel_count = int(d["pixel_count"])
        width, height = map(int, spec["resolution"].split("x"))
        if pixel_count != width * height:
            raise ValueError(f"Incomplete grid in {path}")
        grid_path = Path(d["validation_grid_path"]).resolve()
        if not grid_path.is_relative_to(Path(attempt).resolve()):
            raise ValueError(f"Validation grid points outside its own run: {grid_path}")
        if (d.get("validation_grid_format") != "ieee754_float64_le" or
                d.get("validation_grid_order") != "row_major_top_down_pixel_id" or
                json.loads(d["validation_grid_shape"]) != [height, width] or
                int(d["validation_grid_pixel_count"]) != pixel_count or
                int(d["validation_grid_bytes"]) != pixel_count * 8 or grid_path.stat().st_size != pixel_count * 8):
            raise ValueError(f"Invalid/incomplete compact validation grid: {grid_path}")
        parent_info = {}
        for parent in path.parents:
            if parent == Path(attempt).parent:
                break
            parent_path = parent / "interactive_run_info.txt"
            if parent_path.exists():
                parent_info = read_info(parent_path)
                break
        phases = {k.removeprefix("timing_"): nonnegative(v, k) for k, v in d.items()
                  if k.startswith("timing_") and k.endswith("_seconds")}
        observation = dict(spec, repetition=job["repetition"], job_id=job["job_id"], timing_contract=CONTRACT,
                           computation_seconds=computation, serialization_seconds=serialization,
                           memory_peak_rss_mib=memory, pixel_count=pixel_count, cell_count=int(d["cell_count"]),
                           input_edges=int(d["sled_input_edges"]) if "sled_input_edges" in d else None,
                           output_discarded=True,
                           dataset_fingerprint=fingerprint, bbox=json.loads(d["bbox"]), phases=phases,
                           deleted_polygon_indices=json.loads(d.get("deleted_polygon_indices", "[]")),
                           validation_grid={"path": str(grid_path), "sha256": file_hash(grid_path), "pixel_count": pixel_count,
                                            "shape": [height, width], "format": "ieee754_float64_le", "bytes": grid_path.stat().st_size},
                           source_path=str(path.resolve()), source_sha256=file_hash(path),
                           initialization={k: nonnegative(v, k) for k, v in {**parent_info, **d}.items()
                                           if k in ("initialization_seconds", "interactive_preprocessing_seconds", "interactive_initial_density_seconds")},
                           manifest_sha256=manifest["manifest_sha256"], executable_sha256=manifest["executable_sha256"])
        observations.append(observation)
    return observations


@contextlib.contextmanager
def campaign_lock(lock_dir):
    lock_dir = Path(lock_dir)
    lock_dir.mkdir(parents=True, exist_ok=True)
    with (lock_dir / ".campaign.lock").open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise ValueError("Another round-four campaign is already running")
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def run(args):
    manifest = load_manifest(args.manifest)
    root = Path(manifest["output_directory"])
    jobs = list(selected_jobs(manifest, args))
    if not jobs:
        raise ValueError("Filters select no jobs")
    if args.dry_run:
        for job in jobs:
            print(canonical({"job_id": job["job_id"], "cases": len(job["cases"]),
                             "command": command_for(manifest, job, root / "runs" / job["job_id"] / "attempt-001")}))
        return
    validate_binary_and_inputs(manifest)
    completed = 0
    with campaign_lock(root):
        for job in jobs:
            job_dir = root / "runs" / job["job_id"]
            receipt = job_dir / "completed.json"
            if receipt.exists():
                if not args.resume:
                    raise ValueError(f"Existing result; pass --resume: {receipt}")
                # Recheck the underlying files before trusting a cached result.
                old = read_json(receipt)
                if old["manifest_sha256"] != manifest["manifest_sha256"]:
                    raise ValueError(f"Foreign result receipt: {receipt}")
                collect_attempt(manifest, job, old["attempt_directory"])
                continue
            if args.max_jobs is not None and completed >= args.max_jobs:
                break
            job_dir.mkdir(parents=True, exist_ok=True)
            attempt_no = 1
            while (job_dir / f"attempt-{attempt_no:03d}").exists():
                attempt_no += 1
            attempt = job_dir / f"attempt-{attempt_no:03d}"
            attempt.mkdir()
            command = command_for(manifest, job, attempt)
            started = utc_now()
            atomic_json(attempt / "invocation.json", dict(command=command, started_at=started, job=job,
                                                          manifest_sha256=manifest["manifest_sha256"]))
            with (attempt / "process.log").open("w") as log:
                process = subprocess.Popen(command, cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
                try:
                    while process.poll() is None:
                        atomic_json(root / "progress.json", dict(state="running", complete=False, updated_at=utc_now(),
                                    current_job=job["job_id"], child_pid=process.pid, started_at=started,
                                    jobs_finished_this_invocation=completed,
                                    completed_jobs=sum((root / "runs" / j["job_id"] / "completed.json").exists() for j in manifest["jobs"]),
                                    expected_jobs=len(manifest["jobs"])))
                        time.sleep(2)
                except BaseException:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
                    atomic_json(root / "progress.json", dict(state="interrupted", complete=False, current_job=job["job_id"], updated_at=utc_now()))
                    raise
            if process.returncode:
                atomic_json(root / "progress.json", dict(state="failed", complete=False, current_job=job["job_id"],
                            exit_code=process.returncode, log=str(attempt / "process.log"), updated_at=utc_now()))
                raise ValueError(f"Measurement failed: {attempt / 'process.log'}")
            try:
                observations = collect_attempt(manifest, job, attempt)
            except (ValueError, KeyError, OSError) as exc:
                atomic_json(root / "progress.json", dict(state="invalid_result", complete=False, current_job=job["job_id"],
                            error=str(exc), log=str(attempt / "process.log"), updated_at=utc_now()))
                raise
            atomic_json(receipt, dict(manifest_sha256=manifest["manifest_sha256"], completed_at=utc_now(),
                                     attempt_directory=str(attempt), observations=observations))
            completed += 1
            print(canonical({"finished": job["job_id"], "observations": len(observations)}), flush=True)
        aggregate_manifest(manifest)


def summarize(manifest, observations):
    expected = {}
    for job in manifest["jobs"]:
        for c in job["cases"]:
            expected.setdefault(c["case_id"], c)
    grouped = {}
    for row in observations:
        if row["timing_contract"] != CONTRACT or row["manifest_sha256"] != manifest["manifest_sha256"]:
            raise ValueError("Cannot mix timing contracts or campaign manifests")
        if row["case_id"] not in expected:
            raise ValueError("Result condition absent from manifest")
        group = grouped.setdefault(row["case_id"], [])
        if any(r["repetition"] == row["repetition"] for r in group):
            raise ValueError("Duplicate repetition")
        group.append(row)
    summaries = []
    missing = []
    for case_id, spec in expected.items():
        rows = grouped.get(case_id, [])
        reps = {r["repetition"] for r in rows}
        if reps != set(range(1, manifest["repetitions"] + 1)):
            missing.append(dict(spec, present_repetitions=sorted(reps)))
            continue
        fingerprints = {r["dataset_fingerprint"] for r in rows}
        bboxes = {canonical(r["bbox"]) for r in rows}
        if len(fingerprints) != 1 or len(bboxes) != 1:
            raise ValueError(f"Inputs differ across repetitions for {case_id}")
        representative = sorted(rows, key=lambda r: r["computation_seconds"])[len(rows) // 2]
        summary = dict(spec, n=len(rows), timing_contract=CONTRACT, dataset_fingerprint=representative["dataset_fingerprint"],
                       bbox=representative["bbox"], source_paths=[r["source_path"] for r in sorted(rows, key=lambda r: r["repetition"])],
                       representative_repetition=representative["repetition"], representative_phases=representative["phases"],
                       representative_initialization=representative["initialization"])
        for metric in ("computation_seconds", "serialization_seconds", "memory_peak_rss_mib"):
            numbers = [r[metric] for r in rows]
            summary[metric] = dict(median=statistics.median(numbers), minimum=min(numbers), maximum=max(numbers), values=numbers)
        summaries.append(summary)
    return summaries, missing


def matched_speedups(summaries, family):
    pairs = {}
    for row in summaries:
        if family not in row["families"] or row["variant"] == "full":
            continue
        key = (row["dataset"], row["resolution"], row["dataset_ratio_percent"], row["operation"], row["trial_id"])
        pairs.setdefault(key, {})[row["method"]] = row
    output = []
    for key, methods in pairs.items():
        if "SLED" not in methods:
            continue
        sled = methods["SLED"]
        for name, baseline in methods.items():
            if name == "SLED":
                continue
            if baseline["bbox"] != sled["bbox"] or baseline["dataset_fingerprint"] != sled["dataset_fingerprint"]:
                raise ValueError("Cannot compare speedups across different dataset fingerprints or viewports")
            output.append(dict(dataset=key[0], resolution=key[1], dataset_ratio_percent=key[2], operation=key[3], trial_id=key[4],
                               baseline=name, baseline_case_id=baseline["case_id"], sled_case_id=sled["case_id"],
                               speedup=baseline["computation_seconds"]["median"] / sled["computation_seconds"]["median"]))
    return output


def compare_grids(reference, candidate, absolute_tolerance=1e-9, relative_tolerance=1e-12):
    """Compare all cells in bounded-memory chunks, always outside measured time."""
    try:
        import numpy as np
    except ImportError as exc:
        raise ValueError("Full-grid validation requires NumPy; install the NumPy dependency from requirements.txt") from exc
    if reference["shape"] != candidate["shape"] or reference["pixel_count"] != candidate["pixel_count"]:
        raise ValueError("Cannot compare differently shaped grids")
    maximum_absolute = 0.0
    maximum_relative = 0.0
    maximum_zero_reference = 0.0
    maximum_pixel = 0
    failing = nonfinite = examined = 0
    with Path(reference["path"]).open("rb") as left, Path(candidate["path"]).open("rb") as right:
        while True:
            a = np.fromfile(left, dtype="<f8", count=131072)
            b = np.fromfile(right, dtype="<f8", count=131072)
            if len(a) != len(b):
                raise ValueError("Validation files have different lengths")
            if not len(a):
                break
            finite = np.isfinite(a) & np.isfinite(b)
            with np.errstate(over="ignore", invalid="ignore", divide="ignore"):
                error = np.abs(a-b)
                tolerance = absolute_tolerance + relative_tolerance*np.abs(a)
                failed = ~finite | (error > tolerance)
                eligible = finite & np.isfinite(error)
                if np.any(eligible):
                    errors = np.where(eligible, error, -1.0)
                    index = int(np.argmax(errors))
                    if float(errors[index]) > maximum_absolute:
                        maximum_absolute = float(errors[index])
                        maximum_pixel = examined + index
                    relative = np.where(eligible & (a != 0), error / np.abs(a), 0.0)
                    maximum_relative = max(maximum_relative, float(np.max(relative)))
                    zero_reference = np.where(eligible & (a == 0), error, 0.0)
                    maximum_zero_reference = max(maximum_zero_reference, float(np.max(zero_reference)))
                failing += int(np.count_nonzero(failed))
                nonfinite += int(np.count_nonzero(~finite))
            examined += len(a)
    if examined != reference["pixel_count"]:
        raise ValueError("Compact grid length does not match its declared pixel count")
    return dict(passed=failing == 0, pixel_count=examined, failing_pixels=failing, nonfinite_pixels=nonfinite,
                max_abs_diff=maximum_absolute, max_abs_pixel_id=maximum_pixel,
                max_rel_diff=maximum_relative if math.isfinite(maximum_relative) else "Infinity",
                max_abs_diff_at_zero_reference=maximum_zero_reference,
                relative_error_definition="abs(candidate-reference)/abs(reference), over nonzero finite reference cells",
                tolerance_rule="abs(candidate-reference) <= absolute_tolerance + relative_tolerance*abs(reference)",
                absolute_tolerance=absolute_tolerance, relative_tolerance=relative_tolerance)


def validate_grids(observations, summaries, cache_directory):
    cache_directory = Path(cache_directory)
    cache_directory.mkdir(parents=True, exist_ok=True)
    by_case = {}
    for observation in observations:
        by_case.setdefault(observation["case_id"], []).append(observation)
    comparisons = []
    states = {}

    def compare(left, right, kind):
        key = digest({"version": 1, "left": left["validation_grid"]["sha256"], "right": right["validation_grid"]["sha256"],
                      "shape": left["validation_grid"]["shape"], "absolute": 1e-9, "relative": 1e-12})
        path = cache_directory / (key + ".json")
        result = read_json(path) if path.exists() else compare_grids(left["validation_grid"], right["validation_grid"])
        if not path.exists():
            atomic_json(path, result)
        record = dict(result, kind=kind, reference_case_id=left["case_id"], candidate_case_id=right["case_id"],
                      reference_repetition=left["repetition"], candidate_repetition=right["repetition"],
                      reference_grid_sha256=left["validation_grid"]["sha256"], candidate_grid_sha256=right["validation_grid"]["sha256"])
        comparisons.append(record)
        return result["passed"]

    complete = {s["case_id"] for s in summaries}
    for case_id in complete:
        rows = sorted(by_case[case_id], key=lambda r: r["repetition"])
        repeat_checks = [compare(rows[0], row, "repeat_consistency") for row in rows[1:]]
        states[case_id] = {"repeats_passed": all(repeat_checks), "cross_method_passed": False, "state": "pending_reference"}
    workloads = {}
    for summary in summaries:
        first = sorted(by_case[summary["case_id"]], key=lambda r: r["repetition"])[0]
        key = (summary["dataset"], summary["resolution"], summary["dataset_ratio_percent"], summary["operation"], summary["trial_id"])
        workloads.setdefault(key, []).append(first)
    for rows in workloads.values():
        contexts = {(r["dataset_fingerprint"], canonical(r["bbox"]), canonical(r.get("deleted_polygon_indices", []))) for r in rows}
        if len(contexts) != 1:
            raise ValueError("Grid comparison workload differs in dataset, viewport, or deleted polygon IDs")
        baselines = [r for r in rows if r["method"] != "SLED"]
        reference = min(baselines, key=lambda r: SUPPORTED_METHODS.index(r["method"])) if baselines else None
        if reference is not None and len(rows) > 1:
            passed = []
            for candidate in rows:
                if candidate["case_id"] == reference["case_id"]:
                    continue
                ok = compare(reference, candidate, "cross_method_first_repetition")
                states[candidate["case_id"]]["cross_method_passed"] = ok
                passed.append(ok)
            states[reference["case_id"]]["cross_method_passed"] = all(passed)
        delta = next((r for r in rows if r["method"] == "SLED" and r["variant"] == "delta"), None)
        full = next((r for r in rows if r["method"] == "SLED" and r["variant"] == "full"), None)
        if delta and full:
            ok = compare(full, delta, "full_vs_delta_first_repetition")
            states[delta["case_id"]]["full_delta_passed"] = ok
            states[full["case_id"]]["full_delta_passed"] = ok
    for summary in summaries:
        state = states[summary["case_id"]]
        failed = not state["repeats_passed"] or state.get("full_delta_passed") is False
        comparison_failure = any(not r["passed"] and summary["case_id"] in (r["reference_case_id"], r["candidate_case_id"])
                                 for r in comparisons)
        state["passed"] = state["repeats_passed"] and state["cross_method_passed"] and not failed and not comparison_failure
        state["state"] = "passed" if state["passed"] else ("failed" if failed or comparison_failure else "pending_reference")
        summary["numerical_validation"] = state
    return {"absolute_tolerance": 1e-9, "relative_tolerance": 1e-12, "cases": states, "comparisons": comparisons,
            "all_available_complete_cases_passed": bool(summaries) and all(s["numerical_validation"]["passed"] for s in summaries)}


def write_csv(path, rows, fields):
    with Path(path).open("w", newline="") as out:
        writer = csv.DictWriter(out, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(rows)


def aggregate_manifest(manifest):
    root = Path(manifest["output_directory"])
    observations = []
    completed_jobs = 0
    for job in manifest["jobs"]:
        receipt_path = root / "runs" / job["job_id"] / "completed.json"
        if not receipt_path.exists():
            continue
        receipt = read_json(receipt_path)
        if receipt["manifest_sha256"] != manifest["manifest_sha256"]:
            raise ValueError(f"Foreign receipt: {receipt_path}")
        rows = collect_attempt(manifest, job, receipt["attempt_directory"])
        if rows != receipt["observations"]:
            raise ValueError(f"Measurement files changed since completion: {receipt_path}")
        observations += rows
        completed_jobs += 1
    summaries, missing = summarize(manifest, observations)
    output = root / "aggregate"
    output.mkdir(parents=True, exist_ok=True)
    grid_validation = validate_grids(observations, summaries, output / "grid-comparison-cache")
    atomic_json(output / "grid-validation.json", grid_validation)
    atomic_json(output / "observations.json", observations)
    atomic_json(output / "summaries.json", summaries)
    atomic_json(output / "missing.json", missing)
    fields = ["case_id", "job_id", "repetition", "dataset", "method", "variant", "operation", "trial_id", "resolution",
              "dataset_ratio_percent", "computation_seconds", "serialization_seconds", "memory_peak_rss_mib", "dataset_fingerprint",
              "timing_contract", "source_path", "source_sha256", "manifest_sha256", "executable_sha256"]
    write_csv(output / "observations.csv", observations, fields)
    preparation_rows = []
    for job in manifest["jobs"]:
        candidates = [r for r in observations if r["job_id"] == job["job_id"]]
        if candidates:
            row = candidates[0]
            preparation_rows.append({"job_id": job["job_id"], "dataset": row["dataset"], "method": row["method"],
                                     "operation": row["operation"], "variant": row["variant"], "repetition": row["repetition"],
                                     **row["initialization"]})
    write_csv(output / "preparation.csv", preparation_rows, ["job_id", "dataset", "method", "operation", "variant", "repetition",
              "initialization_seconds", "interactive_preprocessing_seconds", "interactive_initial_density_seconds"])
    statistics_rows = []
    for dataset in manifest["dataset_inputs"]:
        candidates = [r for r in observations if r["dataset"] == dataset and r["method"] == "SLED" and "default" in r["families"]]
        if len(candidates) != manifest["repetitions"]:
            continue
        counts = {(r["cell_count"], r["input_edges"]) for r in candidates}
        if len(counts) != 1 or next(iter(counts))[1] is None:
            raise ValueError(f"Dataset edge/polygon counts missing or inconsistent across repetitions: {dataset}")
        n, edges = next(iter(counts))
        statistics_rows.append(dict(dataset=dataset, label=DATASETS.get(dataset, dataset), N=n, E=edges, E_avg=edges/n,
                                    source_case_id=candidates[0]["case_id"], dataset_fingerprint=candidates[0]["dataset_fingerprint"]))
    write_csv(output / "dataset-stats.csv", statistics_rows, ["dataset", "label", "N", "E", "E_avg", "source_case_id", "dataset_fingerprint"])
    stat_tex = ["% Counts verified identical across all three default SLED repetitions.",
                "% E counts ring edges, excluding repeated closure coordinates as additional edges.",
                r"\begin{tabular}{lrrr}", r"Dataset & $N$ & $E$ & $E/N$ \\", r"\hline"]
    for row in statistics_rows:
        stat_tex.append(f"{row['label']} & {row['N']:,} & {row['E']:,} & {row['E_avg']:.2f}" + r" \\")
    stat_tex.append(r"\end{tabular}")
    (output / "dataset-stats.tex").write_text("\n".join(stat_tex) + "\n")
    claims = {"metric": "computation_seconds", "timing_contract": CONTRACT, "preset": manifest["preset"], "families": {}}
    tex = ["% Generated from unrounded medians of complete three-repetition conditions.", "% Incomplete families expose Ready=0 and no numerical macros."]
    for family, macro in [("default", "Default"), ("resolution", "Resolution"), ("dataset", "Dataset"), ("interaction", "Interaction")]:
        expected_ids = {c["case_id"] for j in manifest["jobs"] for c in j["cases"] if family in c["families"]}
        available_ids = {s["case_id"] for s in summaries if family in s["families"]}
        pairs = matched_speedups(summaries, family)
        complete = bool(expected_ids) and available_ids == expected_ids and bool(pairs)
        complete = complete and all(s["numerical_validation"]["passed"] for s in summaries if family in s["families"])
        # Deadline and smoke data are never promoted into complete paper sweeps.
        if manifest["preset"] == "smoke" or (family in ("resolution", "dataset", "interaction") and manifest["preset"] != "full"):
            complete = False
        entry = dict(complete=complete, expected_conditions=len(expected_ids), completed_conditions=len(available_ids), paired_comparisons=len(pairs))
        tex.append(r"\newcommand{\RoundFour" + macro + "Ready}{" + str(int(complete)) + "}")
        if complete:
            least, most = min(pairs, key=lambda p: p["speedup"]), max(pairs, key=lambda p: p["speedup"])
            entry.update(minimum=least, maximum=most, comparisons=pairs)
            tex += [r"\newcommand{\RoundFour" + macro + "SpeedupMin}{" + f"{least['speedup']:.2f}" + "}",
                    r"\newcommand{\RoundFour" + macro + "SpeedupMax}{" + f"{most['speedup']:.2f}" + "}"]
        claims["families"][family] = entry
    atomic_json(output / "claims.json", claims)
    (output / "claims.tex").write_text("\n".join(tex) + "\n")
    validated = grid_validation["all_available_complete_cases_passed"]
    numerical_failure = any(v["state"] == "failed" for v in grid_validation["cases"].values())
    status = dict(state="numerical_failure" if numerical_failure else ("complete" if not missing and validated else "incomplete"),
                  complete=not missing and validated, measurements_complete=not missing, updated_at=utc_now(),
                  numerically_verified_conditions=sum(v["passed"] for v in grid_validation["cases"].values()),
                  preset=manifest["preset"], expected_jobs=len(manifest["jobs"]), completed_jobs=completed_jobs,
                  expected_observations=sum(len(j["cases"]) for j in manifest["jobs"]), completed_observations=len(observations),
                  complete_conditions=len(summaries), incomplete_conditions=len(missing), aggregate_directory=str(output))
    atomic_json(root / "progress.json", status)
    print(canonical(status), flush=True)
    if numerical_failure:
        raise ValueError(f"Numerical grid comparison failed: {output / 'grid-validation.json'}")
    return status


def parser():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    commands = p.add_subparsers(dest="action", required=True)
    init = commands.add_parser("init")
    init.add_argument("--preset", choices=("full", "deadline", "smoke"), default="full")
    init.add_argument("--output", type=Path, required=True)
    init.add_argument("--executable", type=Path, default=PGV / "build/sled")
    init.add_argument("--validation-report", type=Path, default=PGV / "build/validation.json")
    init.add_argument("--data-dir", type=Path, help="Directory of externally supplied *_records.jsonl files")
    init.add_argument("--datasets")
    init.add_argument("--methods")
    init.add_argument("--repetitions", type=int, default=3)
    run_p = commands.add_parser("run")
    run_p.add_argument("--manifest", type=Path, required=True)
    run_p.add_argument("--resume", action="store_true")
    run_p.add_argument("--dry-run", action="store_true")
    run_p.add_argument("--family", help="Comma-separated default,resolution,dataset,interaction,zoom,panning,updating,full-update")
    run_p.add_argument("--datasets")
    run_p.add_argument("--methods")
    run_p.add_argument("--max-jobs", type=int)
    for action in ("aggregate", "status"):
        command = commands.add_parser(action)
        command.add_argument("--manifest", type=Path, required=True)
    return p


def main():
    args = parser().parse_args()
    if args.action == "init":
        initialize(args)
    elif args.action == "run":
        run(args)
    elif args.action == "aggregate":
        aggregate_manifest(load_manifest(args.manifest))
    else:
        manifest = load_manifest(args.manifest)
        path = Path(manifest["output_directory"]) / "progress.json"
        print(canonical(read_json(path) if path.exists() else {"state": "not_started", "complete": False}))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        sys.exit(2)
