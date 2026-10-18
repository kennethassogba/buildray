#include <charconv>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <system_error>

#include "buildray/core.hpp"

static_assert(__cplusplus > 202302L, "Buildray requires C++26 mode");

namespace {
constexpr const char* help = "Buildray " BUILDRAY_VERSION R"( - measured C++ builds

Usage:
  buildray optimize --source DIR [options] [-- -DNAME=VALUE ...]
  buildray headers --source DIR [--forward-declarations] [options] [-- -DNAME=VALUE ...]
  buildray measure  --source DIR [options] [-- -DNAME=VALUE ...]
  buildray profile --source DIR [options] [-- -DNAME=VALUE ...]
  buildray deps --source DIR [--c-compiler PATH] [--fail-on-cycles] [options]
                [-- -DNAME=VALUE ...]
  buildray pch --source DIR --target NAME --header HEADER [--header HEADER ...]
               [options] [-- -DNAME=VALUE ...]
  buildray pch --auto --source DIR [--target NAME] [options] [-- -DNAME=VALUE ...]
  buildray unity --source DIR --target NAME [--batch-size N] [options]
                 [--unity-exclude PATH ...] [-- -DNAME=VALUE ...]
  buildray prune-links --source DIR [--target NAME] [--dependency NAME] [options]
                       [-- -DNAME=VALUE ...]
  buildray prune-deps --source DIR [--target NAME] [--dependency NAME] [options]
                      [-- -DNAME=VALUE ...]
  buildray compare --baseline-source DIR --source DIR [options] [-- -DNAME=VALUE ...]
  buildray check --baseline REPORT --current REPORT [--output FILE]
                 [--max-regression-percent N]

Options:
  --source DIR          CMake source tree (default: current directory)
  --work-dir DIR        New output directory (default: SOURCE/.buildray/unique-id)
  --compiler PATH       Clang C++ compiler (default: clang++)
  --c-compiler PATH     Clang C compiler for deps/headers/unity or incremental pch
  --fail-on-cycles      deps: return 5 if include or target-link cycles exist
  --clang-tidy PATH     clang-tidy with misc-include-cleaner (default: clang-tidy)
  --runs N              Samples per variant and rebuild scenario, 1..100 (default: 3)
  --rebuild-file PATH   pch/unity/prune-deps: measure a single-file rebuild; repeatable
  --incremental-only    Skip repeated clean timings; requires --rebuild-file (full validation still runs)
  --jobs N              Fixed build parallelism, 1..1024 (default: 2)
  --allow-no-tests      Permit build-only validation when no CTest tests exist
  --baseline-source DIR  Base revision source for paired comparison
  --max-regression-percent N  Regression budget (default: 5)
  --session ID          Optional label shared by comparable measurements
  --target NAME         PCH/unity target, or optional prune-links target restriction
  --batch-size N        unity: maximum sources per batch, 2..32 (default: 4)
  --unity-exclude PATH  unity: keep source files under this relative path separate; repeatable
  --unity-retries N     unity: retry failed compiler groups with their sources separate, 0..8 (default: 0)
  --dependency NAME     prune-links: restrict selection to this direct dependency
  --header HEADER       PCH header: <system> or source-root-relative path; repeatable
  --auto                pch: profile, select one project header/target, validate and measure
  --exclude PATH        optimize/headers/prune-links: skip edits to a relative path; repeatable
  --forward-declarations headers: try replacing eligible project includes with class/struct declarations
  --header-path PATH    headers: restrict edited headers to a relative file/directory; repeatable
  --max-header-edits N  headers: accepted header files per run, 1..1000 (default: 8)
  --help                Show this help
  --version             Print version

optimize analyzes C++ source files, removes unused includes in an isolated copy,
validates the candidate, and compares repeated clean builds. The original tree
is never edited. Outputs: report.json, includes.patch, compiler records, logs.
Patches are validated only for the supplied build configuration. Headers are
not edited. Unity builds and PCH are disabled in both measured variants.

compare validates both revisions and alternates their clean builds on this
machine. check compares saved measure reports and refuses incompatible context.
Both require at least three samples. Reports and Markdown summaries are retained.

profile runs one instrumented build with Clang time traces and CTest, then ranks
headers by inclusive parse time and target. Profiling overhead is not a benchmark.
pch tests a private C++ PCH on one target, validates it, and alternates clean
builds against a baseline with PCH disabled. Outputs: pch.patch and report.json.
PCH generation is included in candidate timings. Neither command edits your tree.
With --auto, pch profiles first and selects the highest inclusive-cost project
header used by at least two compilations of a target. At least three pairs are
required. Selection is a heuristic; only measured improvement recommends a patch.
deps captures C/C++ include edges (including guarded/skipped includes), reports
cycles, header fan-out, and CMake target relationships. Requires CMake 4.2+ and
Ninja 1.13+. Outputs: dependencies.json, dependencies.md, dependencies.dot.
Cycles are structural findings; guarded includes and static link cycles can be valid.
prune-links selects one literal CMake link with an alternate transitive path for
both link and compilation usage requirements. It preserves exported reachability,
reconfigures, checks graph changes, builds and tests, then measures three or more
clean-build pairs. Outputs: cmake.patch, links.json, before/after CMake graphs.
No eligible candidate completes without a patch or benchmark. This is configured
graph redundancy, not proof that a library's symbols are unused.
prune-deps tests private literal static-library dependencies with ELF LLD.
It preserves compiler commands, requires zero extracted members from every removed
archive, and replays the link with identical baseline objects to require identical
ELF bytes. Full builds and tests follow; clean and incremental timings include
link-driver counts, wall/CPU time and individual peak RSS. Shared libraries,
exported interfaces, variables and response files are not removed. Configure LLD
explicitly through project CMake flags; Buildray never switches the linker itself.
headers analyzes observed original project headers with Clang AST and include-cleaner.
Each candidate must compile standalone in observed C++ contexts and in all observed
C/C++ users, then pass the complete clean build and CTest. --forward-declarations
tries simple project class/struct declarations for pointer/reference uses. Templates,
aliases, nested classes, inline/anonymous namespaces and attributed records are kept.
Outputs: headers.patch and header-cleanup.json. Original headers stay unchanged.
unity tests an opt-in CMake unity build on one target, with PCH disabled.
It verifies original C/C++ source/target coverage, builds, tests and measures the
complete default build. --unity-retries can isolate failed compiler groups before
validation. Link, runtime and coverage failures always reject the patch.
Outputs: unity.patch and unity.json. Enable an applied patch with -DBUILDRAY_UNITY=ON.
Add --rebuild-file for incremental timings after single-file comment edits. Each
sample requires an up-to-date build first, then compiles, links and passes CTest.
--incremental-only skips clean timings but keeps full baseline/candidate validation.
Outputs: incremental.json and incremental.md. Per-scenario regressions prevent a
clean-build patch recommendation; compiler RSS is not aggregate build memory.

Exit codes: 0 completed, 1 pipeline/validation failure, 2 invalid arguments,
3 regression over budget and noise, 4 incomparable reports, 5 cycles, 130 interrupted.
No patch is applied automatically.
)";

unsigned number(const std::string& value, unsigned maximum) {
    unsigned parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
        parsed > maximum)
        throw std::runtime_error("Invalid numeric option: " + value);
    return parsed;
}

double budget(const std::string& value) {
    double parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(parsed) ||
        parsed < 0 || parsed > 1000)
        throw std::runtime_error("Regression budget must be between 0 and 1000 percent");
    return parsed;
}

buildray::CheckOptions parse_check(const std::vector<std::string>& args) {
    buildray::CheckOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg != "--baseline" && arg != "--current" && arg != "--output" &&
            arg != "--max-regression-percent")
            throw std::runtime_error("Unknown check option: " + arg);
        if (++i == args.size() || args[i].empty() || args[i].starts_with("--"))
            throw std::runtime_error("Missing value for " + arg);
        if (arg == "--baseline")
            options.baseline = args[i];
        else if (arg == "--current")
            options.current = args[i];
        else if (arg == "--output")
            options.output = args[i];
        else
            options.max_regression_percent = budget(args[i]);
    }
    if (options.baseline.empty() || options.current.empty())
        throw std::runtime_error("check requires --baseline and --current reports");
    return options;
}

buildray::Options parse(const std::vector<std::string>& args) {
    buildray::Options options;
    options.command = args.front();
    bool explicit_c_compiler = false;
    if (options.command != "optimize" && options.command != "measure" &&
        options.command != "compare" && options.command != "profile" && options.command != "pch" &&
        options.command != "deps" && options.command != "prune-links" && options.command != "prune-deps" && options.command != "headers" && options.command != "unity")
        throw std::runtime_error("Unknown command: " + options.command);
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "--") {
            options.cmake_args.assign(args.begin() + static_cast<std::ptrdiff_t>(i + 1),
                                      args.end());
            break;
        }
        if (arg == "--allow-no-tests") {
            options.allow_no_tests = true;
            continue;
        }
        if (arg == "--fail-on-cycles") {
            options.fail_on_cycles = true;
            continue;
        }
        if (arg == "--auto") {
            options.auto_pch = true;
            continue;
        }
        if (arg == "--forward-declarations") {
            options.forward_declarations = true;
            continue;
        }
        if (arg == "--incremental-only") {
            options.incremental_only = true;
            continue;
        }
        if (arg != "--source" && arg != "--work-dir" && arg != "--compiler" &&
            arg != "--clang-tidy" && arg != "--runs" && arg != "--jobs" &&
            arg != "--baseline-source" && arg != "--max-regression-percent" && arg != "--session" &&
            arg != "--target" && arg != "--header" && arg != "--c-compiler" && arg != "--exclude" && arg != "--dependency" &&
            arg != "--header-path" && arg != "--max-header-edits" && arg != "--batch-size" && arg != "--unity-exclude" && arg != "--unity-retries" && arg != "--rebuild-file")
            throw std::runtime_error("Unknown option: " + arg);
        if (++i == args.size() || args[i].starts_with("--"))
            throw std::runtime_error("Missing value for " + arg);
        const auto& value = args[i];
        if (value.empty()) throw std::runtime_error("Empty value for " + arg);
        if (arg == "--max-header-edits" && options.command != "headers")
            throw std::runtime_error("--max-header-edits requires headers");
        if ((arg == "--batch-size" || arg == "--unity-exclude" || arg == "--unity-retries") && options.command != "unity")
            throw std::runtime_error(arg + " requires unity");
        if (arg == "--source")
            options.source = value;
        else if (arg == "--work-dir")
            options.work = value;
        else if (arg == "--compiler")
            options.compiler = value;
        else if (arg == "--c-compiler") {
            options.c_compiler = value;
            explicit_c_compiler = true;
        }
        else if (arg == "--clang-tidy")
            options.tidy = value;
        else if (arg == "--runs")
            options.runs = number(value, 100);
        else if (arg == "--jobs")
            options.jobs = number(value, 1024);
        else if (arg == "--max-header-edits")
            options.max_header_edits = number(value, 1000);
        else if (arg == "--unity-retries")
            options.unity_retries = value == "0" ? 0 : number(value, 8);
        else if (arg == "--batch-size") {
            options.batch_size = number(value, 32);
            if (options.batch_size < 2) throw std::runtime_error("Unity batch size must be at least 2");
        }
        else if (arg == "--baseline-source")
            options.baseline_source = value;
        else if (arg == "--session")
            options.session = value;
        else if (arg == "--target")
            options.target = value;
        else if (arg == "--dependency")
            options.dependency = value;
        else if (arg == "--header")
            options.headers.push_back(value);
        else if (arg == "--exclude" || arg == "--header-path" || arg == "--unity-exclude" || arg == "--rebuild-file") {
            auto path = buildray::fs::path(value).lexically_normal();
            if (path.filename().empty()) path = path.parent_path();
            if (path.empty() || path.is_absolute() || path == "." || *path.begin() == "..")
                throw std::runtime_error(arg + " requires a relative file/directory inside the source tree");
            if (arg == "--exclude") options.exclusions.push_back(std::move(path));
            else if (arg == "--unity-exclude") options.unity_exclusions.push_back(std::move(path));
            else if (arg == "--rebuild-file") options.rebuild_files.push_back(std::move(path));
            else options.header_paths.push_back(std::move(path));
        }
        else if (arg == "--max-regression-percent")
            options.max_regression_percent = budget(value);
    }
    if (options.command == "compare" && (options.baseline_source.empty() || options.runs < 3))
        throw std::runtime_error("compare requires --baseline-source and at least three runs");
    if (options.command != "compare" && !options.baseline_source.empty())
        throw std::runtime_error("--baseline-source is supported only by compare");
    if (options.command == "pch" && !options.auto_pch && (options.target.empty() || options.headers.empty()))
        throw std::runtime_error("pch requires --target and at least one --header");
    if (options.auto_pch && (options.command != "pch" || !options.headers.empty() || options.runs < 3))
        throw std::runtime_error("--auto requires pch, at least three runs, and no --header; --target is optional");
    if (options.command != "pch" && !options.headers.empty())
        throw std::runtime_error("--header requires pch");
    if (options.command != "pch" && options.command != "prune-links" && options.command != "prune-deps" && options.command != "unity" && !options.target.empty())
        throw std::runtime_error("--target requires pch, unity or prune-links");
    if (options.command == "unity" && options.target.empty())
        throw std::runtime_error("unity requires --target");
    if (options.command != "prune-links" && options.command != "prune-deps" && !options.dependency.empty())
        throw std::runtime_error("--dependency requires prune-links");
    if ((options.command == "prune-links" || options.command == "prune-deps") && options.runs < 3)
        throw std::runtime_error("prune-links requires at least three runs");
    if (options.command != "deps" && options.fail_on_cycles)
        throw std::runtime_error("--fail-on-cycles requires deps");
    if ((!options.rebuild_files.empty() || options.incremental_only) && options.command != "pch" && options.command != "unity" && options.command != "prune-deps")
        throw std::runtime_error("Incremental measurements require pch or unity");
    if (options.incremental_only && options.rebuild_files.empty())
        throw std::runtime_error("--incremental-only requires at least one --rebuild-file");
    if (options.command != "deps" && options.command != "headers" && options.command != "unity" &&
        options.command != "prune-links" && options.command != "prune-deps" &&
        !(options.command == "pch" && !options.rebuild_files.empty()) && explicit_c_compiler)
        throw std::runtime_error("--c-compiler requires deps, headers, unity or incremental pch");
    if ((options.command == "headers" || options.command == "unity" || options.command == "pch" || options.command == "prune-links" || options.command == "prune-deps") && !explicit_c_compiler) {
        auto driver = buildray::fs::path(options.compiler);
        auto name = driver.filename().string();
        if (name.starts_with("clang++")) {
            name.replace(0, 7, "clang");
            options.c_compiler = (driver.parent_path() / name).string();
        }
    }
    if (options.command != "optimize" && options.command != "prune-links" && options.command != "prune-deps" && options.command != "headers" && !options.exclusions.empty())
        throw std::runtime_error("--exclude requires optimize, headers or prune-links");
    if (options.command != "headers" && (options.forward_declarations || !options.header_paths.empty() || options.max_header_edits != 8))
        throw std::runtime_error("Header options require command headers");
    // Tools run from the snapshot; resolve explicit relative executable paths now.
    for (auto* tool : {&options.compiler, &options.c_compiler, &options.tidy}) {
        // The driver name is semantic: clang++ can be a symlink to clang.
        // Resolving the link would lose C++ linking behavior (and wrapper names).
        if (tool->find('/') != std::string::npos)
            *tool = buildray::fs::absolute(*tool).lexically_normal().string();
    }
    return options;
}
}  // namespace

int main(int argc, char** argv) {
    buildray::install_signal_handlers();
    const std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty() || args.front() == "--help" || args.front() == "-h" ||
        (args.size() == 2 && args[1] == "--help")) {
        std::cout << help;
        return 0;
    }
    if (args.front() == "--version") {
        std::cout << "buildray " BUILDRAY_VERSION " (C++26)\n";
        return 0;
    }
    if (args.front() == "check") {
        buildray::CheckOptions options;
        try {
            options = parse_check(args);
        } catch (const std::exception& error) {
            std::cerr << "buildray: " << error.what() << '\n';
            return 2;
        }
        try {
            return buildray::check_reports(options);
        } catch (const std::exception& error) {
            std::cerr << "buildray: " << error.what() << '\n';
            return 1;
        }
    }
    if (args.front() == "_launch" || args.front() == "_trace-launch" || args.front() == "_deps-launch" || args.front() == "_link-launch") {
        try {
            if (args.front() == "_link-launch") return buildray::launch_link({args.begin() + 1, args.end()});
            return buildray::launch({args.begin() + 1, args.end()}, args.front() == "_trace-launch",
                                    args.front() == "_deps-launch");
        } catch (const std::exception& error) {
            std::cerr << "buildray launcher: " << error.what() << '\n';
            return 1;
        }
    }
    buildray::Options options;
    try {
        options = parse(args);
    } catch (const std::exception& error) {
        std::cerr << "buildray: " << error.what() << "\nUse buildray --help\n";
        return 2;
    }
    try {
        return buildray::pipeline(options, buildray::fs::read_symlink("/proc/self/exe"));
    } catch (const std::exception& error) {
        std::cerr << "buildray: " << error.what() << '\n';
        return 1;
    }
}
