#include <llvm/Support/CommandLine.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

#include "buildray/core.hpp"

namespace buildray {
namespace {
struct Variant {
    fs::path source;
    fs::path build;
    fs::path records;
    bool trace = false;
    std::string pch_target;
    std::vector<std::string> pch_headers;
};

void checked(const std::vector<std::string>& args, const fs::path& cwd, const fs::path& log) {
    const auto result = run(args, cwd, log);
    if (result.code != 0) {
        throw std::runtime_error(args.front() + " exited " + std::to_string(result.code) +
                                 "; see " + log.string());
    }
}

void snapshot(const fs::path& source, const fs::path& destination, const fs::path& work) {
    fs::create_directories(destination);
    for (const auto& entry : fs::directory_iterator(source)) {
        const auto name = entry.path().filename().string();
        if (name == ".git" || name == ".buildray" || name == ".cache" ||
            absolute_path(entry.path()) == work)
            continue;
        if (entry.is_symlink()) {
            // Following a link could escape the snapshot or modify external files.
            throw std::runtime_error("Source symlinks are not supported yet: " +
                                     entry.path().string());
        }
        const auto target = destination / entry.path().filename();
        if (entry.is_directory()) {
            if (fs::exists(entry.path() / "CMakeCache.txt")) continue;
            snapshot(entry.path(), target, work);
        } else if (entry.is_regular_file()) {
            fs::copy_file(entry.path(), target);
        }
    }
}

void check_cmake_args(const std::vector<std::string>& args) {
    const std::vector<std::string> reserved{"CMAKE_CXX_COMPILER",
                                            "CMAKE_C_COMPILER",
                                            "CMAKE_EXPORT_COMPILE_COMMANDS",
                                            "CMAKE_UNITY_BUILD",
                                            "CMAKE_DISABLE_PRECOMPILE_HEADERS",
                                            "CMAKE_GENERATOR"};
    for (const auto& arg : args) {
        if (!arg.starts_with("-D") || arg.find('=') == std::string::npos)
            throw std::runtime_error("CMake arguments must use -DNAME=VALUE: " + arg);
        const auto key_end = arg.find_first_of(":=", 2);
        const auto key = arg.substr(2, key_end - 2);
        for (const auto& prefix : reserved) {
            if (key.starts_with(prefix))
                throw std::runtime_error("Buildray controls " + key + "; use --compiler for C++");
        }
        if (arg.find("LAUNCHER") != std::string::npos ||
            arg.find("RULE_LAUNCH") != std::string::npos)
            throw std::runtime_error("Custom launchers are unsupported in measured builds");
    }
}

std::string launcher(const fs::path& executable, const fs::path& records, bool trace) {
    if (executable.string().find(';') != std::string::npos ||
        records.string().find(';') != std::string::npos)
        throw std::runtime_error("Semicolons in launcher paths are unsupported by CMake lists");
    return executable.string() + (trace ? ";_trace-launch;" : ";_launch;") + records.string() + ";--";
}

void configure(const Options& options, const Variant& variant, const fs::path& executable,
               const fs::path& log) {
    fs::create_directories(variant.records);
    std::vector<std::string> args{"cmake",
                                  "-S",
                                  variant.source.string(),
                                  "-B",
                                  variant.build.string(),
                                  "-G",
                                  "Ninja",
                                  "-DCMAKE_BUILD_TYPE=Release",
                                  "-DBUILD_TESTING=ON"};
    args.insert(args.end(), options.cmake_args.begin(), options.cmake_args.end());
    const std::vector<std::string> controlled{
        "-DCMAKE_CXX_COMPILER=" + options.compiler, "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_UNITY_BUILD=OFF", "-DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON",
        "-DCMAKE_CXX_COMPILER_LAUNCHER=" + launcher(executable, variant.records, variant.trace)};
    args.insert(args.end(), controlled.begin(), controlled.end());
    checked(args, variant.source, log);
    if (!fs::exists(variant.build / "compile_commands.json"))
        throw std::runtime_error("Project did not generate compile_commands.json");
}

void clean(const Variant& variant, const fs::path& log) {
    checked({"cmake", "--build", variant.build.string(), "--target", "clean"}, variant.source, log);
}

ProcessResult build(const Options& options, const Variant& variant, const fs::path& log) {
    const auto result = run(
        {"cmake", "--build", variant.build.string(), "--parallel", std::to_string(options.jobs)},
        variant.source, log);
    if (result.code != 0)
        throw std::runtime_error("Build failed (exit " + std::to_string(result.code) + "); see " +
                                 log.string());
    return result;
}

std::size_t test(const Options& options, const Variant& variant, const fs::path& logs,
                 const std::string& label) {
    const auto inventory = logs / (label + "-tests.json");
    checked({"ctest", "--test-dir", variant.build.string(), "--show-only=json-v1"}, variant.source,
            inventory);
    const auto document = read_json(inventory);
    const auto* object = document.getAsObject();
    const auto* tests = object ? object->getArray("tests") : nullptr;
    if (!tests) throw std::runtime_error("Invalid CTest inventory: " + inventory.string());
    if (tests->empty() && !options.allow_no_tests)
        throw std::runtime_error(
            "No CTest tests registered. Enable project tests, or explicitly use "
            "--allow-no-tests (build-only validation).");
    if (tests->empty()) return 0;
    for (const auto& item : *tests) {
        const auto* definition = item.getAsObject();
        if (!definition) throw std::runtime_error("Invalid CTest test definition");
        if (const auto* properties = definition->getArray("properties")) {
            for (const auto& property : *properties) {
                const auto* value = property.getAsObject();
                if (value && value->getString("name") == "DISABLED" &&
                    value->getBoolean("value").value_or(false))
                    throw std::runtime_error("Disabled tests cannot count as validation");
            }
        }
    }
    const auto junit = logs / (label + "-tests.xml");
    checked({"ctest", "--test-dir", variant.build.string(), "--output-on-failure", "--timeout",
             "120", "--no-tests=error", "--output-junit", junit.string()},
            variant.source, logs / (label + "-tests.log"));
    // CTest returns success for runtime-skipped tests. Do not silently claim
    // complete validation when a required test did not execute.
    if (read_file(junit).find("<skipped") != std::string::npos)
        throw std::runtime_error("CTest skipped a test; see " + junit.string());
    return tests->size();
}

json::Array compiler_records(const fs::path& records) {
    json::Array result;
    for (const auto& entry : fs::directory_iterator(records)) {
        if (entry.path().extension() == ".json") result.push_back(read_json(entry.path()));
    }
    return result;
}

bool cpp_source(const fs::path& file) {
    const auto extension = file.extension();
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".C" ||
           extension == ".c++" || extension == ".cp";
}

void verify_coverage(const Variant& variant) {
    std::size_t pch_creations = 0;
    std::size_t pch_uses = 0;
    std::map<fs::path, std::size_t> expected;
    const auto database = read_json(variant.build / "compile_commands.json");
    const auto* commands = database.getAsArray();
    if (!commands) throw std::runtime_error("Invalid compilation database");
    for (const auto& command : *commands) {
        const auto* object = command.getAsObject();
        if (!object || !object->getString("file") || !object->getString("directory"))
            throw std::runtime_error("Invalid compilation database entry");
        fs::path file = object->getString("file")->str();
        if (!cpp_source(file)) continue;
        if (file.is_relative()) file = fs::path(object->getString("directory")->str()) / file;
        ++expected[absolute_path(file)];
    }
    std::map<fs::path, std::size_t> measured;
    for (const auto& record : compiler_records(variant.records)) {
        const auto* object = record.getAsObject();
        if (!object || !object->getString("source") || !object->getString("directory") ||
            object->getInteger("exit_code") != 0)
            throw std::runtime_error("Invalid compiler record");
        fs::path file = object->getString("source")->str();
        if (file.is_relative()) file = fs::path(object->getString("directory")->str()) / file;
        if (file.parent_path().filename() == "Unity" &&
            file.filename().string().starts_with("unity_"))
            throw std::runtime_error(
                "A target re-enabled unity builds; isolated compilation is required");
        if (const auto* command = object->getArray("command")) {
            bool creation = false;
            bool use = false;
            bool selected = false;
            for (const auto& argument : *command) {
                const auto text = argument.getAsString();
                if (text && !variant.pch_target.empty() &&
                    text->contains("CMakeFiles/" + variant.pch_target + ".dir/cmake_pch.hxx"))
                    selected = true;
                if (text && *text == "-emit-pch") creation = true;
                if (text && *text == "-include-pch") use = true;
                if (text && (text->contains("cmake_pch") || *text == "-include-pch" ||
                             *text == "-emit-pch") && variant.pch_target.empty())
                    throw std::runtime_error(
                        "A target re-enabled precompiled headers; disable PCH for validation");
            }
            if ((creation || use) && !selected)
                throw std::runtime_error("PCH enabled outside the selected target");
            if (creation) {
                ++pch_creations;
                auto wrapper = file;
                wrapper.replace_extension();  // cmake_pch.hxx.cxx -> cmake_pch.hxx
                std::set<std::string> actual;
                std::istringstream lines(read_file(wrapper));
                std::string line;
                while (std::getline(lines, line)) {
                    if (line.starts_with("#include ")) actual.insert(line.substr(9));
                }
                std::set<std::string> requested;
                for (const auto& header : variant.pch_headers)
                    requested.insert(header.starts_with('<') ? header :
                        "\"" + (variant.source / fs::path(header).lexically_normal()).string() + "\"");
                if (actual != requested)
                    throw std::runtime_error("Generated PCH includes differ from the requested headers; inherited PCHs are unsupported");
            }
            if (use) ++pch_uses;
        }
        if (!cpp_source(file)) continue;
        if (file.is_relative()) file = fs::path(object->getString("directory")->str()) / file;
        ++measured[absolute_path(file)];
    }
    if (expected.empty() || measured != expected)
        throw std::runtime_error(
            "Compiler coverage does not match compile_commands.json. "
            "Build all C++ targets by default (disable EXCLUDE_FROM_ALL targets), and do not "
            "override the compiler launcher. Partial validation is refused.");
    if (!variant.pch_target.empty() && (pch_creations == 0 || pch_uses == 0))
        throw std::runtime_error("Selected target did not create and use a C++ PCH");
}

void clear_records(const fs::path& records) {
    for (const auto& entry : fs::directory_iterator(records)) fs::remove(entry.path());
}

json::Object sample(const Options& options, const Variant& variant, const fs::path& logs,
                    const std::string& label) {
    clean(variant, logs / (label + "-clean.log"));
    clear_records(variant.records);
    const auto result = build(options, variant, logs / (label + "-build.log"));
    verify_coverage(variant);
    auto records = compiler_records(variant.records);
    if (records.empty())
        throw std::runtime_error("No compiler measurements captured; launcher overridden?");
    double compiler_wall = 0;
    double compiler_cpu = 0;
    std::int64_t max_rss = 0;
    for (const auto& value : records) {
        const auto* record = value.getAsObject();
        if (!record || record->getInteger("exit_code") != 0)
            throw std::runtime_error("Invalid compiler measurement");
        compiler_wall += record->getNumber("wall_seconds").value_or(0);
        compiler_cpu += record->getNumber("user_seconds").value_or(0) +
                        record->getNumber("system_seconds").value_or(0);
        max_rss = std::max(max_rss, record->getInteger("peak_rss_kib").value_or(0));
    }
    write_json(logs / (label + "-compilers.json"), std::move(records));
    return json::Object{{"wall_seconds", result.seconds},
                        {"compiler_wall_sum_seconds", compiler_wall},
                        {"compiler_cpu_seconds", compiler_cpu},
                        {"max_compiler_rss_kib", max_rss}};
}

json::Object summarize(const json::Array& samples) {
    std::vector<double> times;
    for (const auto& sample_value : samples)
        times.push_back(*sample_value.getAsObject()->getNumber("wall_seconds"));
    return json::Object{{"median_seconds", median(times)},
                        {"mad_seconds", mad(times)},
                        {"min_seconds", *std::ranges::min_element(times)},
                        {"max_seconds", *std::ranges::max_element(times)},
                        {"samples", json::Array(samples)}};
}

std::pair<json::Array, json::Array> paired_samples(const Options& options, const Variant& baseline,
                                                   const Variant& candidate, const fs::path& logs) {
    json::Array before;
    json::Array after;
    for (unsigned i = 0; i < options.runs; ++i) {
        std::cout << "Measuring pair " << i + 1 << '/' << options.runs
                  << (i % 2 ? " (candidate first)...\n" : " (baseline first)...\n") << std::flush;
        const auto suffix = std::to_string(i);
        if (i % 2 == 0) {
            before.push_back(sample(options, baseline, logs, "baseline-" + suffix));
            after.push_back(sample(options, candidate, logs, "candidate-" + suffix));
        } else {
            after.push_back(sample(options, candidate, logs, "candidate-" + suffix));
            before.push_back(sample(options, baseline, logs, "baseline-" + suffix));
        }
    }
    return {std::move(before), std::move(after)};
}

using Changes = std::map<fs::path, std::vector<Replacement>>;
using SourceFiles = std::map<fs::path, std::string>;

std::vector<std::string> compiler_arguments(const json::Object& entry) {
    std::vector<std::string> result;
    if (const auto* arguments = entry.getArray("arguments")) {
        for (const auto& argument : *arguments) {
            if (!argument.getAsString()) throw std::runtime_error("Invalid compiler argument");
            result.emplace_back(*argument.getAsString());
        }
    } else if (const auto command = entry.getString("command")) {
        llvm::BumpPtrAllocator allocator;
        llvm::StringSaver saver(allocator);
        llvm::SmallVector<const char*, 32> tokens;
        llvm::cl::TokenizeGNUCommandLine(*command, saver, tokens);
        for (const auto* argument : tokens) result.emplace_back(argument);
    }
    if (result.empty()) throw std::runtime_error("Missing compiler command");
    return result;
}

Changes analyze(const Options& options, const Variant& variant, const fs::path& work,
                const SourceFiles& originals, json::Object& report) {
    const auto database = read_json(variant.build / "compile_commands.json");
    const auto* commands = database.getAsArray();
    if (!commands || commands->empty()) throw std::runtime_error("Empty compilation database");
    std::map<fs::path, std::vector<json::Value>> groups;
    std::int64_t generated_commands = 0;
    for (const auto& entry : *commands) {
        const auto* object = entry.getAsObject();
        if (!object || !object->getString("file") || !object->getString("directory"))
            throw std::runtime_error("Invalid compilation database entry");
        const fs::path directory = std::string(*object->getString("directory"));
        fs::path file = std::string(*object->getString("file"));
        if (file.is_relative()) file = directory / file;
        file = absolute_path(file);
        if (!within(file, variant.source) || !fs::is_regular_file(file)) continue;
        if (!cpp_source(file)) continue;
        const auto original = originals.find(file.lexically_relative(variant.source));
        if (original == originals.end()) {
            ++generated_commands;
            continue;
        }
        if (read_file(file) != original->second)
            throw std::runtime_error("Configuration rewrote an input source: " + file.string());
        groups[file].push_back(entry);
    }
    if (groups.empty()) throw std::runtime_error("No project C++ translation units to analyze");
    Changes changes;
    json::Array findings;
    json::Array rejected;
    unsigned invocation = 0;
    unsigned preflight = 0;
    for (const auto& [file, entries] : groups) {
        std::optional<std::set<Replacement>> consensus;
        bool needs_insertions = false;
        for (const auto& entry : entries) {
            const auto analysis = work / "analysis" / std::to_string(invocation++);
            fs::create_directories(analysis);
            write_json(analysis / "compile_commands.json", json::Array{entry});
            const auto fixes = analysis / "fixes.yaml";
            // Use the user's suppression comments, but run only this milestone's
            // check. Never ask clang-tidy to edit any source file.
            const std::string config =
                R"({"Checks":"-*,misc-include-cleaner","WarningsAsErrors":"","CheckOptions":{"misc-include-cleaner.MissingIncludes":"true","misc-include-cleaner.UnusedIncludes":"true"}})";
            checked({options.tidy, "--config=" + config, "--quiet", "-p", analysis.string(),
                     "--export-fixes=" + fixes.string(), file.string()},
                    variant.source, analysis / "clang-tidy.log");
            std::set<Replacement> removals;
            const auto source = read_file(file);
            for (auto replacement : read_fixes(fixes)) {
                needs_insertions = needs_insertions || !replacement.text.empty();
                fs::path path = replacement.file;
                if (path.is_relative()) {
                    const auto* object = entry.getAsObject();
                    path = fs::path(std::string(*object->getString("directory"))) / path;
                }
                if (absolute_path(path) != file || !removable_include(source, replacement))
                    continue;
                replacement.file = file.string();
                removals.insert(std::move(replacement));
            }
            if (!consensus) {
                consensus = std::move(removals);
            } else {
                std::set<Replacement> common;
                std::set_intersection(consensus->begin(), consensus->end(), removals.begin(),
                                      removals.end(), std::inserter(common, common.end()));
                consensus = std::move(common);
            }
        }
        if (!consensus || consensus->empty()) continue;
        const auto relative = file.lexically_relative(variant.source);
        if (needs_insertions) {
            // An "unused directly" header may still supply a transitive declaration.
            // Include-cleaner expects new direct includes to accompany such removals.
            // We only delete: compile that proposal in every configuration first.
            const auto original = read_file(file);
            write_file(file, apply_removals(original, {consensus->begin(), consensus->end()}));
            bool valid = true;
            try {
                for (const auto& entry : entries) {
                    const auto* object = entry.getAsObject();
                    const auto log =
                        work / "analysis" / ("preflight-" + std::to_string(preflight++) + ".log");
                    const auto result = run(compiler_arguments(*object),
                                            std::string(*object->getString("directory")), log);
                    if (interrupted()) throw std::runtime_error("Interrupted during preflight");
                    if (result.code != 0) {
                        rejected.push_back(
                            json::Object{{"file", relative.generic_string()},
                                         {"reason", "deletion-only proposal failed compilation"},
                                         {"compiler_exit_code", result.code},
                                         {"log", log.lexically_relative(work).generic_string()}});
                        valid = false;
                        break;
                    }
                }
            } catch (...) {
                write_file(file, original);
                throw;
            }
            write_file(file, original);
            if (!valid) continue;
        }
        if (relative.string().find_first_of("\n\r\t") != std::string::npos)
            throw std::runtime_error("Patch paths may not contain control characters");
        const auto source = read_file(file);
        for (const auto& removal : *consensus) {
            findings.push_back(json::Object{
                {"file", relative.generic_string()},
                {"offset", static_cast<std::int64_t>(removal.offset)},
                {"length", static_cast<std::int64_t>(removal.length)},
                {"line",
                 static_cast<std::int64_t>(
                     1 + std::count(source.begin(),
                                    source.begin() + static_cast<std::ptrdiff_t>(removal.offset),
                                    '\n'))},
                {"directive", source.substr(removal.offset, removal.length)},
                {"compile_configurations", static_cast<std::int64_t>(entries.size())}});
        }
        changes[relative] = {consensus->begin(), consensus->end()};
    }
    report["translation_units_analyzed"] = static_cast<std::int64_t>(groups.size());
    report["compile_commands_analyzed"] = invocation;
    report["generated_compile_commands_skipped"] = generated_commands;
    report["preflight_rejected_files"] = std::move(rejected);
    report["preflight_compile_commands"] = preflight;
    report["removed_includes"] = static_cast<std::int64_t>(findings.size());
    report["findings"] = std::move(findings);
    return changes;
}

void patch(const Changes& changes, const Variant& baseline, const Variant& candidate,
           const fs::path& work) {
    std::string combined;
    for (const auto& [relative, replacements] : changes) {
        const auto original = read_file(candidate.source / relative);
        if (original != read_file(baseline.source / relative))
            throw std::runtime_error("Source changed during analysis: " + relative.string());
        write_file(candidate.source / relative, apply_removals(original, replacements));
        const auto diff_file = work / "file.diff";
        const auto result =
            run({"diff", "-u", "--label", "a/" + relative.generic_string(), "--label",
                 "b/" + relative.generic_string(), (baseline.source / relative).string(),
                 (candidate.source / relative).string()},
                work, diff_file);
        if (result.code != 1)
            throw std::runtime_error("Cannot generate patch: " + diff_file.string());
        combined += read_file(diff_file);
    }
    fs::remove(work / "file.diff");
    write_file(work / "includes.patch", combined);
}

void print_summary(const json::Object& report, const fs::path& work) {
    std::cout << std::fixed << std::setprecision(3);
    if (const auto* baseline = report.getObject("baseline")) {
        std::cout << "Baseline: " << *baseline->getNumber("median_seconds") << " s median (MAD "
                  << *baseline->getNumber("mad_seconds") << " s)\n";
    }
    if (const auto* candidate = report.getObject("candidate")) {
        std::cout << "Candidate: " << *candidate->getNumber("median_seconds") << " s median (MAD "
                  << *candidate->getNumber("mad_seconds") << " s)\n";
    }
    if (const auto delta = report.getNumber("wall_change_percent")) {
        std::cout << "Wall time change: " << std::showpos << *delta << std::noshowpos << "% ("
                  << report.getString("performance_result")->str() << ")\n";
    }
    if (const auto count = report.getInteger("removed_includes"))
        std::cout << "Removed includes: " << *count << '\n';
    std::cout << "Report: " << (work / "report.json") << '\n';
    if (fs::exists(work / "includes.patch"))
        std::cout << "Patch: " << (work / "includes.patch") << '\n';
    if (fs::exists(work / "pch.patch"))
        std::cout << "Patch: " << (work / "pch.patch") << '\n';
    if (fs::exists(work / "headers.json"))
        std::cout << "Header profile: " << (work / "headers.json") << '\n';
}
}  // namespace

int pipeline(const Options& options, const fs::path& executable) {
    check_cmake_args(options.cmake_args);
    const auto source = absolute_path(options.source);
    const auto base_source =
        options.command == "compare" ? absolute_path(options.baseline_source) : source;
    if (!fs::is_regular_file(source / "CMakeLists.txt"))
        throw std::runtime_error("--source must contain CMakeLists.txt: " + source.string());
    if (!fs::is_regular_file(base_source / "CMakeLists.txt"))
        throw std::runtime_error("--baseline-source must contain CMakeLists.txt");
    // Validate generated CMake inputs before creating an output directory or running project code.
    const auto pch_script = options.command == "pch" ? pch_cmake(options, source) : "";
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const auto work =
        options.work.empty()
            ? source / ".buildray" / (std::to_string(stamp) + "-" + std::to_string(::getpid()))
            : absolute_path(options.work);
    if (within(source, work) || within(base_source, work))
        throw std::runtime_error("Work directory must not contain the source directory");
    fs::create_directories(work.parent_path());
    if (!fs::create_directory(work))
        throw std::runtime_error("Work directory already exists: " + work.string());
    const auto logs = work / "logs";
    fs::create_directories(logs);
    json::Array extra;
    for (const auto& arg : options.cmake_args) extra.push_back(arg);
    json::Object report{
        {"schema_version", 1},
        {"buildray_version", BUILDRAY_VERSION},
        {"status", "running"},
        {"command", options.command},
        {"source", source.string()},
        {"work_dir", work.string()},
        {"jobs", options.jobs},
        {"runs", options.command == "profile" ? 1u : options.runs},
        {"cmake_args", std::move(extra)},
        {"measurement",
         options.command == "measure"
             ? "clean builds; warm filesystem cache; compiler caches disabled"
             : "clean builds; warm filesystem cache; compiler caches disabled; alternating order"},
        {"unity_build", false},
        {"precompiled_headers", false},
        {"patch_validated", false},
        {"validation_scope", "one configured CMake build; project C++ source files only"},
        {"tests_required", !options.allow_no_tests}};
    write_json(work / "report.json", json::Object(report));
    int pipeline_exit = 0;
    try {
        std::cout << "Workspace: " << work << '\n' << std::flush;
        checked({options.compiler, "--version"}, source, logs / "compiler-version.log");
        const auto compiler_version = read_file(logs / "compiler-version.log");
        if (compiler_version.find("clang") == std::string::npos)
            throw std::runtime_error(
                "This milestone requires Clang; pass --compiler clang++[-VERSION]");
        report["compiler_version"] = compiler_version;
        checked({"cmake", "--version"}, source, logs / "cmake-version.log");
        checked({"ninja", "--version"}, source, logs / "ninja-version.log");
        report["cmake_version"] = read_file(logs / "cmake-version.log");
        report["ninja_version"] = read_file(logs / "ninja-version.log");
        if (options.command == "optimize") {
            checked({options.tidy, "--version"}, source, logs / "tidy-version.log");
            report["clang_tidy_version"] = read_file(logs / "tidy-version.log");
            checked(
                {options.tidy, "--config={}", "--checks=-*,misc-include-cleaner", "--list-checks"},
                source, logs / "tidy-checks.log");
            if (read_file(logs / "tidy-checks.log").find("misc-include-cleaner") ==
                std::string::npos)
                throw std::runtime_error("clang-tidy does not provide misc-include-cleaner");
        }
        Variant baseline{work / "baseline-source", work / "baseline-build",
                         work / "baseline-records", options.command == "profile", {}, {}};
        Variant candidate{work / "candidate-source", work / "candidate-build",
                          work / "candidate-records", false,
                          options.command == "pch" ? options.target : "", options.headers};
        report["phase"] = "baseline_validation";
        std::cout << "Configuring baseline, warming build, running tests...\n" << std::flush;
        snapshot(base_source, baseline.source, work);
        // Configure scripts can write generated links or files into their source tree.
        // Prepare both inputs before either configure runs; optimize uses identical bytes.
        if (options.command == "optimize" || options.command == "pch")
            snapshot(baseline.source, candidate.source, work);
        if (options.command == "compare") snapshot(source, candidate.source, work);
        SourceFiles originals;
        if (options.command == "optimize") {
            for (const auto& entry : fs::recursive_directory_iterator(candidate.source)) {
                if (entry.is_regular_file() && cpp_source(entry.path()))
                    originals[entry.path().lexically_relative(candidate.source)] =
                        read_file(entry.path());
            }
        }
        configure(options, baseline, executable, logs / "baseline-configure.log");
        auto initial_context = measurement_context(options, baseline.build);
        if (options.command == "profile") {
            initial_context["method"] = "clang-time-trace-v1";
            initial_context["sampling"] = "single-instrumented-build";
            report["profiling_context"] = std::move(initial_context);
        } else {
            report["measurement_context"] = std::move(initial_context);
        }
        clear_records(baseline.records);
        build(options, baseline, logs / "baseline-warmup.log");
        verify_coverage(baseline);
        const auto baseline_tests = test(options, baseline, logs, "baseline");
        report["baseline_tests_passed"] = static_cast<std::int64_t>(baseline_tests);
        if (options.command == "profile") {
            report["phase"] = "profiling";
            auto profile = header_profile(baseline.records, baseline.source, baseline.build);
            std::cout << "Header parse time (inclusive; nested costs overlap):\n";
            const auto& headers = *profile.getArray("headers");
            for (std::size_t i = 0; i < std::min<std::size_t>(8, headers.size()); ++i) {
                const auto& header = *headers[i].getAsObject();
                std::cout << "  " << std::fixed << std::setprecision(3)
                          << *header.getNumber("inclusive_seconds") << " s  "
                          << *header.getInteger("compilations") << " compilations  "
                          << header.getString("header")->str() << '\n';
            }
            write_file(work / "headers.md", profile_markdown(profile));
            write_json(work / "headers.json", std::move(profile));
            report["measurement"] = "one Clang time-trace build; profiling overhead; not a benchmark";
            report["validation"] = baseline_tests ? "build_and_tests_passed" : "build_only";
        } else if (options.command == "compare") {
            report["baseline_source"] = base_source.string();
            report["phase"] = "candidate_validation";
            std::cout << "Configuring current revision, warming build, running tests...\n"
                      << std::flush;
            configure(options, candidate, executable, logs / "candidate-configure.log");
            auto current_context = measurement_context(options, candidate.build);
            if (json::Value(json::Object(current_context)) != *report.get("measurement_context"))
                throw std::runtime_error(
                    "Revisions have different measurement context or CMake flags");
            clear_records(candidate.records);
            build(options, candidate, logs / "candidate-warmup.log");
            verify_coverage(candidate);
            const auto current_tests = test(options, candidate, logs, "candidate");
            report["candidate_tests_passed"] = static_cast<std::int64_t>(current_tests);
            report["validation"] =
                baseline_tests && current_tests ? "build_and_tests_passed" : "build_only";
            report["phase"] = "measurement";
            const auto [before, after] = paired_samples(options, baseline, candidate, logs);
            auto base_report = json::Object(report);
            base_report["command"] = "measure";
            base_report["phase"] = "complete";
            base_report["status"] = "passed";
            base_report["source"] = base_source.string();
            base_report["baseline"] = summarize(before);
            auto current_report = json::Object(base_report);
            current_report["source"] = source.string();
            current_report["baseline_tests_passed"] = static_cast<std::int64_t>(current_tests);
            current_report["baseline"] = summarize(after);
            current_report["measurement_context"] = std::move(current_context);
            auto comparison =
                compare_reports(json::Object(base_report), json::Object(current_report),
                                options.max_regression_percent);
            write_json(work / "baseline-report.json", std::move(base_report));
            write_json(work / "current-report.json", std::move(current_report));
            write_file(work / "comparison.md", comparison_markdown(comparison.report));
            write_json(work / "comparison.json", json::Object(comparison.report));
            report["baseline"] = summarize(before);
            report["candidate"] = summarize(after);
            report["performance_result"] = comparison.report.getString("performance_result")->str();
            report["wall_change_percent"] = *comparison.report.getNumber("wall_change_percent");
            report["gate_passed"] = *comparison.report.getBoolean("gate_passed");
            report["comparison"] = std::move(comparison.report);
            pipeline_exit = comparison.exit_code;
        } else if (options.command == "measure") {
            json::Array samples;
            report["phase"] = "measurement";
            for (unsigned i = 0; i < options.runs; ++i) {
                std::cout << "Measuring " << i + 1 << '/' << options.runs << "...\n" << std::flush;
                samples.push_back(sample(options, baseline, logs, "baseline-" + std::to_string(i)));
            }
            report["baseline"] = summarize(samples);
        } else {
            report["phase"] = "analysis";
            Changes changes;
            if (options.command == "pch") {
                const auto path = candidate.source / "CMakeLists.txt";
                if (read_file(path) != read_file(baseline.source / "CMakeLists.txt"))
                    throw std::runtime_error("CMakeLists.txt changed during baseline configuration");
                write_file(path, read_file(path) + pch_script);
                const auto result = run({"diff", "-u", "--label", "a/CMakeLists.txt", "--label",
                                         "b/CMakeLists.txt", (baseline.source / "CMakeLists.txt").string(),
                                         path.string()}, work, work / "pch.patch");
                if (result.code != 1) throw std::runtime_error("Cannot generate PCH patch");
                report["pch_target"] = options.target;
                json::Array headers;
                for (const auto& header : options.headers) headers.push_back(header);
                report["pch_headers"] = std::move(headers);
                report["candidate_precompiled_headers"] = true;
                report["validation_scope"] = "one configured CMake build; private C++ PCH on one target";
            }
            configure(options, candidate, executable, logs / "candidate-configure.log");
            if (options.command == "pch") {
                auto context = measurement_context(options, candidate.build);
                if (json::Value(json::Object(context)) != *report.get("measurement_context"))
                    throw std::runtime_error("PCH experiment changed measurement context or CMake flags");
                context["precompiled_headers"] = true;
                report["candidate_measurement_context"] = std::move(context);
            }
            if (options.command == "optimize") {
                // Generated headers must exist before clang-tidy parses sources.
                clear_records(candidate.records);
                build(options, candidate, logs / "candidate-initial-build.log");
                verify_coverage(candidate);
                std::cout << "Analyzing unused includes with clang-tidy...\n" << std::flush;
                changes = analyze(options, candidate, work, originals, report);
                patch(changes, baseline, candidate, work);
            }
            write_json(work / "report.json", json::Object(report));
            report["phase"] = "candidate_validation";
            std::cout << "Validating candidate with a clean build and tests...\n" << std::flush;
            clean(candidate, logs / "candidate-validation-clean.log");
            clear_records(candidate.records);
            build(options, candidate, logs / "candidate-validation-build.log");
            verify_coverage(candidate);
            const auto candidate_tests = test(options, candidate, logs, "candidate");
            report["candidate_tests_passed"] = static_cast<std::int64_t>(candidate_tests);
            if (baseline_tests != candidate_tests)
                throw std::runtime_error("CTest inventory changed");
            report["validation"] = baseline_tests ? "build_and_tests_passed" : "build_only";
            report["patch_validated"] = true;
            report["phase"] = "measurement";
            const auto [before, after] = paired_samples(options, baseline, candidate, logs);
            auto before_summary = summarize(before);
            auto after_summary = summarize(after);
            const double before_median = *before_summary.getNumber("median_seconds");
            const double after_median = *after_summary.getNumber("median_seconds");
            const double delta = 100.0 * (after_median - before_median) / before_median;
            const double threshold = std::max(
                0.02 * before_median, 3 * std::max(*before_summary.getNumber("mad_seconds"),
                                                   *after_summary.getNumber("mad_seconds")));
            const bool changed = options.command == "pch" || !changes.empty();
            const std::string verdict = !changed    ? "no_changes"
                                        : options.runs < 3 ? "insufficient_samples"
                                        : std::abs(after_median - before_median) <= threshold
                                            ? "inconclusive"
                                        : after_median < before_median ? "improved"
                                                                       : "regressed";
            report["baseline"] = std::move(before_summary);
            report["candidate"] = std::move(after_summary);
            report["wall_change_percent"] = delta;
            report["performance_result"] = verdict;
            report["noise_threshold_seconds"] = threshold;
            report["performance_rule"] =
                "heuristic: difference exceeds max(2% baseline, 3x max MAD); not a significance "
                "test";
            report["patch_recommended"] = changed && verdict == "improved";
        }
        if (interrupted()) throw std::runtime_error("Interrupted");
        report["phase"] = "complete";
        report["status"] = "passed";
        write_json(work / "report.json", json::Object(report));
        print_summary(report, work);
        return pipeline_exit;
    } catch (const std::exception& error) {
        report["status"] = interrupted() ? "interrupted" : "failed";
        report["error"] = error.what();
        report["patch_recommended"] = false;
        write_json(work / "report.json", json::Object(report));
        std::cerr << "buildray: " << error.what() << "\nReport: " << (work / "report.json") << '\n';
        return interrupted() ? 130 : 1;
    }
}
}  // namespace buildray
