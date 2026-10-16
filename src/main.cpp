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
  buildray measure  --source DIR [options] [-- -DNAME=VALUE ...]
  buildray profile --source DIR [options] [-- -DNAME=VALUE ...]
  buildray pch --source DIR --target NAME --header HEADER [--header HEADER ...]
               [options] [-- -DNAME=VALUE ...]
  buildray compare --baseline-source DIR --source DIR [options] [-- -DNAME=VALUE ...]
  buildray check --baseline REPORT --current REPORT [--output FILE]
                 [--max-regression-percent N]

Options:
  --source DIR          CMake source tree (default: current directory)
  --work-dir DIR        New output directory (default: SOURCE/.buildray/unique-id)
  --compiler PATH       Clang C++ compiler (default: clang++)
  --clang-tidy PATH     clang-tidy with misc-include-cleaner (default: clang-tidy)
  --runs N              Clean build samples per variant, 1..100 (default: 3)
  --jobs N              Fixed build parallelism, 1..1024 (default: 2)
  --allow-no-tests      Permit build-only validation when no CTest tests exist
  --baseline-source DIR  Base revision source for paired comparison
  --max-regression-percent N  Regression budget (default: 5)
  --session ID          Optional label shared by comparable measurements
  --target NAME         CMake target for a PCH experiment
  --header HEADER       PCH header: <system> or source-root-relative path; repeatable
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

Exit codes: 0 completed, 1 pipeline/validation failure, 2 invalid arguments,
3 regression over budget and noise, 4 incomparable reports, 130 interrupted.
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
    if (options.command != "optimize" && options.command != "measure" &&
        options.command != "compare" && options.command != "profile" && options.command != "pch")
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
        if (arg != "--source" && arg != "--work-dir" && arg != "--compiler" &&
            arg != "--clang-tidy" && arg != "--runs" && arg != "--jobs" &&
            arg != "--baseline-source" && arg != "--max-regression-percent" && arg != "--session" &&
            arg != "--target" && arg != "--header")
            throw std::runtime_error("Unknown option: " + arg);
        if (++i == args.size() || args[i].starts_with("--"))
            throw std::runtime_error("Missing value for " + arg);
        const auto& value = args[i];
        if (value.empty()) throw std::runtime_error("Empty value for " + arg);
        if (arg == "--source")
            options.source = value;
        else if (arg == "--work-dir")
            options.work = value;
        else if (arg == "--compiler")
            options.compiler = value;
        else if (arg == "--clang-tidy")
            options.tidy = value;
        else if (arg == "--runs")
            options.runs = number(value, 100);
        else if (arg == "--jobs")
            options.jobs = number(value, 1024);
        else if (arg == "--baseline-source")
            options.baseline_source = value;
        else if (arg == "--session")
            options.session = value;
        else if (arg == "--target")
            options.target = value;
        else if (arg == "--header")
            options.headers.push_back(value);
        else if (arg == "--max-regression-percent")
            options.max_regression_percent = budget(value);
    }
    if (options.command == "compare" && (options.baseline_source.empty() || options.runs < 3))
        throw std::runtime_error("compare requires --baseline-source and at least three runs");
    if (options.command != "compare" && !options.baseline_source.empty())
        throw std::runtime_error("--baseline-source is supported only by compare");
    if (options.command == "pch" && (options.target.empty() || options.headers.empty()))
        throw std::runtime_error("pch requires --target and at least one --header");
    if (options.command != "pch" && (!options.target.empty() || !options.headers.empty()))
        throw std::runtime_error("--target and --header are supported only by pch");
    // Tools run from the snapshot; resolve explicit relative executable paths now.
    for (auto* tool : {&options.compiler, &options.tidy}) {
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
    if (args.front() == "_launch" || args.front() == "_trace-launch") {
        try {
            return buildray::launch({args.begin() + 1, args.end()}, args.front() == "_trace-launch");
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
