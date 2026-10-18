#include <llvm/Support/CommandLine.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>
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
    bool dependencies = false;
    bool cmake_model = false;
    bool active_coverage = false;
    std::string unity_target{};
    bool links = false;
};

bool link_experiment(const Options& options) {
    return options.command == "prune-links" || options.command == "prune-deps";
}

fs::path link_records(const Variant& variant) { return variant.records.string() + "-links"; }

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
                                            "CMAKE_GENERATOR", "BUILDRAY_UNITY"};
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

std::string launcher(const fs::path& executable, const fs::path& records, bool trace, bool deps) {
    if (executable.string().find(';') != std::string::npos ||
        records.string().find(';') != std::string::npos)
        throw std::runtime_error("Semicolons in launcher paths are unsupported by CMake lists");
    return executable.string() + (deps ? ";_deps-launch;" : trace ? ";_trace-launch;" : ";_launch;") + records.string() + ";--";
}

void configure(const Options& options, const Variant& variant, const fs::path& executable,
               const fs::path& log) {
    fs::create_directories(variant.records);
    if (variant.links) fs::create_directories(link_records(variant));
    if (variant.dependencies || variant.cmake_model)
        write_file(variant.build / ".cmake/api/v1/query/codemodel-v2", "");
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
        "-DCMAKE_CXX_COMPILER_LAUNCHER=" + launcher(executable, variant.records, variant.trace, variant.dependencies)};
    args.insert(args.end(), controlled.begin(), controlled.end());
    if (variant.dependencies || link_experiment(options) || options.command == "headers" || options.command == "unity" ||
        (options.command == "pch" && !options.rebuild_files.empty()))
        args.push_back("-DCMAKE_C_COMPILER=" + options.c_compiler);
    if (options.command == "unity")
        args.push_back("-DBUILDRAY_UNITY=" + std::string(variant.unity_target.empty() ? "OFF" : "ON"));
    if (variant.dependencies || variant.active_coverage) {
        args.push_back("-DCMAKE_C_COMPILER_LAUNCHER=" + launcher(executable, variant.records, false, variant.dependencies));
    }
    if (variant.links) {
        const auto command = executable.string() + ";_link-launch;" + link_records(variant).string() + ";--";
        args.push_back("-DCMAKE_CXX_LINKER_LAUNCHER=" + command);
        args.push_back("-DCMAKE_C_LINKER_LAUNCHER=" + command);
    }
    checked(args, variant.source, log);
    if (!fs::exists(variant.build / "compile_commands.json"))
        throw std::runtime_error("Project did not generate compile_commands.json");
    if (variant.dependencies || variant.active_coverage)
        checked({"ninja", "-C", variant.build.string(), "-t", "compdb-targets", "all"},
                variant.source, variant.build / "active-commands.json");
}

void clean(const Variant& variant, const fs::path& log) {
    checked({"cmake", "--build", variant.build.string(), "--target", "clean"}, variant.source, log);
}

ProcessResult build(const Options& options, const Variant& variant, const fs::path& log,
                    bool explain = false) {
    std::vector<std::string> args{"cmake", "--build", variant.build.string(), "--parallel",
                                  std::to_string(options.jobs)};
    if (explain) args.insert(args.end(), {"--", "-d", "explain"});
    const auto result = run(args, variant.source, log);
    if (result.code != 0)
        throw std::runtime_error("Build failed (exit " + std::to_string(result.code) + "); see " +
                                 log.string());
    return result;
}

json::Array skipped_tests(const fs::path& junit, std::size_t expected) {
    // Read CTest's generated JUnit records, not test output (which CTest XML-escapes).
    // Retain encoded names verbatim: ambiguous or unstable names must fail closed.
    const auto xml = read_file(junit);
    static const std::regex header(R"xml(<testcase name="([^"]*)"[^>]*>)xml");
    std::vector<std::string> names;
    std::size_t count = 0;
    for (auto it = std::sregex_iterator(xml.begin(), xml.end(), header);
         it != std::sregex_iterator(); ++it) {
        ++count;
        const auto start = static_cast<std::size_t>(it->position() + it->length());
        const auto end = xml.find("</testcase>", start);
        const auto nested = xml.find("<testcase ", start);
        if (end == std::string::npos || (nested != std::string::npos && nested < end))
            throw std::runtime_error("Invalid CTest JUnit record: " + junit.string());
        const auto body = std::string_view(xml).substr(start, end - start);
        const bool skipped = body.find("<skipped") != std::string_view::npos;
        if (body.find("<failure") != std::string_view::npos ||
            body.find("<error") != std::string_view::npos ||
            (!skipped && it->str().find("status=\"notrun\"") != std::string::npos))
            throw std::runtime_error("CTest did not pass a test; see " + junit.string());
        if (skipped) names.push_back((*it)[1].str());
    }
    if (count != expected)
        throw std::runtime_error("CTest JUnit inventory does not match registered tests");
    std::ranges::sort(names);
    json::Array result;
    for (const auto& name : names) result.push_back(name);
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
    if (tests->empty()) {
        if (options.allow_baseline_skips) {
            write_json(logs / (label + "-skipped-tests.json"), json::Array{});
            if (label != "baseline" &&
                read_json(logs / "baseline-skipped-tests.json") != json::Value(json::Array{}))
                throw std::runtime_error("CTest skipped-test inventory changed");
        }
        return 0;
    }
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
    std::cout << "Running " << tests->size() << " CTest tests (" << label << ")...\n" << std::flush;
    const auto junit = logs / (label + "-tests.xml");
    checked({"ctest", "--test-dir", variant.build.string(), "--output-on-failure", "--timeout",
             "120", "--no-tests=error", "--output-junit", junit.string()},
            variant.source, logs / (label + "-tests.log"));
    if (options.allow_baseline_skips) {
        auto skipped = skipped_tests(junit, tests->size());
        write_json(logs / (label + "-skipped-tests.json"), json::Array(skipped));
        if (skipped.size() == tests->size())
            throw std::runtime_error("No CTest tests passed; all tests were skipped");
        if (label != "baseline" &&
            read_json(logs / "baseline-skipped-tests.json") != json::Value(json::Array(skipped)))
            throw std::runtime_error("CTest skipped-test inventory changed; see " + junit.string());
        std::cout << tests->size() - skipped.size() << " passed, " << skipped.size()
                  << " baseline skips (" << label << ").\n" << std::flush;
        return tests->size() - skipped.size();
    }
    // Skips remain an error unless the caller explicitly opts into a stable baseline.
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

std::multiset<std::string> test_names(const fs::path& inventory, unsigned& normalized) {
    const auto document = read_json(inventory);
    const auto* object = document.getAsObject();
    const auto* tests = object ? object->getArray("tests") : nullptr;
    if (!tests) throw std::runtime_error("Invalid CTest inventory");
    std::multiset<std::string> names;
    static const std::regex opaque(R"(([0-9]+)-byte object <[0-9A-Fa-f .-]+>)");
    for (const auto& entry : *tests) {
        const auto* definition = entry.getAsObject();
        const auto name = definition ? definition->getString("name") : std::nullopt;
        if (!name) throw std::runtime_error("Missing CTest name");
        auto identity = name->str();
        std::string filter;
        if (const auto* command = definition->getArray("command"))
            for (const auto& value : *command)
                if (const auto argument = value.getAsString();
                    argument && argument->starts_with("--gtest_filter="))
                    filter = argument->str();
        const auto slash = identity.rfind('/');
        if (!filter.empty() && slash != std::string::npos &&
            std::regex_match(identity.substr(slash + 1), opaque)) {
            // GTest's opaque value printer can dump ASLR pointers. Preserve the display
            // prefix and object size, and distinguish cases by their real execution filter.
            identity = std::regex_replace(identity, opaque, "$1-byte object <opaque>");
            identity += " [" + filter + "]";
            ++normalized;
        }
        names.insert(std::move(identity));
    }
    return names;
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
    std::set<fs::path> active;
    if ((variant.dependencies || variant.active_coverage)) {
        const auto document = read_json(variant.build / "active-commands.json");
        const auto* commands = document.getAsArray();
        if (!commands) throw std::runtime_error("Invalid Ninja default-target compilation database");
        for (const auto& item : *commands) {
            const auto* object = item.getAsObject();
            if (!object || !object->getString("output") || !object->getString("directory"))
                throw std::runtime_error("Invalid Ninja command");
            fs::path output = object->getString("output")->str();
            if (output.is_relative()) output = fs::path(object->getString("directory")->str()) / output;
            active.insert(absolute_path(output));
        }
    }
    const auto database = read_json(variant.build / "compile_commands.json");
    const auto* commands = database.getAsArray();
    if (!commands) throw std::runtime_error("Invalid compilation database");
    for (const auto& command : *commands) {
        const auto* object = command.getAsObject();
        if (!object || !object->getString("file") || !object->getString("directory"))
            throw std::runtime_error("Invalid compilation database entry");
        fs::path file = object->getString("file")->str();
        if (!cpp_source(file) && !((variant.dependencies || variant.active_coverage) && file.extension() == ".c")) continue;
        if ((variant.dependencies || variant.active_coverage)) {
            if (!object->getString("output")) throw std::runtime_error("Missing compilation output");
            fs::path output = object->getString("output")->str();
            if (output.is_relative()) output = fs::path(object->getString("directory")->str()) / output;
            if (!active.contains(absolute_path(output))) continue;
        }
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
            file.filename().string().starts_with("unity_")) {
            if (variant.unity_target.empty() || !within(absolute_path(file), variant.build) ||
                file.parent_path().parent_path().filename() != variant.unity_target + ".dir")
                throw std::runtime_error(
                    "A target re-enabled unity builds outside the selected experiment");
        }
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
        if (!cpp_source(file) && !((variant.dependencies || variant.active_coverage) && file.extension() == ".c")) continue;
        if (file.is_relative()) file = fs::path(object->getString("directory")->str()) / file;
        ++measured[absolute_path(file)];
    }
    if (expected.empty() || measured != expected)
        throw std::runtime_error(
            (variant.dependencies || variant.active_coverage) ?
            "Compiler coverage does not match Ninja's default all target. "
            "Every C/C++ compilation in that target must use Buildray's launcher; "
            "do not override the C or C++ compiler launcher." :
            "Compiler coverage does not match compile_commands.json. "
            "Build all C++ targets by default (disable EXCLUDE_FROM_ALL targets), and do not "
            "override the compiler launcher. Partial validation is refused.");
    if (!variant.pch_target.empty() && (pch_creations == 0 || pch_uses == 0))
        throw std::runtime_error("Selected target did not create and use a C++ PCH");
}

void clear_records(const fs::path& records) {
    for (const auto& entry : fs::directory_iterator(records)) fs::remove(entry.path());
}

void clear_variant(const Variant& variant) {
    clear_records(variant.records);
    if (variant.links) clear_records(link_records(variant));
}

void add_links(json::Object& summary, const Variant& variant, const fs::path& prefix, bool full) {
    if (!variant.links) return;
    const auto records = compiler_records(link_records(variant));
    if (full) verify_link_coverage(cmake_dependencies(variant.source, variant.build), variant.build, records);
    for (auto& [key, value] : link_sample(records, variant.source, variant.build))
        summary[key] = std::move(value);
    write_json(prefix.string() + "-linkers.json", json::Array(records));
}

json::Object compiler_sample(const ProcessResult& result, const json::Array& records, bool allow_empty = false) {
    if (records.empty() && !allow_empty)
        throw std::runtime_error("No compiler measurements captured; launcher overridden?");
    double compiler_wall = 0;
    double compiler_cpu = 0;
    std::int64_t max_rss = 0;
    std::int64_t pch_creations = 0;
    for (const auto& value : records) {
        const auto* record = value.getAsObject();
        if (!record || record->getInteger("exit_code") != 0)
            throw std::runtime_error("Invalid compiler measurement");
        compiler_wall += record->getNumber("wall_seconds").value_or(0);
        compiler_cpu += record->getNumber("user_seconds").value_or(0) +
                        record->getNumber("system_seconds").value_or(0);
        max_rss = std::max(max_rss, record->getInteger("peak_rss_kib").value_or(0));
        if (const auto* command = record->getArray("command"))
            for (const auto& arg : *command)
                if (arg.getAsString() == "-emit-pch") ++pch_creations;
    }
    return json::Object{{"wall_seconds", result.seconds},
                        {"compiler_wall_sum_seconds", compiler_wall},
                        {"compiler_cpu_seconds", compiler_cpu},
                        {"compiler_invocations", static_cast<std::int64_t>(records.size())},
                        {"pch_creations", pch_creations},
                        {"max_compiler_rss_kib", max_rss}};
}

json::Object sample(const Options& options, const Variant& variant, const fs::path& logs,
                    const std::string& label) {
    clean(variant, logs / (label + "-clean.log"));
    clear_variant(variant);
    const auto result = build(options, variant, logs / (label + "-build.log"));
    verify_coverage(variant);
    auto records = compiler_records(variant.records);
    auto summary = compiler_sample(result, records);
    add_links(summary, variant, logs / label, true);
    write_json(logs / (label + "-compilers.json"), std::move(records));
    return summary;
}

json::Object summarize(const json::Array& samples) {
    std::vector<double> times;
    for (const auto& sample_value : samples)
        times.push_back(*sample_value.getAsObject()->getNumber("wall_seconds"));
    json::Object result{{"median_seconds", median(times)},
                        {"mad_seconds", mad(times)},
                        {"min_seconds", *std::ranges::min_element(times)},
                        {"max_seconds", *std::ranges::max_element(times)},
                        {"samples", json::Array(samples)}};
    std::vector<double> link_times, link_counts;
    for (const auto& sample_value : samples) {
        const auto& row = *sample_value.getAsObject();
        if (const auto value = row.getNumber("linker_driver_wall_sum_seconds")) {
            link_times.push_back(*value);
            link_counts.push_back(static_cast<double>(*row.getInteger("linker_invocations")));
        }
    }
    if (link_times.size() == samples.size()) {
        result["linker_driver_wall_sum_median_seconds"] = median(link_times);
        result["linker_invocations_median"] = median(link_counts);
    }
    return result;
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
        if (options.command == "unity")
            verify_unity_plan(baseline.source, baseline.build, candidate.source,
                              candidate.build, options);
    }
    return {std::move(before), std::move(after)};
}

std::map<std::string, unsigned> command_inventory(const json::Array& records) {
    std::map<std::string, unsigned> result;
    for (const auto& value : records) {
        const auto* record = value.getAsObject();
        const auto* command = record ? record->getArray("command") : nullptr;
        if (!command || record->getInteger("exit_code") != 0)
            throw std::runtime_error("Invalid incremental compiler record");
        std::string identity;
        for (const auto& arg : *command) {
            const auto text = arg.getAsString();
            if (!text) throw std::runtime_error("Invalid compiler argument");
            identity += std::to_string(text->size()) + ":" + text->str();
        }
        ++result[identity];
    }
    return result;
}

void incremental_samples(const Options& options, const Variant& baseline, const Variant& candidate,
                         const fs::path& work, json::Object& report) {
    const auto logs = work / "logs";
    const auto before_commands = command_inventory(compiler_records(baseline.records));
    const auto after_commands = command_inventory(compiler_records(candidate.records));
    if (before_commands.empty() || after_commands.empty())
        throw std::runtime_error("Incremental measurements require verified full builds");
    report["phase"] = "incremental_measurement";
    report["incremental"] = json::Object{
        {"status", "running"}, {"runs", options.runs},
        {"method", "single-file comment edit; existing build trees; alternating pairs; warm filesystem cache; compiler caches disabled"},
        {"timed_scope", "default build including compilation and linking; editing, no-op checks, restoration and CTest excluded"},
        {"memory_scope", "maximum RSS of an individual compiler invocation, not total build memory"},
        {"limitation", "dependency invalidation after a comment-only edit; not representative of arbitrary semantic changes"},
        {"scenarios", json::Array{}}};
    const auto persist = [&] {
        write_json(work / "incremental.json", json::Object(*report.getObject("incremental")));
        write_json(work / "report.json", json::Object(report));
    };
    persist();
    bool regressed = false;
    for (std::size_t index = 0; index < options.rebuild_files.size(); ++index) {
        if (interrupted()) throw std::runtime_error("Interrupted");
        const auto& relative = options.rebuild_files[index];
        const auto original = read_file(baseline.source / relative);
        if (original != read_file(candidate.source / relative) || original != read_file(options.source / relative))
            throw std::runtime_error("Incremental input differs between variants: " + relative.string());
        const auto label = "incremental-" + std::to_string(index);
        auto* scenarios = report.getObject("incremental")->getArray("scenarios");
        scenarios->push_back(json::Object{{"file", relative.generic_string()},
            {"status", "running"}, {"baseline_samples", json::Array{}},
            {"candidate_samples", json::Array{}}});
        auto& scenario = *scenarios->back().getAsObject();
        persist();
        const auto restore = [&] {
            write_file(baseline.source / relative, original);
            write_file(candidate.source / relative, original);
        };
        try {
            const auto collect = [&](const Variant& variant, bool before, unsigned pair) {
                const auto name = label + (before ? "-baseline-" : "-candidate-") + std::to_string(pair);
                ProcessResult noop;
                unsigned settling_builds = 0;
                for (;; ++settling_builds) {
                    clear_variant(variant);
                    const auto log = logs / (name + "-settle-" + std::to_string(settling_builds) + ".log");
                    noop = build(options, variant, log, true);
                    const auto pending = compiler_records(variant.records);
                    write_json(logs / (name + "-settle-" + std::to_string(settling_builds) + "-compilers.json"), json::Array(pending));
                    fs::copy_file(log, logs / (name + "-noop.log"), fs::copy_options::overwrite_existing);
                    if (pending.empty() && read_file(log).find("ninja: no work to do.") != std::string::npos) break;
                    if (settling_builds == 2) {
                        fs::copy_file(log, logs / (name + "-dirty-explanation.log"), fs::copy_options::overwrite_existing);
                        throw std::runtime_error("Incremental build was not up to date before editing after two settling builds; see " + name + "-dirty-explanation.log");
                    }
                }
                const auto file = variant.source / relative;
                // Match the bytes of each probe in both variants. Each pair changes the
                // previous probe, without a second expensive restoration build per sample.
                const auto probe = original + "\n// Buildray incremental probe " + std::to_string(pair) + "\n";
                write_file(file, probe);
                clear_variant(variant);
                const auto result = build(options, variant, logs / (name + "-build.log"));
                auto records = compiler_records(variant.records);
                write_json(logs / (name + "-compilers.json"), json::Array(records));
                const auto measured = command_inventory(records);
                const auto& expected = before ? before_commands : after_commands;
                for (const auto& [command, count] : measured) {
                    const auto found = expected.find(command);
                    if (found == expected.end() || count > found->second)
                        throw std::runtime_error("Incremental build changed the verified compiler commands");
                }
                const auto links = variant.links ? compiler_records(link_records(variant)) : json::Array{};
                if (before && options.command == "prune-deps" && records.empty() && links.empty())
                    throw std::runtime_error("Baseline incremental probe triggered no observed compilation or link");
                auto summary = compiler_sample(result, records, options.command == "prune-deps");
                add_links(summary, variant, logs / name, false);
                json::Array sources;
                std::set<std::string> original_sources;
                for (const auto& record : records) {
                    const auto* object = record.getAsObject();
                    fs::path path = object->getString("source")->str();
                    if (path.is_relative()) path = fs::path(object->getString("directory")->str()) / path;
                    path = absolute_path(path);
                    const auto display = within(path, variant.source) ? "<SOURCE>/" + path.lexically_relative(variant.source).generic_string() :
                        within(path, variant.build) ? "<BUILD>/" + path.lexically_relative(variant.build).generic_string() : path.generic_string();
                    sources.push_back(display);
                    if (within(path, variant.source) &&
                        fs::is_regular_file(options.source / path.lexically_relative(variant.source)))
                        original_sources.insert(display);
                    if (!before && !variant.unity_target.empty()) {
                        for (const auto& item : *report.getObject("unity_plan")->getArray("groups")) {
                            const auto& group = *item.getAsObject();
                            if (group.getString("file") != display) continue;
                            for (const auto& member : *group.getArray("sources")) {
                                const auto member_path = member.getAsString()->str();
                                if (member_path.starts_with("<SOURCE>/") &&
                                    fs::is_regular_file(options.source / member_path.substr(9)))
                                    original_sources.insert(member_path);
                            }
                        }
                    }
                }
                summary["compiled_sources"] = std::move(sources);
                json::Array originals;
                for (const auto& original_path : original_sources) originals.push_back(original_path);
                summary["original_source_files"] = std::move(originals);
                summary["original_source_count"] = static_cast<std::int64_t>(original_sources.size());
                if (read_file(file) != probe)
                    throw std::runtime_error("Build rewrote the incremental probe");
                const auto count = test(options, variant, logs, name);
                unsigned opaque = 0, reference_opaque = 0;
                if (test_names(logs / (name + "-tests.json"), opaque) !=
                    test_names(logs / "baseline-tests.json", reference_opaque))
                    throw std::runtime_error("Incremental CTest inventory changed");
                summary["tests_passed"] = static_cast<std::int64_t>(count);
                if (options.allow_baseline_skips)
                    summary["skipped_tests"] = read_json(logs / (name + "-skipped-tests.json"));
                summary["noop_seconds"] = noop.seconds;
                summary["settling_builds"] = settling_builds;
                summary["log_prefix"] = "logs/" + name;
                scenario.getArray(before ? "baseline_samples" : "candidate_samples")->push_back(std::move(summary));
                persist();
            };
            for (unsigned pair = 0; pair < options.runs; ++pair) {
                std::cout << "Incremental " << relative << ": pair " << pair + 1 << '/' << options.runs
                          << (pair % 2 ? " (candidate first)...\n" : " (baseline first)...\n") << std::flush;
                if (pair % 2) { collect(candidate, false, pair); collect(baseline, true, pair); }
                else { collect(baseline, true, pair); collect(candidate, false, pair); }
            }
            restore();
            // Restore the exact input bytes and rebuild outside all sample timers.
            // PCH validation embeds header metadata, so rewinding mtimes is unsafe.
            for (const auto* variant : {&baseline, &candidate}) {
                clear_variant(*variant);
                build(options, *variant, logs / (label + (variant == &baseline ? "-baseline-restore.log" : "-candidate-restore.log")));
            }
        } catch (...) {
            restore();
            throw;
        }
        auto before = summarize(*scenario.getArray("baseline_samples"));
        auto after = summarize(*scenario.getArray("candidate_samples"));
        const auto b = *before.getNumber("median_seconds"), a = *after.getNumber("median_seconds");
        const auto threshold = std::max(0.02 * b, 3 * std::max(*before.getNumber("mad_seconds"), *after.getNumber("mad_seconds")));
        const std::string verdict = options.runs < 3 ? "insufficient_samples" :
            std::abs(a - b) <= threshold ? "inconclusive" : a < b ? "improved" : "regressed";
        scenario["baseline"] = std::move(before);
        scenario["candidate"] = std::move(after);
        scenario["wall_change_percent"] = 100 * (a - b) / b;
        scenario["noise_threshold_seconds"] = threshold;
        scenario["performance_result"] = verdict;
        scenario["status"] = "passed";
        scenario["input_restored"] = true;
        regressed = regressed || verdict == "regressed";
        std::cout << "  " << b << " -> " << a << " s (" << verdict << ")\n" << std::flush;
        persist();
    }
    auto* incremental = report.getObject("incremental");
    (*incremental)["status"] = "passed";
    (*incremental)["regression_detected"] = regressed;
    (*incremental)["performance_rule"] = "heuristic: difference exceeds max(2% baseline, 3x max MAD); at least three pairs; not a significance test";
    if (regressed) report["patch_recommended"] = false;
    std::ostringstream markdown;
    markdown << "# Incremental rebuild measurements\n\nSingle-file comment edits; compilation and linking timed. Tests, no-op checks and restoration are outside timing.\n\n"
             << "| File | Before (s) | After (s) | Change | Result |\n| --- | ---: | ---: | ---: | --- |\n";
    for (const auto& value : *incremental->getArray("scenarios")) {
        const auto& scenario = *value.getAsObject();
        auto file = scenario.getString("file")->str();
        for (std::size_t i = 0; i < file.size(); ++i)
            if (file[i] == '|' || file[i] == '`' || file[i] == '\n' || file[i] == '\r') file[i] = ' ';
        markdown << "| " << file << " | " << std::fixed << std::setprecision(3)
                 << *scenario.getObject("baseline")->getNumber("median_seconds") << " | "
                 << *scenario.getObject("candidate")->getNumber("median_seconds") << " | "
                 << std::showpos << *scenario.getNumber("wall_change_percent") << std::noshowpos << "% | "
                 << scenario.getString("performance_result")->str() << " |\n";
    }
    markdown << "\nThese probes measure dependency invalidation, not arbitrary semantic changes. Raw compiler counts, CPU times and maximum per-compiler RSS are in incremental.json. RSS is not total build memory.\n";
    write_file(work / "incremental.md", markdown.str());
    persist();
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
    std::int64_t excluded_commands = 0;
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
        const auto relative = file.lexically_relative(variant.source);
        if (std::ranges::any_of(options.exclusions, [&](const fs::path& excluded) {
                return within(relative, excluded);
            })) {
            ++excluded_commands;
            continue;
        }
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
        {
            // Include-cleaner can miss namespace imports or require provider additions.
            // Compile every deletion-only proposal in every configuration. Keep a
            // rejected file unchanged while allowing independent valid proposals.
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
    report["excluded_compile_commands_skipped"] = excluded_commands;
    report["preflight_rejected_files"] = std::move(rejected);
    report["preflight_compile_commands"] = preflight;
    report["removed_includes"] = static_cast<std::int64_t>(findings.size());
    report["findings"] = std::move(findings);
    return changes;
}

void patch(const Changes& changes, const Variant& baseline, const Variant& candidate,
           const fs::path& work, const std::string& filename = "includes.patch") {
    std::string combined;
    for (const auto& [relative, replacements] : changes) {
        const auto original = read_file(candidate.source / relative);
        if (original != read_file(baseline.source / relative))
            throw std::runtime_error("Source changed during analysis: " + relative.string());
        if (filename == "cmake.patch") {
            auto ordered = replacements;
            std::ranges::sort(ordered, [](const auto& a, const auto& b) { return a.offset > b.offset; });
            auto edited = original;
            auto boundary = original.size();
            for (const auto& item : ordered) {
                if (item.offset > boundary || item.length > boundary - item.offset)
                    throw std::runtime_error("Invalid or overlapping CMake dependency edits");
                edited.replace(item.offset, item.length, item.text);
                boundary = item.offset;
            }
            write_file(candidate.source / relative, edited);
        } else {
            write_file(candidate.source / relative, apply_include_replacements(original, replacements));
        }
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
    write_file(work / filename, combined);
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
    if (fs::exists(work / "cmake.patch"))
        std::cout << "Patch: " << (work / "cmake.patch") << '\n';
    if (fs::exists(work / "headers.patch"))
        std::cout << "Patch: " << (work / "headers.patch") << '\n';
    if (fs::exists(work / "unity.patch"))
        std::cout << "Patch: " << (work / "unity.patch") << '\n';
    if (fs::exists(work / "headers.json"))
        std::cout << "Header profile: " << (work / "headers.json") << '\n';
    if (fs::exists(work / "incremental.json"))
        std::cout << "Incremental report: " << (work / "incremental.json")
                  << "\nIncremental summary: " << (work / "incremental.md") << '\n';
}

int parallelism_sweep(const Options& options, const fs::path& executable) {
    check_cmake_args(options.cmake_args);
    const auto source = absolute_path(options.source);
    if (!fs::is_regular_file(source / "CMakeLists.txt"))
        throw std::runtime_error("--source must contain CMakeLists.txt");
    if (options.command == "unity") unity_cmake(options, source);
    else pch_cmake(options, source);
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const auto work = options.work.empty() ? source / ".buildray" /
        (std::to_string(stamp) + "-" + std::to_string(::getpid())) : absolute_path(options.work);
    if (within(source, work)) throw std::runtime_error("Work directory must not contain the source directory");
    fs::create_directories(work.parent_path());
    if (!fs::create_directory(work)) throw std::runtime_error("Work directory already exists: " + work.string());
    json::Array levels, counts;
    for (const auto jobs : options.job_levels) {
        counts.push_back(jobs);
        levels.push_back(json::Object{{"jobs", jobs}, {"status", "not_run"},
            {"report", "jobs-" + std::to_string(jobs) + "/report.json"}});
    }
    json::Object report{{"schema_version", 1}, {"buildray_version", BUILDRAY_VERSION},
        {"command", options.command}, {"parallelism_sweep", true}, {"status", "running"},
        {"source", source.string()}, {"work_dir", work.string()}, {"runs", options.runs},
        {"job_levels", std::move(counts)}, {"levels", std::move(levels)},
        {"patch_validated", false}, {"patch_recommended", false},
        {"candidate_consistency_verified", false}, {"performance_result", "not_measured"},
        {"measurement", "sequential paired experiments on one frozen source tree; no pooling across job counts"}};
    const auto escape = [](std::string value) {
        for (const auto& [from, to] : {std::pair{"|", "&#124;"}, {"`", "&#96;"}, {"\n", " "}, {"\r", " "}}) {
            std::size_t pos = 0;
            while ((pos = value.find(from, pos)) != std::string::npos) { value.replace(pos, std::string(from).size(), to); pos += std::string(to).size(); }
        }
        return value;
    };
    const auto persist = [&] {
        write_json(work / "report.json", json::Object(report));
        write_json(work / "parallelism.json", json::Object(report));
        std::ostringstream out;
        out << std::fixed << std::setprecision(3) << "# Parallelism comparison\n\nStatus: "
            << report.getString("status")->str() << ". Runs are sequential. Each level has its own paired samples and full validation.\n\n"
            << "| Jobs | Before (s) | After (s) | Change | Result | Exceeds detected CPU capacity |\n"
            << "| ---: | ---: | ---: | ---: | --- | --- |\n";
        for (const auto& value : *report.getArray("levels")) {
            const auto& row = *value.getAsObject();
            out << "| " << *row.getInteger("jobs") << " | ";
            if (const auto* before = row.getObject("baseline"); before && row.getObject("candidate"))
                out << *before->getNumber("median_seconds") << " | " << *row.getObject("candidate")->getNumber("median_seconds")
                    << " | " << *row.getNumber("wall_change_percent") << "% | " << row.getString("performance_result")->str();
            else out << "— | — | — | " << row.getString("status")->str();
            const auto over = row.getBoolean("exceeds_detected_cpu_capacity");
            out << " | " << (over ? (*over ? "yes" : "no") : "unknown") << " |\n";
        }
        out << "\nCPU affinity and quota, compiler counts, per-compiler peak RSS, raw samples and test results are in each level's report. RSS is not total build memory.\n"
            << "\n| Jobs | Edited file | Before (s) | After (s) | Change | Result |\n| ---: | --- | ---: | ---: | ---: | --- |\n";
        for (const auto& value : *report.getArray("levels")) {
            const auto& row = *value.getAsObject();
            if (const auto* inc = row.getObject("incremental"))
                for (const auto& scenario : *inc->getArray("scenarios")) {
                    const auto& item = *scenario.getAsObject();
                    if (item.getString("status") != "passed") continue;
                    out << "| " << *row.getInteger("jobs") << " | `" << escape(item.getString("file")->str()) << "` | "
                        << *item.getObject("baseline")->getNumber("median_seconds") << " | "
                        << *item.getObject("candidate")->getNumber("median_seconds") << " | "
                        << *item.getNumber("wall_change_percent") << "% | " << item.getString("performance_result")->str() << " |\n";
                }
        }
        write_file(work / "parallelism.md", out.str());
    };
    persist();
    try {
        // Freeze once, excluding the whole output tree even for a custom nested
        // work directory. Child workspaces are siblings of this input snapshot.
        snapshot(source, work / "input", work);
        std::optional<json::Value> reference_context, reference_plan;
        std::optional<std::string> reference_patch;
        fs::path first_patch;
        std::set<std::string> verdicts;
        bool recommended = true, regression = false;
        for (auto& value : *report.getArray("levels")) {
            auto& row = *value.getAsObject();
            if (interrupted()) throw std::runtime_error("Interrupted during parallelism comparison");
            const auto jobs = static_cast<unsigned>(*row.getInteger("jobs"));
            auto child = options;
            child.job_levels.clear();
            child.jobs = jobs;
            child.source = work / "input";
            child.work = work / ("jobs-" + std::to_string(jobs));
            row["status"] = "running";
            persist();
            std::cout << "Parallelism: " << jobs << " jobs\n" << std::flush;
            const auto code = pipeline(child, executable);
            const auto document = read_json(child.work / "report.json");
            const auto& result = *document.getAsObject();
            row["exit_code"] = code;
            row["status"] = result.getString("status")->str();
            for (const auto* key : {"baseline", "candidate", "wall_change_percent", "performance_result",
                    "baseline_tests_passed", "candidate_tests_passed", "incremental", "patch_validated",
                    "patch_recommended", "measurement_context", "error"})
                if (const auto* item = result.get(key)) row[key] = json::Value(*item);
            persist();
            if (code != 0 || result.getString("status") != "passed" || !result.getBoolean("patch_validated").value_or(false))
                throw std::runtime_error("Parallelism validation failed at " + std::to_string(jobs) + " jobs; see " + (child.work / "report.json").string());
            auto context = json::Object(*result.getObject("measurement_context"));
            double capacity = static_cast<double>(context.getInteger("cpu_affinity_count").value_or(0));
            std::istringstream quota(context.getString("cpu_quota")->str());
            std::string limit;
            double period = 0;
            if (quota >> limit >> period; period > 0 && limit.find_first_not_of("0123456789") == std::string::npos) {
                const auto cores = std::stod(limit) / period;
                if (cores > 0) capacity = capacity > 0 ? std::min(capacity, cores) : cores;
            }
            if (capacity > 0) {
                row["effective_cpu_capacity"] = capacity;
                row["exceeds_detected_cpu_capacity"] = static_cast<double>(jobs) > capacity;
            }
            context.erase("jobs");
            for (const auto* key : {"compiler_version", "cmake_version", "ninja_version"}) context[key] = result.getString(key)->str();
            json::Value identity(std::move(context));
            if (reference_context && *reference_context != identity)
                throw std::runtime_error("Machine, toolchain or configuration changed between job levels");
            if (!reference_context) reference_context = std::move(identity);
            const auto candidate = child.work / (options.command + ".patch");
            const auto contents = read_file(candidate);
            if (reference_patch && *reference_patch != contents)
                throw std::runtime_error("Candidate patch changed between job levels");
            if (!reference_patch) { reference_patch = contents; first_patch = candidate; }
            if (options.command == "unity") {
                const auto plan = read_json(child.work / "unity.json");
                if (reference_plan && *reference_plan != plan)
                    throw std::runtime_error("Unity source groups changed between job levels");
                if (!reference_plan) reference_plan = json::Value(plan);
            }
            recommended &= result.getBoolean("patch_recommended").value_or(false);
            const auto verdict = result.getString("performance_result").value_or("not_measured").str();
            verdicts.insert(verdict);
            regression |= verdict == "regressed";
            if (const auto* inc = result.getObject("incremental")) regression |= inc->getBoolean("regression_detected").value_or(false);
            persist();
        }
        fs::copy_file(first_patch, work / (options.command + ".patch"));
        report["status"] = "passed";
        report["candidate_consistency_verified"] = true;
        report["patch_validated"] = true;
        report["regression_detected"] = regression;
        report["patch_recommended"] = recommended && !regression;
        report["performance_result"] = verdicts.size() == 1 ? *verdicts.begin() : "mixed_results";
        persist();
        std::cout << "Parallelism report: " << work / "parallelism.json" << "\nSummary: " << work / "parallelism.md" << '\n';
        return 0;
    } catch (const std::exception& error) {
        report["status"] = interrupted() ? "interrupted" : "failed";
        report["error"] = std::string(error.what());
        report["patch_validated"] = false;
        report["candidate_consistency_verified"] = false;
        report["patch_recommended"] = false;
        std::error_code cleanup_error;
        fs::remove(work / (options.command + ".patch"), cleanup_error);
        for (auto& value : *report.getArray("levels"))
            if (value.getAsObject()->getString("status") == "running") (*value.getAsObject())["status"] = interrupted() ? "interrupted" : "failed";
        persist();
        std::cerr << "buildray: " << error.what() << "\nReport: " << work / "report.json" << '\n';
        return interrupted() ? 130 : 1;
    }
}

json::Object unused_plan(const Options& options, const Variant& baseline, const Variant& candidate,
                         const fs::path& work, const fs::path& executable, const json::Object& before) {
    const auto started = std::chrono::steady_clock::now();
    const auto elapsed = [&] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(); };
    auto plan = unused_link_candidates(before, candidate.source, options);
    const auto links_doc = read_json(work / "logs/baseline-linkers.json");
    const auto& links = *links_doc.getAsArray();
    json::Object profiles;
    json::Array screened, screening, accepted;
    std::map<fs::path, std::string> originals;
    unsigned configurations = 0, profile_count = 0, cache_hits = 0, final_checks = 0;
    bool exhausted = false;
    struct Cleanup {
        fs::path binaries;
        ~Cleanup() { std::error_code error; fs::remove_all(binaries, error); }
    } cleanup{work / "dependency-profiles/binaries"};
    const auto time_left = [&] {
        if (interrupted()) throw std::runtime_error("Interrupted during dependency search");
        if (elapsed() >= options.dependency_time_budget) { exhausted = true; return false; }
        return true;
    };
    const auto persist = [&] {
        plan["selections"] = json::Array(accepted);
        plan["screening"] = json::Array(screening);
        plan["search"] = json::Object{{"profiled_links", profile_count}, {"configurations", configurations},
            {"cache_hits", cache_hits}, {"final_verifications", final_checks},
            {"trial_limit", options.max_dependency_trials}, {"time_budget_seconds", options.dependency_time_budget},
            {"budget_exhausted", exhausted}, {"elapsed_seconds", elapsed()}};
        write_json(work / "links.json", json::Object(plan));
    };
    if (!plan.getArray("candidates")->empty()) {
        try { cache_link_compilations(baseline.source, baseline.build, profiles); }
        catch (const std::exception& error) {
            if (interrupted()) throw;
            plan["search_blocker"] = std::string(error.what());
        }
    }
    // One extraction replay per relevant consumer, never one per dependency.
    for (const auto& value : *plan.getArray("candidates")) {
        auto row = json::Object(*value.getAsObject());
        if (const auto blocker = plan.getString("search_blocker")) {
            row["status"] = "unsupported_configuration";
            row["reason"] = blocker->str();
            screening.push_back(std::move(row));
            continue;
        }
        const auto output = row.getString("output")->str();
        const auto archive = row.getString("archive")->str();
        originals.try_emplace(fs::path(row.getString("file")->str()), read_file(candidate.source / row.getString("file")->str()));
        if (!profiles.getObject(output)) {
            if (!time_left()) {
                row["status"] = "unexamined_budget";
                screening.push_back(std::move(row));
                continue;
            }
            const auto record = std::ranges::find_if(links, [&](const json::Value& link) {
                const auto path = fs::path(link.getAsObject()->getString("output")->str());
                return "<BUILD>/" + path.lexically_relative(baseline.build).generic_string() == output;
            });
            try {
                if (record == links.end()) throw std::runtime_error("Consumer is not part of the active default build");
                std::cout << "Profiling archive use: " << row.getString("target")->str() << '\n' << std::flush;
                profiles[output] = profile_link_archives(*record->getAsObject(), baseline.source, baseline.build,
                    work / "dependency-profiles" / std::to_string(profile_count++));
            } catch (const std::exception& error) {
                if (interrupted()) throw;
                profiles[output] = json::Object{{"error", std::string(error.what())}};
            }
        }
        const auto& profile = *profiles.getObject(output);
        const auto* archives = profile.getObject("archives");
        const auto* stats = archives ? archives->getObject(archive) : nullptr;
        if (stats && stats->getInteger("extracted") == 0) {
            row["status"] = "candidate";
            screened.push_back(json::Object(*value.getAsObject()));
        } else {
            row["status"] = stats ? "contributes_objects" : "unsupported_or_absent";
            if (stats) row["extracted"] = *stats->getInteger("extracted");
            if (const auto error = profile.getString("error")) row["reason"] = error->str();
        }
        screening.push_back(std::move(row));
    }
    plan["screened_candidates"] = static_cast<std::int64_t>(screened.size());
    persist();
    write_json(work / "archive-profile.json", json::Object(profiles));
    const auto restore = [&] {
        for (const auto& [file, original] : originals) write_file(candidate.source / file, original);
    };
    const auto configure_group = [&](const json::Array& group, const fs::path& folder) {
        restore();
        Changes changes;
        for (auto edit : dependency_replacements(candidate.source, group))
            changes[fs::path(edit.file)].push_back(std::move(edit));
        patch(changes, baseline, candidate, work, "cmake.patch");
        fs::create_directories(folder);
        fs::copy_file(work / "cmake.patch", folder / "cmake.patch", fs::copy_options::overwrite_existing);
        fs::remove(candidate.build / "CMakeCache.txt");
        fs::remove_all(candidate.build / "CMakeFiles");
        configure(options, candidate, executable, folder / "configure.log");
        if (measurement_context(options, baseline.build) != measurement_context(options, candidate.build))
            throw std::runtime_error("Dependency removal changed measurement context or CMake flags");
        const auto after = cmake_dependencies(candidate.source, candidate.build);
        write_json(folder / "cmake-after.json", json::Object(after));
        verify_link_removals(before, after, group);
        auto proof = verify_unused_link_inputs(baseline.source, baseline.build, candidate.source,
            candidate.build, links, before, folder / "link-replay", executable, profiles);
        for (auto& item : *proof.getArray("changed_links")) {
            auto& row = *item.getAsObject();
            for (const auto* key : {"evidence_directory", "baseline_evidence_directory"})
                row[key] = fs::path(row.getString(key)->str()).lexically_relative(work).generic_string();
        }
        write_json(folder / "proof.json", json::Object(proof));
        return proof;
    };
    std::map<std::string, std::string> failures;
    const auto trial = [&](const json::Array& batch, bool compile_only) {
        json::Array proposed(accepted);
        for (const auto& value : batch) {
            auto row = json::Object(*value.getAsObject());
            row["compile_only"] = compile_only;
            proposed.push_back(std::move(row));
        }
        // The same cumulative edit may recur after bisection. Reuse its rejection.
        std::vector<std::string> keys;
        for (const auto& value : proposed) {
            const auto& row = *value.getAsObject();
            keys.push_back(row.getString("file")->str() + ":" + std::to_string(*row.getInteger("offset")) +
                (row.getBoolean("compile_only").value_or(false) ? ":compile" : ":delete"));
        }
        std::ranges::sort(keys);
        std::string key;
        for (const auto& item : keys) key += std::to_string(item.size()) + ":" + item;
        auto attempt = json::Object{{"selections", json::Array(proposed)},
            {"batch_size", static_cast<std::int64_t>(batch.size())}, {"compile_only", compile_only}};
        if (const auto found = failures.find(key); found != failures.end()) {
            ++cache_hits;
            attempt["status"] = "rejected";
            attempt["cached"] = true;
            attempt["reason"] = found->second;
            plan.getArray("attempts")->push_back(std::move(attempt));
            persist();
            return false;
        }
        if (!time_left() || configurations >= options.max_dependency_trials) { exhausted = true; persist(); return false; }
        const auto folder = work / "dependency-trials" / std::to_string(configurations++);
        attempt["evidence_directory"] = folder.lexically_relative(work).generic_string();
        std::cout << "Dependency group: " << proposed.size() << " links (" << batch.size()
                  << " new, " << (compile_only ? "compile-only" : "delete") << ")\n" << std::flush;
        try {
            configure_group(proposed, folder);
            accepted = std::move(proposed);
            attempt["status"] = "accepted";
            plan.getArray("attempts")->push_back(std::move(attempt));
            persist();
            return true;
        } catch (const std::exception& error) {
            restore();
            if (interrupted()) throw;
            failures[key] = std::string(error.what());
            attempt["status"] = "rejected";
            attempt["reason"] = std::string(error.what());
            plan.getArray("attempts")->push_back(std::move(attempt));
            persist();
            return false;
        }
    };
    std::function<void(const json::Array&)> search = [&](const json::Array& batch) {
        if (batch.empty() || exhausted) return;
        if (trial(batch, false) || exhausted || trial(batch, true) || exhausted) return;
        if (batch.size() == 1) return;
        json::Array left, right;
        for (std::size_t i = 0; i < batch.size(); ++i)
            (i < batch.size() / 2 ? left : right).push_back(json::Object(*batch[i].getAsObject()));
        search(left);
        search(right);
    };
    try {
        search(screened);
        restore();
        if (!accepted.empty()) {
            ++final_checks;
            // Revalidate the combined set even when the search ended on a rejection
            // or reached a budget. This mandatory step is outside the search budget.
            auto proof = configure_group(accepted, work / "dependency-trials/final");
            plan["link_evidence"] = std::move(proof);
            plan["selection"] = json::Object(*accepted.front().getAsObject());
        } else fs::remove(work / "cmake.patch");
    } catch (...) {
        restore();
        fs::remove(work / "cmake.patch");
        throw;
    }
    plan["search_exhausted"] = !exhausted && !plan.getString("search_blocker");
    persist();
    return plan;
}

int automatic_pch(const Options& options, const fs::path& executable) {
    check_cmake_args(options.cmake_args);
    const auto source = absolute_path(options.source);
    if (!fs::is_regular_file(source / "CMakeLists.txt"))
        throw std::runtime_error("--source must contain CMakeLists.txt");
    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
    const auto work = options.work.empty()
        ? source / ".buildray" / (std::to_string(stamp) + "-" + std::to_string(::getpid()))
        : absolute_path(options.work);
    if (within(source, work)) throw std::runtime_error("Work directory must not contain the source directory");
    fs::create_directories(work.parent_path());
    if (!fs::create_directory(work))
        throw std::runtime_error("Work directory already exists: " + work.string());
    fs::create_directories(work / "logs");
    json::Object report{{"schema_version", 1}, {"buildray_version", BUILDRAY_VERSION},
                         {"command", "pch"}, {"pch_auto", true}, {"status", "running"},
                         {"phase", "profiling"}, {"source", source.string()},
                         {"work_dir", work.string()}, {"patch_validated", false},
                         {"patch_recommended", false}};
    write_json(work / "report.json", json::Object(report));
    const auto retain_logs = [&](const std::string& phase) {
        const auto directory = work / phase / "logs";
        if (!fs::exists(directory)) return;
        for (const auto& entry : fs::directory_iterator(directory)) {
            const auto name = entry.path().filename().string();
            const bool incremental = name.starts_with("incremental-");
            if (!entry.is_regular_file() || entry.is_symlink() ||
                (entry.path().extension() != ".log" && !incremental)) continue;
            const auto destination = work / "logs" / (incremental ? name : phase + "-" + name);
            fs::copy_file(entry.path(), destination, fs::copy_options::overwrite_existing);
            fs::last_write_time(destination, fs::last_write_time(entry.path()));
        }
    };
    const auto retain = [&](const std::string& phase, const std::string& name) {
        const auto path = work / phase / name;
        if (fs::is_regular_file(path) && !fs::is_symlink(path))
            fs::copy_file(path, work / name, fs::copy_options::overwrite_existing);
    };
    try {
        std::cout << "Automatic PCH workspace: " << work << '\n' << std::flush;
        // Freeze inputs once. Child workspaces are siblings, so an explicit work
        // directory inside the project cannot recursively enter later snapshots.
        snapshot(source, work / "input", work);
        auto profile_options = options;
        profile_options.command = "profile";
        profile_options.auto_pch = false;
        profile_options.source = work / "input";
        profile_options.work = work / "profile";
        profile_options.target.clear();
        profile_options.headers.clear();
        profile_options.rebuild_files.clear();
        profile_options.incremental_only = false;
        const int profile_status = pipeline(profile_options, executable);
        retain_logs("profile");
        if (profile_status != 0)
            throw std::runtime_error("PCH profiling failed; see " + (work / "profile/report.json").string());
        retain("profile", "headers.json");
        retain("profile", "headers.md");
        const auto profile_document = read_json(work / "headers.json");
        const auto* profile = profile_document.getAsObject();
        if (!profile) throw std::runtime_error("Invalid automatic PCH profile");
        auto selection = select_pch(*profile, work / "input", options.target);
        report["automatic_selection"] = json::Object(selection);
        report["phase"] = "experiment";
        write_json(work / "selection.json", json::Object(selection));
        write_json(work / "report.json", json::Object(report));
        std::cout << "Automatic PCH candidate: " << selection.getString("target")->str()
                  << " with " << selection.getString("header")->str() << " ("
                  << *selection.getInteger("compilations") << " compilations).\n" << std::flush;
        auto experiment_options = options;
        experiment_options.auto_pch = false;
        experiment_options.source = work / "input";
        experiment_options.work = work / "experiment";
        experiment_options.target = selection.getString("target")->str();
        experiment_options.headers = {selection.getString("header")->str()};
        const int experiment_status = pipeline(experiment_options, executable);
        retain_logs("experiment");
        retain("experiment", "pch.patch");
        retain("experiment", "incremental.json");
        retain("experiment", "incremental.md");
        const auto experiment_document = read_json(work / "experiment/report.json");
        const auto* experiment = experiment_document.getAsObject();
        if (!experiment) throw std::runtime_error("Invalid automatic PCH experiment report");
        report = json::Object(*experiment);
        report["pch_auto"] = true;
        report["source"] = source.string();
        report["work_dir"] = work.string();
        report["automatic_selection"] = std::move(selection);
        report["profile_report"] = (work / "profile/report.json").string();
        report["experiment_report"] = (work / "experiment/report.json").string();
        report["selection_profiling_in_measurements"] = false;
        if (experiment_status != 0)
            throw std::runtime_error("PCH experiment failed: " +
                experiment->getString("error").value_or("unknown failure").str());
        if (interrupted()) throw std::runtime_error("Interrupted");
        write_json(work / "report.json", json::Object(report));
        std::cout << "Automatic PCH report: " << (work / "report.json")
                  << "\nPatch: " << (work / "pch.patch") << '\n';
        return 0;
    } catch (const std::exception& error) {
        report["status"] = interrupted() ? "interrupted" : "failed";
        report["error"] = std::string(error.what());
        report["patch_recommended"] = false;
        retain_logs("profile");
        retain_logs("experiment");
        write_file(work / "logs/automatic-pch.log", error.what());
        write_json(work / "report.json", json::Object(report));
        std::cerr << "buildray: " << error.what() << "\nReport: " << (work / "report.json") << '\n';
        return interrupted() ? 130 : 1;
    }
}
}  // namespace

std::vector<std::string> compile_arguments(const json::Object& entry) {
    return compiler_arguments(entry);
}

int pipeline(const Options& options, const fs::path& executable) {
    if (!options.job_levels.empty()) return parallelism_sweep(options, executable);
    if (options.auto_pch) return automatic_pch(options, executable);
    check_cmake_args(options.cmake_args);
    const auto source = absolute_path(options.source);
    const auto base_source =
        options.command == "compare" ? absolute_path(options.baseline_source) : source;
    if (!fs::is_regular_file(source / "CMakeLists.txt"))
        throw std::runtime_error("--source must contain CMakeLists.txt: " + source.string());
    if (!fs::is_regular_file(base_source / "CMakeLists.txt"))
        throw std::runtime_error("--baseline-source must contain CMakeLists.txt");
    std::set<fs::path> rebuild_paths;
    for (const auto& file : options.rebuild_files) {
        const auto ext = file.extension();
        if ((!cpp_source(file) && ext != ".c" && ext != ".h" && ext != ".hh" &&
             ext != ".hpp" && ext != ".hxx" && ext != ".inc" && ext != ".inl") ||
            !within(absolute_path(source / file), source) || !fs::is_regular_file(source / file) ||
            fs::is_symlink(source / file) || !rebuild_paths.insert(file).second)
            throw std::runtime_error("--rebuild-file requires a unique original C/C++ source or header: " + file.string());
    }
    // Validate generated CMake inputs before creating an output directory or running project code.
    const auto pch_script = options.command == "pch" ? pch_cmake(options, source) : "";
    const auto unity_script = options.command == "unity" ? unity_cmake(options, source) : "";
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
    json::Array exclusions;
    for (const auto& path : options.exclusions) exclusions.push_back(path.generic_string());
    json::Object report{
        {"schema_version", 1},
        {"buildray_version", BUILDRAY_VERSION},
        {"status", "running"},
        {"command", options.command},
        {"source", source.string()},
        {"work_dir", work.string()},
        {"jobs", options.jobs},
        {"runs", options.command == "profile" || options.command == "deps" ? 1u : options.runs},
        {"cmake_args", std::move(extra)},
        {"excluded_paths", std::move(exclusions)},
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
        if (options.command == "deps" || link_experiment(options) || options.command == "headers" || options.command == "unity" ||
            (options.command == "pch" && !options.rebuild_files.empty())) {
            checked({options.c_compiler, "--version"}, source, logs / "c-compiler-version.log");
            report["c_compiler_version"] = read_file(logs / "c-compiler-version.log");
            if (report.getString("c_compiler_version")->find("clang") == llvm::StringRef::npos)
                throw std::runtime_error("This command requires Clang for C as well as C++");
        }
        if (options.command == "deps" || link_experiment(options)) {
            checked({"cmake", "-E", "capabilities"}, source, logs / "cmake-capabilities.json");
            const auto capabilities = read_json(logs / "cmake-capabilities.json");
            const auto* object = capabilities.getAsObject();
            const auto* version = object ? object->getObject("version") : nullptr;
            if (!version || version->getInteger("major").value_or(0) < 4 ||
                (version->getInteger("major") == 4 && version->getInteger("minor").value_or(0) < 2))
                throw std::runtime_error("Dependency analysis requires CMake 4.2+ for direct target relationships");
        }
        if (options.command == "optimize" || options.command == "headers") {
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
                         work / "baseline-records", options.command == "profile", {}, {}, options.command == "deps"};
        Variant candidate{work / "candidate-source", work / "candidate-build",
                          work / "candidate-records", false,
                          options.command == "pch" ? options.target : "", options.headers};
        baseline.cmake_model = candidate.cmake_model = link_experiment(options);
        baseline.links = candidate.links = link_experiment(options);
        baseline.active_coverage = candidate.active_coverage = link_experiment(options) || options.command == "headers" || options.command == "unity" || !options.rebuild_files.empty();
        if (options.command == "unity") candidate.unity_target = options.target;
        report["phase"] = "baseline_validation";
        std::cout << "Configuring baseline, warming build, running tests...\n" << std::flush;
        snapshot(base_source, baseline.source, work);
        std::vector<fs::path> dependency_originals;
        if (options.command == "deps") {
            for (const auto& entry : fs::recursive_directory_iterator(baseline.source))
                if (entry.is_regular_file())
                    dependency_originals.push_back(entry.path().lexically_relative(baseline.source));
        }
        // Configure scripts can write generated links or files into their source tree.
        // Prepare both inputs before either configure runs; optimize uses identical bytes.
        if (options.command == "optimize" || options.command == "pch" || link_experiment(options) || options.command == "headers" || options.command == "unity")
            snapshot(baseline.source, candidate.source, work);
        if (options.command == "compare") snapshot(source, candidate.source, work);
        SourceFiles originals;
        if (options.command == "optimize" || options.command == "headers") {
            for (const auto& entry : fs::recursive_directory_iterator(candidate.source)) {
                const auto ext = entry.path().extension();
                if (entry.is_regular_file() && (cpp_source(entry.path()) ||
                    (options.command == "headers" && (ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx"))))
                    originals[entry.path().lexically_relative(candidate.source)] =
                        read_file(entry.path());
            }
        }
        configure(options, baseline, executable, logs / "baseline-configure.log");
        auto initial_context = measurement_context(options, baseline.build);
        if (options.command == "profile" || options.command == "deps") {
            initial_context["method"] = options.command == "deps" ? "clang-include-tree-cmake-codemodel-v1" : "clang-time-trace-v1";
            initial_context["sampling"] = "single-instrumented-build";
            report["profiling_context"] = std::move(initial_context);
        } else {
            report["measurement_context"] = std::move(initial_context);
        }
        clear_variant(baseline);
        build(options, baseline, logs / "baseline-warmup.log");
        verify_coverage(baseline);
        if (baseline.links) {
            json::Object link_summary;
            add_links(link_summary, baseline, logs / "baseline", true);
            report["baseline_validation_links"] = std::move(link_summary);
            report["link_measurement_scope"] = "compiler-driver link invocations, including driver startup; sums may overlap under parallelism; maximum individual driver RSS, not aggregate memory; archive analysis and replays excluded from timing";
        }
        const auto baseline_tests = test(options, baseline, logs, "baseline");
        report["baseline_tests_passed"] = static_cast<std::int64_t>(baseline_tests);
        if (options.allow_baseline_skips)
            report["baseline_skipped_tests"] = read_json(logs / "baseline-skipped-tests.json");
        if (options.command == "deps") {
            report["phase"] = "dependency_analysis";
            auto graph = dependency_graph(baseline.records, baseline.source, baseline.build, dependency_originals);
            const auto include_cycles = graph.getArray("include_cycles")->size();
            const auto target_cycles = graph.getObject("cmake")->getArray("link_cycles")->size();
            const bool cycles = include_cycles || target_cycles;
            report["include_cycles"] = static_cast<std::int64_t>(include_cycles);
            report["target_link_cycles"] = static_cast<std::int64_t>(target_cycles);
            report["fail_on_cycles"] = options.fail_on_cycles;
            report["cycle_gate_passed"] = !options.fail_on_cycles || !cycles;
            report["validation_scope"] = "default Ninja all target; C and C++; original snapshot include graph; configured CMake targets";
            report["validation"] = baseline_tests ? "build_and_tests_passed" : "build_only";
            report["measurement"] = "one include-instrumented build; not a performance benchmark";
            std::cout << "Dependency graph: " << *graph.getInteger("compilations") << " compilations, "
                      << include_cycles << " include cycles, " << target_cycles << " target-link cycles.\n";
            write_file(work / "dependencies.md", dependencies_markdown(graph));
            write_file(work / "dependencies.dot", dependencies_dot(graph));
            write_json(work / "dependencies.json", std::move(graph));
            if (options.fail_on_cycles && cycles) pipeline_exit = 5;
        } else if (options.command == "profile") {
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
            if (options.allow_baseline_skips)
                report["candidate_skipped_tests"] = read_json(logs / "candidate-skipped-tests.json");
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
            json::Object cmake_before;
            json::Object selected_link;
            json::Array selected_links;
            if (link_experiment(options)) {
                cmake_before = cmake_dependencies(baseline.source, baseline.build);
                write_json(work / "cmake-before.json", json::Object(cmake_before));
                auto plan = options.command == "prune-deps" ?
                    unused_plan(options, baseline, candidate, work, executable, cmake_before) :
                    link_candidates(cmake_before, candidate.source, options);
                auto& candidates = *plan.getArray("candidates");
                report["eligible_links"] = options.command == "prune-deps" ? *plan.getInteger("candidate_links") : static_cast<std::int64_t>(candidates.size());
                report["removed_links"] = 0;
                report["validation_scope"] = "one configured CMake build; one redundant literal target link; original snapshot files only";
                if (options.command == "prune-deps") {
                    report["validation_scope"] = "one configured Ninja all build; combined private literal static dependency removals; compiler commands preserved; zero extracted members and byte-identical ELF replay; full build and CTest";
                    report["dependency_trials"] = json::Array(*plan.getArray("attempts"));
                    report["dependency_search"] = json::Object(*plan.getObject("search"));
                    report["dependency_screening"] = json::Array(*plan.getArray("screening"));
                    if (const auto blocker = plan.getString("search_blocker")) report["dependency_search_blocker"] = blocker->str();
                    selected_links = json::Array(*plan.getArray("selections"));
                    report["selected_links"] = json::Array(selected_links);
                }
                if (options.command == "prune-deps" ? !plan.getObject("selection") : candidates.empty()) {
                    write_json(work / "links.json", std::move(plan));
                    if (!options.dependency.empty())
                        throw std::runtime_error("Requested dependency has no eligible validated link removal; see links.json");
                    report["status"] = "passed";
                    report["phase"] = "complete";
                    report["performance_result"] = "no_changes";
                    report["patch_recommended"] = false;
                    report["validation"] = baseline_tests ? "build_and_tests_passed" : "build_only";
                    report["measurement"] = "no confirmed eligible link; baseline validation only; no performance benchmark; inspect links.json for rejected trials and unsupported cases";
                    if (options.command == "prune-deps" && !options.incremental_only) {
                        json::Array samples;
                        for (unsigned i = 0; i < options.runs; ++i) {
                            std::cout << "Measuring baseline links " << i + 1 << '/' << options.runs << "...\n" << std::flush;
                            samples.push_back(sample(options, baseline, logs, "baseline-" + std::to_string(i)));
                        }
                        report["baseline"] = summarize(samples);
                        report["measurement"] = "baseline-only clean builds with link-driver measurements; no confirmed removable dependency and no candidate comparison; inspect links.json for rejected or unsupported cases";
                    }
                    if (interrupted()) throw std::runtime_error("Interrupted");
                    write_json(work / "report.json", json::Object(report));
                    std::cout << "No confirmed eligible CMake link removal.\n";
                    print_summary(report, work);
                    return 0;
                }
                selected_link = json::Object(options.command == "prune-deps" ?
                    *plan.getObject("selection") : *candidates.front().getAsObject());
                plan["selection"] = json::Object(selected_link);
                report["selected_link"] = json::Object(selected_link);
                if (options.command != "prune-deps") {
                    const fs::path file = selected_link.getString("file")->str();
                    changes[file] = {{file.string(), static_cast<std::size_t>(*selected_link.getInteger("offset")),
                                      static_cast<std::size_t>(*selected_link.getInteger("length")), ""}};
                    patch(changes, baseline, candidate, work, "cmake.patch");
                }
                if (const auto* proof = plan.getObject("link_evidence")) report["link_evidence"] = json::Object(*proof);
                write_json(work / "links.json", std::move(plan));
                if (options.command == "prune-deps")
                    std::cout << "CMake candidate: remove " << selected_links.size() << " dependency links\n" << std::flush;
                else std::cout << "CMake candidate: remove " << selected_link.getString("target")->str()
                               << " -> " << selected_link.getString("dependency")->str() << '\n' << std::flush;
            }
            if (options.command == "pch" || options.command == "unity") {
                const auto path = candidate.source / "CMakeLists.txt";
                if (read_file(path) != read_file(baseline.source / "CMakeLists.txt"))
                    throw std::runtime_error("CMakeLists.txt changed during baseline configuration");
                write_file(path, read_file(path) + (options.command == "pch" ? pch_script : unity_script));
                const auto result = run({"diff", "-u", "--label", "a/CMakeLists.txt", "--label",
                                         "b/CMakeLists.txt", (baseline.source / "CMakeLists.txt").string(),
                                         path.string()}, work, work / (options.command + ".patch"));
                if (result.code != 1) throw std::runtime_error("Cannot generate CMake experiment patch");
            }
            if (options.command == "unity") {
                report["unity_target"] = options.target;
                report["unity_batch_size"] = options.batch_size;
                report["unity_retries"] = options.unity_retries;
                report["unity_attempts"] = json::Array{};
                report["unity_auto_exclusions"] = json::Array{};
                json::Array unity_exclusions;
                for (const auto& file : options.unity_exclusions) unity_exclusions.push_back(file.generic_string());
                report["unity_exclusions"] = std::move(unity_exclusions);
                report["candidate_unity_build"] = true;
                report["validation_scope"] = "one configured Ninja default all target; unity on one selected target; original C/C++ source/target multiset preserved; complete build and CTest";
            }
            if (options.command == "pch") {
                report["pch_target"] = options.target;
                json::Array headers;
                for (const auto& header : options.headers) headers.push_back(header);
                report["pch_headers"] = std::move(headers);
                report["candidate_precompiled_headers"] = true;
                report["validation_scope"] = "one configured CMake build; private C++ PCH on one target";
            }
            if (options.command != "prune-deps") configure(options, candidate, executable, logs / "candidate-configure.log");
            if (options.command == "pch" || link_experiment(options) || options.command == "unity") {
                auto context = measurement_context(options, candidate.build);
                if (json::Value(json::Object(context)) != *report.get("measurement_context"))
                    throw std::runtime_error("Experiment changed measurement context or CMake flags");
                context["precompiled_headers"] = options.command == "pch";
                context["unity_build"] = options.command == "unity";
                report["candidate_measurement_context"] = std::move(context);
            }
            if (link_experiment(options)) {
                auto after = cmake_dependencies(candidate.source, candidate.build);
                write_json(work / "cmake-after.json", json::Object(after));
                if (options.command == "prune-deps") verify_link_removals(cmake_before, after, selected_links);
                else verify_link_removal(cmake_before, after, selected_link);
                report["removed_links"] = options.command == "prune-deps" ? static_cast<std::int64_t>(selected_links.size()) : 1;
                report["cmake_graph_change_verified"] = true;
            }
            if (options.command == "optimize" || options.command == "headers") {
                // Generated headers must exist before clang-tidy parses sources.
                clear_variant(candidate);
                build(options, candidate, logs / "candidate-initial-build.log");
                verify_coverage(candidate);
                if (options.command == "headers") {
                    for (auto item : analyze_headers(options, candidate.source, candidate.build, work, executable, originals, report))
                        changes[fs::path(item.file)].push_back(std::move(item));
                    report["validation_scope"] = "one configured Ninja default all target; observed original project headers; standalone and observed C/C++ user preflights; complete default build and CTest";
                    patch(changes, baseline, candidate, work, "headers.patch");
                } else {
                    std::cout << "Analyzing unused includes with clang-tidy...\n" << std::flush;
                    changes = analyze(options, candidate, work, originals, report);
                    patch(changes, baseline, candidate, work);
                }
            }
            report["phase"] = "candidate_validation";
            write_json(work / "report.json", json::Object(report));
            std::cout << "Validating candidate with a clean build and tests...\n" << std::flush;
            auto unity_options = options;
            for (unsigned attempt = 0;; ++attempt) {
                const auto label = attempt == 0 ? std::string("candidate-validation") :
                                   "candidate-retry-" + std::to_string(attempt);
                clean(candidate, logs / (label + "-clean.log"));
                clear_variant(candidate);
                try {
                    build(options, candidate, logs / (label + "-build.log"));
                    if (options.command == "unity")
                        report.getArray("unity_attempts")->push_back(json::Object{
                            {"attempt", attempt}, {"build_passed", true},
                            {"build_log", "logs/" + label + "-build.log"}});
                    break;
                } catch (const std::exception&) {
                    if (options.command != "unity" || interrupted()) throw;
                    auto attempt_report = json::Object{
                        {"attempt", attempt}, {"build_passed", false},
                        {"build_log", "logs/" + label + "-build.log"}};
                    const auto records = compiler_records(candidate.records);
                    write_json(work / ("unity-attempt-" + std::to_string(attempt) + "-records.json"),
                               json::Array(records));
                    report.getArray("unity_attempts")->push_back(std::move(attempt_report));
                    write_json(work / "report.json", json::Object(report));
                    if (attempt >= options.unity_retries) throw;
                    const auto current_plan = verify_unity_plan(baseline.source, baseline.build,
                        candidate.source, candidate.build, options);
                    write_json(work / ("unity-attempt-" + std::to_string(attempt) + "-plan.json"),
                               json::Object(current_plan));
                    const auto isolated = failed_unity_sources(records, current_plan,
                                                               candidate.source, candidate.build);
                    auto* history = report.getArray("unity_auto_exclusions");
                    for (const auto& file : isolated) {
                        if (!fs::is_regular_file(source / file))
                            throw std::runtime_error("Unity isolation requires files from the original source tree");
                        if (std::ranges::find(unity_options.unity_exclusions, file) !=
                            unity_options.unity_exclusions.end())
                            throw std::runtime_error("Unity exclusion did not isolate a failed source");
                    }
                    for (const auto& file : isolated) {
                        unity_options.unity_exclusions.push_back(file);
                        history->push_back(file.generic_string());
                    }
                    const auto path = candidate.source / "CMakeLists.txt";
                    write_file(path, read_file(baseline.source / "CMakeLists.txt") +
                                     unity_cmake(unity_options, candidate.source));
                    const auto diff = run({"diff", "-u", "--label", "a/CMakeLists.txt", "--label",
                        "b/CMakeLists.txt", (baseline.source / "CMakeLists.txt").string(), path.string()},
                        work, work / "unity.patch");
                    if (diff.code != 1) throw std::runtime_error("Cannot generate isolated unity patch");
                    std::cout << "Unity retry " << attempt + 1 << ": keeping " << isolated.size()
                              << " failed-group sources separate...\n" << std::flush;
                    write_json(work / "report.json", json::Object(report));
                    configure(options, candidate, executable, logs / ("candidate-retry-" +
                              std::to_string(attempt + 1) + "-configure.log"));
                    if (json::Value(measurement_context(options, candidate.build)) !=
                        *report.get("measurement_context"))
                        throw std::runtime_error("Unity retry changed measurement context or CMake flags");
                }
            }
            verify_coverage(candidate);
            if (candidate.links) {
                json::Object link_summary;
                add_links(link_summary, candidate, logs / "candidate", true);
                report["candidate_validation_links"] = std::move(link_summary);
            }
            if (options.command == "unity") {
                auto plan = verify_unity_plan(baseline.source, baseline.build, candidate.source,
                                               candidate.build, options);
                report["unity_plan"] = json::Object(plan);
                write_json(work / "unity.json", std::move(plan));
            }
            const auto candidate_tests = test(options, candidate, logs, "candidate");
            if (options.allow_baseline_skips)
                report["candidate_skipped_tests"] = read_json(logs / "candidate-skipped-tests.json");
            report["candidate_tests_passed"] = static_cast<std::int64_t>(candidate_tests);
            unsigned baseline_opaque = 0, candidate_opaque = 0;
            const auto baseline_names = test_names(logs / "baseline-tests.json", baseline_opaque);
            const auto candidate_names = test_names(logs / "candidate-tests.json", candidate_opaque);
            report["test_inventory_identity"] = json::Object{
                {"policy", "CTest names with multiplicity; GTest opaque-byte values use stable filters"},
                {"baseline_opaque_values", baseline_opaque},
                {"candidate_opaque_values", candidate_opaque}};
            if (baseline_tests != candidate_tests || baseline_names != candidate_names) {
                json::Array missing, added;
                for (const auto& name : baseline_names)
                    if (baseline_names.count(name) != candidate_names.count(name) && missing.size() < 10)
                        missing.push_back(name);
                for (const auto& name : candidate_names)
                    if (baseline_names.count(name) != candidate_names.count(name) && added.size() < 10)
                        added.push_back(name);
                report["test_inventory_difference"] = json::Object{
                    {"missing_or_changed_multiplicity", std::move(missing)},
                    {"added_or_changed_multiplicity", std::move(added)}};
                throw std::runtime_error("CTest inventory changed");
            }
            report["validation"] = baseline_tests ? "build_and_tests_passed" : "build_only";
            report["patch_validated"] = true;
            if (options.command == "headers" && changes.empty()) {
                report["performance_result"] = "no_changes";
                report["patch_recommended"] = false;
                report["measurement"] = "no eligible header changes; build and tests validated; benchmark skipped";
            } else if (options.incremental_only) {
                report["performance_result"] = "not_measured";
                report["patch_recommended"] = false;
                report["measurement"] = "incremental timings only; full baseline and candidate builds and tests validated outside timing";
            } else {
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
                const bool changed = options.command == "pch" || options.command == "unity" || !changes.empty();
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
            if (!options.rebuild_files.empty())
                incremental_samples(options, baseline, candidate, work, report);
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
        if (auto* incremental = report.getObject("incremental")) {
            (*incremental)["status"] = interrupted() ? "interrupted" : "failed";
            (*incremental)["error"] = error.what();
            write_json(work / "incremental.json", json::Object(*incremental));
        }
        write_json(work / "report.json", json::Object(report));
        std::cerr << "buildray: " << error.what() << "\nReport: " << (work / "report.json") << '\n';
        return interrupted() ? 130 : 1;
    }
}
}  // namespace buildray
