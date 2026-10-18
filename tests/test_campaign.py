"""Pipeline contract tests use temporary fixtures, never paper result folders."""
import copy
import json
import struct
from pathlib import Path
import tempfile
import unittest

import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
import campaign as c


class CampaignTests(unittest.TestCase):
    def manifest(self, preset="smoke", methods=None):
        return dict(timing_contract=c.CONTRACT, repetitions=3, preset=preset,
                    manifest_sha256="test-only-manifest", executable_sha256="test-only-binary",
                    jobs=c.make_jobs(preset, 3, ["dataset_ratio"], methods or ["spatial_join", "SLED"]))

    def observation(self, spec, rep, seconds):
        return dict(spec, repetition=rep, job_id=f"test-{rep}", timing_contract=c.CONTRACT,
                    manifest_sha256="test-only-manifest", computation_seconds=seconds,
                    serialization_seconds=0.0, memory_peak_rss_mib=64.0,
                    dataset_fingerprint="test-only-input", bbox={"x_min": 0, "y_min": 0, "x_max": 1, "y_max": 1},
                    phases={}, initialization={}, source_path=f"test-only-{spec['case_id']}-{rep}")

    def test_full_campaign_counts_and_default_deduplication(self):
        jobs = c.make_jobs("full")
        self.assertEqual(len(jobs), 492)
        self.assertEqual(sum(len(j["cases"]) for j in jobs), 1008)
        defaults = [s for j in jobs for s in j["cases"] if "default" in s["families"]]
        self.assertEqual(len(defaults), 48)
        self.assertEqual(len({s["case_id"] for s in defaults}), 16)
        self.assertTrue(all({"resolution", "dataset"} <= set(s["families"]) for s in defaults))

    def test_deadline_is_exactly_scoped(self):
        jobs = c.make_jobs("deadline")
        observations = [s for j in jobs for s in j["cases"]]
        self.assertEqual(len(observations), 108)
        self.assertEqual({s["trial_id"] for s in observations}, {"static", "zoom_0p5", "pan_01", "delete_10"})
        self.assertEqual(len([s for s in observations if s["variant"] == "full"]), 12)

    def test_partial_repetitions_do_not_become_summary(self):
        m = self.manifest(methods=["SLED"])
        s = m["jobs"][0]["cases"][0]
        summary, missing = c.summarize(m, [self.observation(s, 1, 1), self.observation(s, 2, 2)])
        self.assertEqual(summary, [])
        self.assertEqual(missing[0]["present_repetitions"], [1, 2])

    def test_duplicate_repetition_rejected(self):
        m = self.manifest(methods=["SLED"])
        r = self.observation(m["jobs"][0]["cases"][0], 1, 1)
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            c.summarize(m, [r, r])

    def test_old_contract_rejected(self):
        m = self.manifest(methods=["SLED"])
        r = self.observation(m["jobs"][0]["cases"][0], 1, 1)
        r["timing_contract"] = "legacy"
        with self.assertRaisesRegex(ValueError, "mix timing"):
            c.summarize(m, [r])

    def test_fingerprint_change_rejected(self):
        m = self.manifest(methods=["SLED"])
        s = m["jobs"][0]["cases"][0]
        rows = [self.observation(s, rep, rep) for rep in (1, 2, 3)]
        rows[2]["dataset_fingerprint"] = "changed"
        with self.assertRaisesRegex(ValueError, "Inputs differ"):
            c.summarize(m, rows)

    def test_unrounded_median_ratio_and_representative_phase(self):
        m = self.manifest()
        specs = {s["method"]: s for j in m["jobs"] for s in j["cases"]}
        rows = []
        for method, values in [("spatial_join", [0.10001, 0.20001, 0.30001]), ("SLED", [0.010001, 0.020001, 0.040001])]:
            for rep, value in enumerate(values, 1):
                r = self.observation(specs[method], rep, value)
                r["phases"] = {"test_phase_seconds": value}
                rows.append(r)
        summaries, missing = c.summarize(m, rows)
        self.assertFalse(missing)
        speed = c.matched_speedups(summaries, "default")[0]["speedup"]
        self.assertEqual(speed, 0.20001 / 0.020001)
        self.assertNotEqual(speed, 10.0)
        self.assertEqual(summaries[0]["representative_repetition"], 2)

    def test_mismatched_viewports_cannot_be_compared(self):
        m = self.manifest()
        specs = {s["method"]: s for j in m["jobs"] for s in j["cases"]}
        rows = [self.observation(s, rep, rep) for s in specs.values() for rep in (1, 2, 3)]
        summaries, _ = c.summarize(m, rows)
        summaries[0]["bbox"] = {"different": True}
        with self.assertRaisesRegex(ValueError, "different dataset fingerprints or viewports"):
            c.matched_speedups(summaries, "default")

    def test_full_update_not_used_as_main_sled_denominator(self):
        jobs = c.make_jobs("deadline", datasets=["dataset_ratio"])
        variants = [s for j in jobs for s in j["cases"] if s["operation"] == "updating"]
        self.assertEqual({s["variant"] for s in variants if s["method"] == "SLED"}, {"delta", "full"})
        self.assertTrue(all("updating" not in s["families"] for s in variants if s["variant"] == "full"))

    def test_run_info_requires_contract_full_grid_and_export_time(self):
        m = self.manifest(methods=["SLED"])
        job = m["jobs"][0]
        with tempfile.TemporaryDirectory() as temporary:
            info = Path(temporary) / "run_info.txt"
            values = dict(method="SLED", dataset="dataset_ratio", resolution="4x4", runtime_seconds="2",
                          computation_seconds="0.125", serialization_seconds="0", timing_contract=c.CONTRACT,
                          common_input_state="CellRecord_geometry_JSON_in_memory", result_state="complete_dense_grid_in_memory", output_discarded="true",
                          dataset_fingerprint="fnv1a64:test", memory_peak_rss_bytes=str(2**20), pixel_count="16", cell_count="5",
                          bbox=json.dumps({"x_min": 0, "y_min": 0, "x_max": 1, "y_max": 1}))
            grid = Path(temporary) / "validation_density.f64le"
            grid.write_bytes(struct.pack("<16d", *range(16)))
            values.update(validation_grid_path=str(grid), validation_grid_shape="[4,4]",
                          validation_grid_format="ieee754_float64_le", validation_grid_order="row_major_top_down_pixel_id",
                          validation_grid_pixel_count="16", validation_grid_bytes="128")
            def write():
                info.write_text("\n".join(f"{k}={v}" for k,v in values.items()))
            write()
            row = c.collect_attempt(m, job, temporary)[0]
            self.assertEqual(row["memory_peak_rss_mib"], 1)
            values["pixel_count"] = "15"
            write()
            with self.assertRaisesRegex(ValueError, "Incomplete grid"):
                c.collect_attempt(m, job, temporary)
            values["pixel_count"] = "16"
            del values["timing_contract"]
            write()
            with self.assertRaisesRegex(ValueError, "Legacy/missing"):
                c.collect_attempt(m, job, temporary)

    def test_command_has_dense_discard_and_trial_filter(self):
        m = self.manifest("deadline")
        m.update(executable="/test/pgv", dataset_inputs={"dataset_ratio": {"path": "/test/records"}})
        job = next(j for j in m["jobs"] if j["cases"][0]["variant"] == "full")
        cmd = c.command_for(m, job, Path("/test/output"))
        self.assertIn("--discard-output", cmd)
        self.assertIn("--validation-grid", cmd)
        self.assertEqual(cmd[cmd.index("--interactive-trials")+1], "delete_10")
        self.assertEqual(cmd[cmd.index("--sled-update-mode")+1], "full")

    def test_nan_and_nonpositive_timings_rejected(self):
        for value in ("nan", "inf", "0", "-1"):
            with self.assertRaises(ValueError):
                c.positive(value, "computation_seconds")

    def grid(self, directory, name, values):
        path = Path(directory) / name
        path.write_bytes(struct.pack("<" + str(len(values)) + "d", *values))
        return dict(path=str(path), sha256=c.file_hash(path), pixel_count=len(values), shape=[1, len(values)])

    def test_full_grid_tolerance_and_nonfinite_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            ref = self.grid(directory, "ref", [0., 1., 1e6])
            within = self.grid(directory, "within", [5e-10, 1., 1e6 + 5e-7])
            result = c.compare_grids(ref, within)
            self.assertTrue(result["passed"])
            self.assertEqual(result["pixel_count"], 3)
            bad = self.grid(directory, "bad", [2e-9, float("nan"), 1e6])
            result = c.compare_grids(ref, bad)
            self.assertFalse(result["passed"])
            self.assertEqual(result["failing_pixels"], 2)
            self.assertEqual(result["nonfinite_pixels"], 1)
            short = self.grid(directory, "short", [0.])
            with self.assertRaisesRegex(ValueError, "differently shaped"):
                c.compare_grids(ref, short)

    def test_repeat_checks_need_an_independent_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            m = self.manifest(methods=["SLED"])
            spec = m["jobs"][0]["cases"][0]
            rows = [self.observation(spec, rep, rep) for rep in (1, 2, 3)]
            for row in rows:
                row["validation_grid"] = self.grid(directory, str(row["repetition"]), [0., 1.])
            summaries, _ = c.summarize(m, rows)
            report = c.validate_grids(rows, summaries, Path(directory) / "cache")
            self.assertFalse(report["all_available_complete_cases_passed"])
            self.assertEqual(summaries[0]["numerical_validation"]["state"], "pending_reference")

    def test_cross_method_and_repeat_failure_are_reported(self):
        with tempfile.TemporaryDirectory() as directory:
            m = self.manifest()
            specs = {s["method"]: s for j in m["jobs"] for s in j["cases"]}
            rows = [self.observation(s, rep, rep) for s in specs.values() for rep in (1, 2, 3)]
            for i, row in enumerate(rows):
                row["validation_grid"] = self.grid(directory, str(i), [0., 1.])
            summaries, _ = c.summarize(m, rows)
            self.assertTrue(c.validate_grids(rows, summaries, Path(directory) / "cache")["all_available_complete_cases_passed"])
            bad = rows[-1]
            bad["validation_grid"] = self.grid(directory, "changed", [0., 2.])
            report = c.validate_grids(rows, summaries, Path(directory) / "cache")
            self.assertFalse(report["all_available_complete_cases_passed"])
            self.assertTrue(any(r["kind"] == "repeat_consistency" and not r["passed"] for r in report["comparisons"]))

    def test_full_delta_pair_is_compared_even_without_baseline(self):
        with tempfile.TemporaryDirectory() as directory:
            jobs = c.make_jobs("deadline", datasets=["dataset_ratio"], methods=["SLED"])
            specs = {s["variant"]: s for j in jobs for s in j["cases"] if s["operation"] == "updating"}
            rows = [self.observation(s, rep, rep) for s in specs.values() for rep in (1, 2, 3)]
            for i, row in enumerate(rows):
                row["validation_grid"] = self.grid(directory, str(i), [0., 1. if row["variant"] == "full" else 2.])
            summaries = list(copy.deepcopy(specs).values())
            report = c.validate_grids(rows, summaries, Path(directory) / "cache")
            pairs = [r for r in report["comparisons"] if r["kind"] == "full_vs_delta_first_repetition"]
            self.assertEqual(len(pairs), 1)
            self.assertFalse(pairs[0]["passed"])
            self.assertTrue(all(s["numerical_validation"]["state"] == "failed" for s in summaries))


if __name__ == "__main__":
    unittest.main()
