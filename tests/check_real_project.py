"""Check validation evidence without requiring a speedup on a shared runner."""

import json
from pathlib import Path
import sys

report = json.loads(Path(sys.argv[1]).read_text())
assert report["status"] == "passed", report
assert report["validation"] == "build_and_tests_passed", report
assert report["patch_validated"] is True, report
assert report["removed_includes"] >= 1, report
assert report["baseline_tests_passed"] == report["candidate_tests_passed"] >= 1, report
assert report["translation_units_analyzed"] == 2, report
assert len(report["baseline"]["samples"]) == len(report["candidate"]["samples"]) == report["runs"]
assert all(sample["compiler_cpu_seconds"] > 0 for variant in ("baseline", "candidate")
           for sample in report[variant]["samples"])
print("Real-project validation passed; performance result:", report["performance_result"])
