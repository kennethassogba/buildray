#include <charconv>
#include <iostream>
#include <stdexcept>
#include <system_error>

#include "buildray/core.hpp"

static_assert(__cplusplus > 202302L, "Buildray requires C++26 mode");

namespace {
constexpr const char* help = R"(Buildray 0.1.0 - measured C++ include cleanup

Usage:
  buildray optimize --source DIR [options] [-- -DNAME=VALUE ...]
  buildray measure  --source DIR [options] [-- -DNAME=VALUE ...]

Options:
  --source DIR          CMake source tree (default: current directory)
  --work-dir DIR        New output directory (default: SOURCE/.buildray/unique-id)
  --compiler PATH       Clang C++ compiler (default: clang++)
  --clang-tidy PATH     clang-tidy with misc-include-cleaner (default: clang-tidy)
  --runs N              Clean build samples per variant, 1..100 (default: 3)
  --jobs N              Fixed build parallelism, 1..1024 (default: 2)
  --allow-no-tests      Permit build-only validation when no CTest tests exist
  --help                Show this help
  --version             Print version

optimize analyzes C++ source files, removes unused includes in an isolated copy,
validates the candidate, and compares repeated clean builds. The original tree
is never edited. Outputs: report.json, includes.patch, compiler records, logs.
Patches are validated only for the supplied build configuration. Headers are
not edited. Unity builds and PCH are disabled in both measured variants.

Exit codes: 0 completed (inspect performance_result), 1 pipeline/validation
failure, 2 invalid arguments, 130 interrupted. No patch is applied automatically.
)";

unsigned number(const std::string& value, unsigned maximum) {
    unsigned parsed = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (error != std::errc{} || end != value.data() + value.size() || parsed == 0 ||
        parsed > maximum)
        throw std::runtime_error("Invalid numeric option: " + value);
    return parsed;
}

buildray::Options parse(const std::vector<std::string>& args) {
    buildray::Options options;
    options.command = args.front();
    if (options.command != "optimize" && options.command != "measure")
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
            arg != "--clang-tidy" && arg != "--runs" && arg != "--jobs")
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
    }
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
        std::cout << "buildray 0.1.0 (C++26)\n";
        return 0;
    }
    if (args.front() == "_launch") {
        try {
            return buildray::launch({args.begin() + 1, args.end()});
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
