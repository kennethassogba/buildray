#include <sched.h>
#include <sys/utsname.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "buildray/core.hpp"

namespace buildray {
namespace {
std::string environment(const char* key) {
    const auto* value = std::getenv(key);
    return value ? value : "";
}

std::map<std::string, std::string> cache_values(const fs::path& build) {
    std::map<std::string, std::string> values;
    std::istringstream input(read_file(build / "CMakeCache.txt"));
    std::string line;
    while (std::getline(input, line)) {
        if (line.starts_with("//") || line.starts_with('#')) continue;
        const auto colon = line.find(':');
        const auto equal = line.find('=', colon);
        if (colon != std::string::npos && equal != std::string::npos)
            values[line.substr(0, colon)] = line.substr(equal + 1);
    }
    return values;
}

std::string cpu_model() {
    std::istringstream input(read_file("/proc/cpuinfo"));
    std::string line;
    while (std::getline(input, line)) {
        if (!line.starts_with("model name") && !line.starts_with("Hardware")) continue;
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto start = line.find_first_not_of(" \t", colon + 1);
        return start == std::string::npos ? "unknown" : line.substr(start);
    }
    return "unknown";
}

const json::Object& valid_measurement(const json::Value& value) {
    const auto* object = value.getAsObject();
    if (!object || object->getInteger("schema_version") != 1 ||
        object->getString("status") != "passed" || object->getString("command") != "measure")
        throw std::runtime_error("Expected a successful schema-1 measure report");
    if (!object->getObject("measurement_context"))
        throw std::runtime_error(
            "Report has no measurement_context; rerun measure with Buildray 0.2+");
    const auto& context = *object->getObject("measurement_context");
    for (const auto* key : {"method", "buildray_version", "sampling", "session", "cpu_model",
                            "architecture", "kernel", "cpu_quota"}) {
        if (!context.getString(key)) throw std::runtime_error("Incomplete measurement_context");
    }
    if (!context.getInteger("jobs") || *context.getInteger("jobs") < 1 ||
        !context.getInteger("cpu_affinity_count") || !context.getBoolean("tests_required") ||
        !context.getArray("cmake_args") || !context.getObject("cache_flags") ||
        !context.getObject("environment_flags"))
        throw std::runtime_error("Incomplete measurement_context");
    if (!object->getInteger("baseline_tests_passed") ||
        *object->getInteger("baseline_tests_passed") < 0)
        throw std::runtime_error("Report has no test-validation result");
    return *object;
}

std::vector<double> sample_times(const json::Object& report) {
    const auto* summary = report.getObject("baseline");
    const auto* samples = summary ? summary->getArray("samples") : nullptr;
    if (!samples || samples->size() < 3)
        throw std::runtime_error(
            "At least three samples per revision are required for a regression check");
    const auto count = report.getInteger("runs");
    if (!count || *count != static_cast<std::int64_t>(samples->size()))
        throw std::runtime_error("Report run count does not match its samples");
    std::vector<double> values;
    for (const auto& sample : *samples) {
        const auto* object = sample.getAsObject();
        const auto time = object ? object->getNumber("wall_seconds") : std::nullopt;
        if (!time || !std::isfinite(*time) || *time <= 0)
            throw std::runtime_error("Build sample must have a finite positive wall_seconds value");
        values.push_back(*time);
    }
    return values;
}

json::Object statistics(const std::vector<double>& values) {
    return json::Object{{"median_seconds", median(values)},
                        {"mad_seconds", mad(values)},
                        {"min_seconds", *std::ranges::min_element(values)},
                        {"max_seconds", *std::ranges::max_element(values)},
                        {"sample_count", static_cast<std::int64_t>(values.size())}};
}
}  // namespace

json::Object measurement_context(const Options& options, const fs::path& build) {
    const auto cache = cache_values(build);
    const auto comparable_flags = [&](std::string value) {
        for (const auto& [key, label] : {std::pair{"CMAKE_HOME_DIRECTORY", "<SOURCE>"},
                                         std::pair{"CMAKE_CACHEFILE_DIR", "<BUILD>"}}) {
            const auto root = cache.find(key);
            if (root == cache.end() || root->second.empty()) continue;
            const std::string marker(label);
            std::size_t position = 0;
            while ((position = value.find(root->second, position)) != std::string::npos) {
                value.replace(position, root->second.size(), marker);
                position += marker.size();
            }
        }
        return value;
    };
    json::Object flags;
    for (const auto& [key, value] : cache) {
        if (key.starts_with("CMAKE_CXX_FLAGS") || key.starts_with("CMAKE_C_FLAGS") ||
            key.starts_with("CMAKE_EXE_LINKER_FLAGS") ||
            key.starts_with("CMAKE_SHARED_LINKER_FLAGS") || key == "CMAKE_BUILD_TYPE" ||
            key == "CMAKE_PROJECT_NAME")
            flags[key] = comparable_flags(value);
    }
    json::Array arguments;
    for (const auto& argument : options.cmake_args) arguments.push_back(argument);
    cpu_set_t affinity;
    CPU_ZERO(&affinity);
    const auto processors =
        sched_getaffinity(0, sizeof(affinity), &affinity) == 0 ? CPU_COUNT(&affinity) : 0;
    struct utsname host{};
    if (uname(&host) != 0) throw std::runtime_error("Cannot read kernel/architecture context");
    const fs::path quota = "/sys/fs/cgroup/cpu.max";
    return json::Object{
        {"method", "clean-build-warm-filesystem-v1"},
        {"buildray_version", BUILDRAY_VERSION},
        {"sampling", options.command == "measure" ? "independent" : "paired_alternating"},
        {"session", options.session},
        {"jobs", options.jobs},
        {"cpu_model", cpu_model()},
        {"cpu_affinity_count", processors},
        {"architecture", std::string(host.machine)},
        {"kernel", std::string(host.release)},
        {"cpu_quota", fs::exists(quota) ? read_file(quota) : "unavailable"},
        {"cmake_args", std::move(arguments)},
        {"cache_flags", std::move(flags)},
        {"environment_flags", json::Object{{"CXXFLAGS", environment("CXXFLAGS")},
                                           {"CFLAGS", environment("CFLAGS")},
                                           {"LDFLAGS", environment("LDFLAGS")}}},
        {"tests_required", !options.allow_no_tests},
        {"allow_baseline_skips", options.allow_baseline_skips},
        {"unity_build", false},
        {"precompiled_headers", false}};
}

Comparison compare_reports(const json::Value& baseline, const json::Value& current, double budget) {
    if (!std::isfinite(budget) || budget < 0 || budget > 1000)
        throw std::runtime_error("Regression budget must be between 0 and 1000 percent");
    const auto& before = valid_measurement(baseline);
    const auto& after = valid_measurement(current);
    for (const auto* key : {"compiler_version", "cmake_version", "ninja_version"}) {
        if (!before.getString(key) || !after.getString(key))
            throw std::runtime_error(std::string("Missing tool version string: ") + key);
    }
    for (const auto* key :
         {"compiler_version", "cmake_version", "ninja_version", "measurement_context"}) {
        const auto* left = before.get(key);
        const auto* right = after.get(key);
        if (!left || !right || *left != *right)
            throw std::runtime_error(std::string("Incomparable reports: different or missing ") +
                                     key);
    }
    const auto& context = *before.getObject("measurement_context");
    if (context.getBoolean("allow_baseline_skips").value_or(false) &&
        (!before.getArray("baseline_skipped_tests") || !after.getArray("baseline_skipped_tests") ||
         *before.getArray("baseline_skipped_tests") != *after.getArray("baseline_skipped_tests")))
        throw std::runtime_error("Incomparable reports: different skipped tests");
    if (context.getBoolean("tests_required").value_or(true) &&
        (*before.getInteger("baseline_tests_passed") < 1 ||
         *after.getInteger("baseline_tests_passed") < 1))
        throw std::runtime_error("The configured regression check requires passing project tests");
    const auto before_times = sample_times(before);
    const auto after_times = sample_times(after);
    const double old_time = median(before_times);
    const double new_time = median(after_times);
    const double difference = new_time - old_time;
    const double change = 100.0 * difference / old_time;
    const double noise = 3.0 * std::max(mad(before_times), mad(after_times));
    const double allowance = old_time * budget / 100.0;
    if (!std::isfinite(change) || !std::isfinite(noise) || !std::isfinite(allowance))
        throw std::runtime_error("Measurement statistics exceed the supported numeric range");
    const bool regression = difference > allowance && difference > noise;
    const std::string result = std::abs(difference) <= noise ? "inconclusive"
                               : regression                  ? "regressed"
                               : difference < 0              ? "improved"
                                                             : "within_budget";
    return {json::Object{{"schema_version", 1},
                         {"kind", "comparison"},
                         {"status", "passed"},
                         {"performance_result", result},
                         {"gate_passed", !regression},
                         {"wall_change_percent", change},
                         {"max_regression_percent", budget},
                         {"noise_threshold_seconds", noise},
                         {"budget_seconds", allowance},
                         {"baseline", statistics(before_times)},
                         {"candidate", statistics(after_times)},
                         {"measurement_context", json::Object(context)},
                         {"performance_rule",
                          "regression if slowdown exceeds both configured budget and 3x max MAD; "
                          "heuristic, not a significance test"}},
            regression ? 3 : 0};
}

std::string comparison_markdown(const json::Object& report) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(3) << "### Buildray build-time comparison\n\n";
    if (report.getString("status") == "incomparable") {
        text << "Comparison refused. Check the comparison JSON for the reason.\n";
        return text.str();
    }
    text << "Result: " << report.getString("performance_result")->str() << ".\n\n"
         << "| Clean build | Median (s) | MAD (s) | Samples |\n"
         << "| --- | ---: | ---: | ---: |\n";
    for (const auto* key : {"baseline", "candidate"}) {
        const auto* summary = report.getObject(key);
        text << "| " << key << " | " << *summary->getNumber("median_seconds") << " | "
             << *summary->getNumber("mad_seconds") << " | " << *summary->getInteger("sample_count")
             << " |\n";
    }
    text << "\nWall-time change: " << std::showpos << *report.getNumber("wall_change_percent")
         << std::noshowpos
         << "%. Allowed regression: " << *report.getNumber("max_regression_percent")
         << "%. Noise threshold: " << *report.getNumber("noise_threshold_seconds") << " s.\n\n"
         << "The classification uses a reporting heuristic, not a statistical significance test.\n";
    return text.str();
}

int check_reports(const CheckOptions& options) {
    const auto output = absolute_path(options.output);
    auto markdown = output;
    markdown.replace_extension(".md");
    if (output == markdown)
        throw std::runtime_error("Use a JSON output path distinct from its Markdown companion");
    const auto aliases = [](const fs::path& a, const fs::path& b) {
        return a == b || (fs::exists(a) && fs::exists(b) && fs::equivalent(a, b));
    };
    if (aliases(output, markdown))
        throw std::runtime_error("JSON and Markdown outputs must be separate files");
    for (const auto& input : {options.baseline, options.current}) {
        const auto path = absolute_path(input);
        if (aliases(path, output) || aliases(path, markdown))
            throw std::runtime_error("Comparison output must not overwrite an input report");
    }
    Comparison result;
    try {
        result = compare_reports(read_json(options.baseline), read_json(options.current),
                                 options.max_regression_percent);
    } catch (const std::exception& error) {
        result = {json::Object{{"schema_version", 1},
                               {"kind", "comparison"},
                               {"status", "incomparable"},
                               {"gate_passed", false},
                               {"error", std::string(error.what())}},
                  4};
    }
    write_file(markdown, comparison_markdown(result.report));
    write_json(output, json::Object(result.report));
    if (result.exit_code == 4)
        std::cerr << result.report.getString("error")->str() << '\n';
    else
        std::cout << comparison_markdown(result.report);
    return result.exit_code;
}
}  // namespace buildray
