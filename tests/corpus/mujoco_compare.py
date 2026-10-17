"""Native MuJoCo validation; preserves the exact upstream float32 skip set.

This corpus-only harness does not change Buildray's strict no-skipped-tests policy.
Both variants include the same three new regression tests.
"""

from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ET

REVISION = "651bae11c3c38efe483f033b4b7e869e7ce3f4ec"
SINGLE_SKIPS = {
    "CoreConstraintTest.BoxShellPinnedParentWithFreejoint",
    "JacobianTest.WeldRotJacobian", "ForwardTest.NormalizeQuats", "RayTest.RayNormal",
    "BoxQPTest.BoundedQPvariations", "MjCGeomTest.ShellInertiaSphere",
    "MjCGeomTest.ShellInertiaCapsule", "MjCGeomTest.ShellInertiaCylinder",
    "MjCGeomTest.ShellInertiaEllipsoid", "MjCGeomTest.ShellInertiaBox",
    "UserFlexTest.BoundingBoxCoordinates", "UserFlexTest.StiffnessMatrix",
    "XMLWriterTest.WritesFrameDefaults", "XMLWriterTest.TrimsDefaults",
    "ElasticityTest.CantileverIntoCircle",
}


def inventory(path):
    result = Counter()
    normalized = 0
    opaque = re.compile(r"([0-9]+)-byte object <[0-9A-Fa-f .-]+>")
    for test in json.loads(path.read_text())["tests"]:
        name = test["name"]
        filters = [arg for arg in test.get("command", []) if arg.startswith("--gtest_filter=")]
        suffix = name.rsplit("/", 1)[-1]
        if filters and "/" in name and opaque.fullmatch(suffix):
            name = opaque.sub(r"\1-byte object <opaque>", name) + " [" + filters[-1] + "]"
            normalized += 1
        result[name] += 1
    return result, normalized


def main():
    source, work = (Path(arg).resolve() for arg in sys.argv[1:3])
    precision = sys.argv[3]
    assert precision in ("double", "single")
    work.mkdir(parents=True, exist_ok=False)
    patches = Path(__file__).resolve().parent
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
    assert revision == REVISION
    assert not subprocess.check_output(["git", "status", "--porcelain"], cwd=source)
    report = {"source_revision": revision, "precision": precision, "jobs": 4,
              "status": "running", "native_patch_validated": False,
              "scope": "native CMake/CTest validation; no clean-build performance comparison; exact upstream float32 skips permitted only by this corpus harness"}

    def save():
        (work / "validation.json").write_text(json.dumps(report, indent=2) + "\n")

    def run(args, label, timeout=1800):
        print(label, flush=True)
        started = time.monotonic()
        log = work / (label + ".log")
        with log.open("w") as stream:
            result = subprocess.run([str(arg) for arg in args], cwd=source,
                                    stdout=stream, stderr=subprocess.STDOUT, timeout=timeout)
        if result.returncode:
            print(log.read_text()[-16000:], flush=True)
            raise RuntimeError(f"{label} exited {result.returncode}; see {log}")
        return time.monotonic() - started

    def test(label):
        definitions = work / (label + "-inventory.log")
        run(["ctest", "--test-dir", build, "--show-only=json-v1"], label + "-inventory")
        junit = work / (label + "-tests.xml")
        seconds = run(["ctest", "--test-dir", build, "--output-on-failure", "--timeout", "120",
                       "--no-tests=error", "--output-junit", junit], label + "-tests", timeout=900)
        cases = ET.parse(junit).getroot().findall(".//testcase")
        assert not any(c.find("failure") is not None or c.find("error") is not None for c in cases)
        skipped = Counter(c.attrib["name"] for c in cases if c.find("skipped") is not None)
        expected = Counter({name: 1 for name in SINGLE_SKIPS}) if precision == "single" else Counter()
        assert skipped == expected, ("Unexpected skip set", skipped, expected)
        names, normalized = inventory(definitions)
        assert len(cases) == sum(names.values()) == 3522
        return {"registered": len(cases), "passed": len(cases) - sum(skipped.values()),
                "skipped": sorted(skipped.elements()), "failures": 0,
                "normalized_opaque_names": normalized,
                "ctest_seconds_observed": seconds}, names

    save()
    try:
        run(["git", "apply", "--check", patches / "mujoco-compare-tests.patch"], "check-tests-patch")
        run(["git", "apply", patches / "mujoco-compare-tests.patch"], "apply-tests-patch")
        build = work / "build"
        flags = "-DmjUSESINGLE" if precision == "single" else ""
        run(["cmake", "-S", source, "-B", build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_CXX_COMPILER=clang++-21", "-DCMAKE_C_COMPILER=clang-21",
             "-DCMAKE_C_FLAGS=" + flags, "-DCMAKE_CXX_FLAGS=" + flags,
             "-DCMAKE_UNITY_BUILD=OFF", "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON",
             "-DMUJOCO_BUILD_EXAMPLES=OFF", "-DMUJOCO_BUILD_SIMULATE=OFF",
             "-DMUJOCO_BUILD_TESTS=ON", "-DMUJOCO_TEST_PYTHON_UTIL=OFF"], "configure")
        run(["cmake", "--build", build, "--parallel", "4"], "baseline-build")
        report["baseline"], before_names = test("baseline")
        save()
        run(["git", "apply", "--check", patches / "mujoco-compare-refactor.patch"], "check-refactor")
        run(["git", "apply", patches / "mujoco-compare-refactor.patch"], "apply-refactor")
        run(["cmake", "--build", build, "--parallel", "4"], "candidate-build")
        report["candidate"], after_names = test("candidate")
        assert before_names == after_names, "Native test inventory changed"
        report.update(status="passed", native_patch_validated=True, identical_test_inventory=True,
                      refactor_patch_sha256=hashlib.sha256((patches / "mujoco-compare-refactor.patch").read_bytes()).hexdigest())
    except BaseException as error:
        report.update(status="failed", error=str(error) or type(error).__name__)
        raise
    finally:
        save()


if __name__ == "__main__":
    main()
