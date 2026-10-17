"""Diagnose library readiness only; this is not a benchmark or runtime validation."""
import json
import shutil
import subprocess
import sys
from pathlib import Path


def main():
    original, work = (Path(arg).resolve() for arg in sys.argv[1:])
    source, build, evidence = work / "source", work / "build", work / "evidence"
    evidence.mkdir(parents=True)
    shutil.copytree(original, source, ignore=shutil.ignore_patterns(".git", ".buildray"))
    report = {"scope": "library linkage diagnosis; no timing or runtime validation",
              "revision": "651bae11c3c38efe483f033b4b7e869e7ce3f4ec", "status": "running", "variants": []}

    def execute(args, name):
        print(name, flush=True)
        result = subprocess.run([str(arg) for arg in args], text=True, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, check=False)
        (evidence / name).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(f"{name}: exit {result.returncode}: {result.stdout[-2500:]}")
        return result.stdout

    try:
        execute(["ld", "--version"], "system-linker-version.log")
        for label in ["default", "without-linker-depfiles"]:
            if label != "default":
                (build / "CMakeFiles/mujoco.dir/link.d").unlink(missing_ok=True)
            args = ["cmake", "-S", source, "-B", build, "-G", "Ninja",
                    "-DCMAKE_CXX_COMPILER=clang++-21", "-DCMAKE_C_COMPILER=clang-21",
                    "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON",
                    "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON", "-DCMAKE_UNITY_BUILD=OFF",
                    "-DMUJOCO_BUILD_EXAMPLES=OFF", "-DMUJOCO_BUILD_SIMULATE=OFF",
                    "-DMUJOCO_BUILD_TESTS=ON", "-DMUJOCO_TEST_PYTHON_UTIL=OFF"]
            if label != "default":
                args.append("-DCMAKE_LINK_DEPENDS_USE_LINKER=OFF")
            execute(args, label + "-configure.log")
            command = ["cmake", "--build", build, "--target", "mujoco", "--parallel", "4",
                       "--", "-d", "explain", "-d", "keepdepfile", "-v"]
            execute(command, label + "-initial-build.log")
            controls = []
            for index in range(3):
                output = execute(command, f"{label}-control-{index}.log")
                controls.append({"index": index, "idle": "ninja: no work to do." in output})
            report["variants"].append({"configuration": label, "controls": controls})
            for relative in ["CMakeCache.txt", "build.ninja", "CMakeFiles/rules.ninja", "CMakeFiles/mujoco.dir/link.d"]:
                path = build / relative
                if path.is_file():
                    shutil.copyfile(path, evidence / (label + "-" + path.name))
            library = next(p for p in (build / "lib").glob("libmujoco.so.*") if not p.is_symlink())
            execute(["ninja", "-C", build, "-t", "deps", library.relative_to(build)], label + "-link-deps.log")
            (evidence / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        report["status"] = "complete"
    except Exception as error:
        report.update(status="failed", error=str(error))
        raise
    finally:
        (evidence / "report.json").write_text(json.dumps(report, indent=2) + "\n")


if __name__ == "__main__":
    main()
