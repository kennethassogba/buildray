"""Independently check downloaded incremental corpus artifacts and write a summary.

Pass artifact directories named <project>-<mode> or <project>-<mode>-case-N.
--require-complete requires all 18 project/strategy/edit combinations.
"""
import argparse
import json
import math
import re
import statistics
import xml.etree.ElementTree as ET
from collections import Counter
from pathlib import Path

FILES = {
    "xgrammar": ["cpp/earley_parser.cc", "cpp/grammar_impl.h", "include/xgrammar/grammar.h"],
    "sentencepiece": ["src/sentencepiece_processor.cc", "src/common.h", "src/sentencepiece_processor.h"],
    "mujoco": ["src/user/user_model.cc", "src/user/user_model.h", "include/mujoco/mujoco.h"],
}
KINDS = ["implementation", "internal header", "public header"]
TESTS = {"xgrammar": 83, "sentencepiece": 1, "mujoco": 3519}


def close(actual, expected):
    if not math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-6):
        raise ValueError(f"Numeric mismatch: {actual} != {expected}")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def read_json(path):
    return json.loads(path.read_text())


def test_names(tests):
    """Keep stable GTest execution filters when opaque values contain ASLR pointers."""
    identities = []
    for test in tests:
        name = test["name"]
        match = re.fullmatch(r"([0-9]+)-byte object <[0-9A-Fa-f .-]+>", name.rsplit("/", 1)[-1])
        filters = [arg for arg in test.get("command", []) if arg.startswith("--gtest_filter=")]
        if match and "/" in name and len(filters) == 1:
            name = name.rsplit("/", 1)[0] + f"/{match[1]}-byte object <opaque> [{filters[0]}]"
        identities.append(name)
    return Counter(identities)


def verify(directory):
    project, mode, *_ = directory.name.split("-")
    require(project in FILES and mode in ("unity", "pch"), f"Unknown artifact: {directory}")
    report = read_json(directory / "report.json")
    incremental = report["incremental"]
    require(report["status"] == incremental["status"] == "passed", "Incomplete experiment")
    require(report["patch_validated"] and report["tests_required"], "Missing patch validation")
    require(report["runs"] == 3 and report["jobs"] == (4 if project == "mujoco" else 2), "Unexpected sampling")
    require(report["performance_result"] == "not_measured" and not report["patch_recommended"], "Unexpected clean result")
    require(incremental == read_json(directory / "incremental.json"), "Incremental reports differ")
    require(report["baseline_tests_passed"] == report["candidate_tests_passed"] == TESTS[project], "Test inventory changed")
    baseline_context = report["measurement_context"]
    candidate_context = report["candidate_measurement_context"]
    for key in ["session", "cpu_model", "cpu_affinity_count", "jobs", "cmake_args", "cache_flags", "environment_flags"]:
        require(baseline_context[key] == candidate_context[key], f"Context changed: {key}")
    if project == "mujoco":
        require("-DCMAKE_LINK_DEPENDS_USE_LINKER=OFF" in report["cmake_args"], "Missing MuJoCo readiness configuration")
    groups = {g["file"]: g["sources"] for g in report.get("unity_plan", {}).get("groups", [])}
    rows = []
    inventory = None
    for scenario in incremental["scenarios"]:
        require(scenario["status"] == "passed" and scenario["input_restored"], "Incomplete or unrestored scenario")
        require(scenario["file"] in FILES[project], "Unexpected edited file")
        row = {"project": project, "mode": mode, "kind": KINDS[FILES[project].index(scenario["file"])],
               "file": scenario["file"], "artifact": directory.name,
               "performance_result": scenario["performance_result"], "wall_change_percent": scenario["wall_change_percent"]}
        for variant in ["baseline", "candidate"]:
            samples = scenario[variant]["samples"]
            require(len(samples) == 3 and samples == scenario[variant + "_samples"], "Missing samples")
            times = [sample["wall_seconds"] for sample in samples]
            median = statistics.median(times)
            mad = statistics.median(abs(value - median) for value in times)
            close(median, scenario[variant]["median_seconds"])
            close(mad, scenario[variant]["mad_seconds"])
            counts = []
            for sample in samples:
                relative = Path(sample["log_prefix"])
                require(not relative.is_absolute() and ".." not in relative.parts, "Unsafe log path")
                prefix = directory / relative
                records = read_json(Path(str(prefix) + "-compilers.json"))
                require(len(records) == sample["compiler_invocations"] > 0, "Compiler count mismatch")
                require(all(record["exit_code"] == 0 for record in records), "Failed compiler command")
                require(max(record["peak_rss_kib"] for record in records) == sample["max_compiler_rss_kib"], "RSS mismatch")
                close(sum(record["user_seconds"] + record["system_seconds"] for record in records), sample["compiler_cpu_seconds"])
                require(sum("-emit-pch" in record["command"] for record in records) == sample["pch_creations"], "PCH count mismatch")
                require(sample["tests_passed"] == TESTS[project], "Missing tests")
                xml = ET.parse(Path(str(prefix) + "-tests.xml"))
                require(not any(list(xml.iter(tag)) for tag in ["failure", "error", "skipped"]), "JUnit failure or skip")
                junit_names = Counter(test.attrib["name"] for test in xml.iter("testcase"))
                tests = read_json(Path(str(prefix) + "-tests.json"))["tests"]
                require(junit_names == Counter(test["name"] for test in tests), "JUnit does not match its inventory")
                require(sum(junit_names.values()) == TESTS[project], "JUnit count mismatch")
                names = test_names(tests)
                if inventory is None:
                    inventory = names
                require(names == inventory, "JUnit names changed")
                require("ninja: no work to do." in Path(str(prefix) + "-noop.log").read_text(), "Missing idle control")
                originals = set()
                for source in sample["compiled_sources"]:
                    if source.startswith("<SOURCE>/"):
                        originals.add(source)
                    elif variant == "candidate":
                        originals.update(member for member in groups.get(source, []) if member.startswith("<SOURCE>/"))
                if "original_source_count" in sample:
                    require(len(originals) == sample["original_source_count"], "Input source count mismatch")
                    require(originals == set(sample["original_source_files"]), "Input sources mismatch")
                counts.append(len(originals))
            row[variant] = {"median_seconds": median, "mad_seconds": mad,
                            "compiler_invocations": sorted({s["compiler_invocations"] for s in samples}),
                            "original_source_counts": sorted(set(counts)),
                            "max_compiler_rss_kib": max(s["max_compiler_rss_kib"] for s in samples),
                            "pch_creations": sorted({s["pch_creations"] for s in samples}),
                            "tests_passed_per_sample": TESTS[project]}
        before, after = row["baseline"]["median_seconds"], row["candidate"]["median_seconds"]
        threshold = max(.02 * before, 3 * max(row["baseline"]["mad_seconds"], row["candidate"]["mad_seconds"]))
        close(threshold, scenario["noise_threshold_seconds"])
        verdict = "inconclusive" if abs(after - before) <= threshold else "improved" if after < before else "regressed"
        require(verdict == row["performance_result"], "Verdict mismatch")
        close(100 * (after - before) / before, row["wall_change_percent"])
        rows.append(row)
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", type=Path, nargs="+")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--require-complete", action="store_true")
    args = parser.parse_args()
    rows = [row for directory in args.directories for row in verify(directory)]
    keys = [(row["project"], row["mode"], row["file"]) for row in rows]
    require(len(keys) == len(set(keys)), "Duplicate scenarios")
    if args.require_complete:
        expected = {(project, mode, file) for project, files in FILES.items() for mode in ["unity", "pch"] for file in files}
        require(set(keys) == expected, "Corpus is incomplete")
    args.output.write_text(json.dumps(rows, indent=2) + "\n")
    for row in rows:
        print(f'{row["project"]} / {row["mode"]} / {row["kind"]}: '
              f'{row["baseline"]["median_seconds"]:.3f} -> {row["candidate"]["median_seconds"]:.3f}s '
              f'({row["wall_change_percent"]:+.2f}%, {row["performance_result"]})')
    print(f"Verified {len(rows)} scenarios")


if __name__ == "__main__":
    main()
