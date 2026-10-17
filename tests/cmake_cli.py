"""Real CMake link/compile usage propagation, conservative edits and failed experiments."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile

binary, compiler = sys.argv[1:]


def run(*args, expected=0, **kwargs):
    result = subprocess.run([str(a) for a in args], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, **kwargs)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def digest(tree):
    return {str(p.relative_to(tree)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in tree.rglob("*") if p.is_file()}


with tempfile.TemporaryDirectory(prefix="buildray cmake ") as folder:
    root = Path(folder)
    source = root / "source tree"
    source.mkdir()
    (source / "cmake dir").mkdir()
    (source / "main.cpp").write_text("#ifndef API\n#error missing usage requirements\n#endif\nint bridge(); int main() { return bridge(); }\n")
    (source / "bridge.cpp").write_text("int bridge() { return 0; }\n")
    cmake = source / "cmake dir/links.cmake"
    original = '''# target_link_libraries(sample PRIVATE wrong)
#[=[
target_link_libraries(sample PRIVATE wrong)
]=]
set(example [=[target_link_libraries(sample PRIVATE wrong)]=])
add_library(api INTERFACE)
target_compile_definitions(api INTERFACE API=1)
add_library(bridge STATIC bridge.cpp)
target_link_libraries(bridge PUBLIC api)
add_executable(sample main.cpp)
target_link_libraries(
  sample PRIVATE bridge
  api # retain this comment
)
'''
    (source / "CMakeLists.txt").write_text('''cmake_minimum_required(VERSION 4.2)
project(link_fixture LANGUAGES CXX)
include("cmake dir/links.cmake")
enable_testing()
add_test(NAME works COMMAND sample)
''')
    cmake.write_text(original)

    def invoke(label, *args, expected=0):
        before = digest(source)
        work = root / label
        run(binary, "prune-links", "--source", source, "--work-dir", work,
            "--compiler", compiler, "--runs", 3, "--jobs", 2, *args, expected=expected)
        assert digest(source) == before, "Original source tree changed"
        return work, json.loads((work / "report.json").read_text())

    work, report = invoke("private")
    assert report["patch_validated"] and report["cmake_graph_change_verified"]
    assert report["removed_links"] == 1 and report["baseline_tests_passed"] == report["candidate_tests_passed"] == 1
    assert report["selected_link"]["alternate_path"] == ["sample", "bridge", "api"]
    assert report["selected_link"]["file"] == "cmake dir/links.cmake"
    assert len(report["baseline"]["samples"]) == len(report["candidate"]["samples"]) == 3
    plan = json.loads((work / "links.json").read_text())
    assert plan["selection"] == report["selected_link"] and len(plan["candidates"]) == 1
    run("git", "apply", "--check", work / "cmake.patch", cwd=source)
    patch = (work / "cmake.patch").read_text()
    assert "wrong" not in patch and "retain this comment" in patch
    assert not (work / "includes.patch").exists()
    # The patch must also work independently of Buildray's compiler launcher.
    run("git", "apply", work / "cmake.patch", cwd=source)
    run("cmake", "-S", source, "-B", root / "applied", "-G", "Ninja", f"-DCMAKE_CXX_COMPILER={compiler}")
    run("cmake", "--build", root / "applied", "--parallel", 2)
    run("ctest", "--test-dir", root / "applied", "--output-on-failure")
    cmake.write_text(original)

    # An unscoped executable declaration is supported too.
    cmake.write_text(original.replace("sample PRIVATE bridge", "sample bridge"))
    _, legacy = invoke("legacy", "--target", "sample", "--dependency", "api")
    assert legacy["patch_validated"]
    assert legacy["selected_link"]["alternate_path"] == ["sample", "bridge", "api"]

    # PUBLIC removal must preserve the interface of a library for its consumers.
    cmake.write_text(original.replace("add_executable(sample main.cpp)", "add_library(core STATIC bridge.cpp)\nadd_executable(sample main.cpp)\ntarget_link_libraries(sample PRIVATE core)")
                    .replace("sample PRIVATE bridge", "core PUBLIC bridge"))
    _, public = invoke("public", "--target", "core")
    assert public["patch_validated"] and public["selected_link"]["interface_path"] == ["core", "bridge", "api"]

    # LINK_ONLY is not evidence that compilation usage requirements survive.
    cmake.write_text(original.replace("target_link_libraries(bridge PUBLIC api)", "target_link_libraries(bridge PRIVATE api)"))
    no_work, none = invoke("link-only")
    assert none["performance_result"] == "no_changes" and not none["patch_validated"]
    assert "baseline" not in none and not (no_work / "cmake.patch").exists()
    _, required = invoke("required", "--dependency", "api", expected=1)
    assert "no eligible" in required["error"]

    cmake.write_text(original.replace("target_link_libraries(bridge PUBLIC api)", 'target_link_libraries(bridge PUBLIC "$<COMPILE_ONLY:api>")'))
    _, compile_only = invoke("compile-only")
    assert compile_only["performance_result"] == "no_changes"

    # PUBLIC api with only a PRIVATE bridge cannot lose its sole exported path.
    cmake.write_text(original.replace("add_executable(sample main.cpp)", "add_library(core STATIC bridge.cpp)\nadd_executable(sample main.cpp)\ntarget_link_libraries(sample PRIVATE core)")
                    .replace("sample PRIVATE bridge\n  api", "core PRIVATE bridge PUBLIC\n  api"))
    _, protected = invoke("exported", "--target", "core")
    assert protected["performance_result"] == "no_changes"

    # CMake variables, quoted/bracket arguments and duplicate occurrences are not edited.
    for label, replacement in [("variable", "${chosen}"), ("quoted", '"api"'),
                               ("bracket", "[=[api]=]"), ("duplicate", "api api")]:
        cmake.write_text("set(chosen api)\n" + original.replace("  api #", f"  {replacement} #"))
        _, untouched = invoke(label)
        assert untouched["performance_result"] == "no_changes"

    cmake.write_text(original)
    _, excluded = invoke("excluded", "--exclude", "cmake dir")
    assert excluded["performance_result"] == "no_changes"

    # Redundant graph edges do not guarantee semantic equivalence of arbitrary CMake code.
    # A behavioral change is caught by the complete candidate tests.
    cmake.write_text(original + '''get_target_property(sample_links sample LINK_LIBRARIES)
if("api" IN_LIST sample_links)
  target_compile_definitions(sample PRIVATE CONFIG_OK=1)
else()
  target_compile_definitions(sample PRIVATE CONFIG_OK=0)
endif()
''')
    (source / "main.cpp").write_text("int main() { return CONFIG_OK ? 0 : 1; }\n")
    failed_work, failed = invoke("runtime-failure", expected=1)
    assert failed["status"] == "failed" and not failed["patch_validated"]
    assert not failed["patch_recommended"] and "candidate-tests.log" in failed["error"]
    assert (failed_work / "cmake.patch").exists()
    assert "baseline" not in failed

    (source / "main.cpp").write_text("int main() { return 0; }\n")
    cmake.write_text(original + '''get_target_property(sample_links sample LINK_LIBRARIES)
if(NOT "api" IN_LIST sample_links)
  add_library(unexpected INTERFACE)
endif()
''')
    _, unexpected = invoke("graph-change", expected=1)
    assert not unexpected["patch_validated"] and "changed targets" in unexpected["error"]

    cmake.write_text(original + 'target_link_libraries(sample PRIVATE api)\n')
    _, repeated_call = invoke("retained-link", expected=1)
    assert not repeated_call["patch_validated"] and "retained the direct link" in repeated_call["error"]

    cmake.write_text(original + '''get_target_property(sample_links sample LINK_LIBRARIES)
if(NOT "api" IN_LIST sample_links)
  target_link_libraries(sample PRIVATE m)
endif()
''')
    _, raw_argument = invoke("raw-argument-change", expected=1)
    assert not raw_argument["patch_validated"] and "raw linker arguments" in raw_argument["error"]

    # Swapping a test for another with the same count cannot count as validation.
    cmake.write_text(original)
    root_cmake = source / "CMakeLists.txt"
    root_original = root_cmake.read_text()
    root_cmake.write_text(root_original.replace("add_test(NAME works COMMAND sample)", '''get_target_property(sample_links sample LINK_LIBRARIES)
if("api" IN_LIST sample_links)
  add_test(NAME works COMMAND sample)
else()
  add_test(NAME replacement COMMAND sample)
endif()'''))
    _, swapped = invoke("test-inventory", expected=1)
    assert not swapped["patch_validated"] and "CTest inventory changed" in swapped["error"]
    root_cmake.write_text(root_original)

    run(binary, "prune-links", "--runs", 1, expected=2)
    run(binary, "measure", "--dependency", "api", expected=2)
    run(binary, "prune-links", "--header", "<vector>", expected=2)

print("CMake link experiments passed")
