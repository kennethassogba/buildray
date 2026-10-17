#pragma once

#include <llvm/Support/JSON.h>

#include <compare>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace buildray {
namespace fs = std::filesystem;
namespace json = llvm::json;

struct ProcessResult {
    int code{};
    double seconds{};
    double user_seconds{};
    double system_seconds{};
    long peak_rss_kib{};
};

// Runs argv directly, without a shell. The child owns a process group so an
// interrupt reaches the build, its compiler children and test processes.
ProcessResult run(const std::vector<std::string>& args, const fs::path& cwd,
                  const fs::path& log = {});
void install_signal_handlers();
bool interrupted();
std::string read_file(const fs::path& path);
void write_file(const fs::path& path, const std::string& data);
json::Value read_json(const fs::path& path);
void write_json(const fs::path& path, json::Value value);
fs::path absolute_path(const fs::path& path);
bool within(const fs::path& path, const fs::path& parent);

struct Replacement {
    std::string file;
    std::size_t offset{};
    std::size_t length{};
    std::string text;
    auto operator<=>(const Replacement&) const = default;
};

std::vector<Replacement> read_fixes(const fs::path& path);
bool removable_include(const std::string& source, const Replacement& replacement);
std::string apply_removals(std::string source, std::vector<Replacement> replacements);
std::string apply_include_replacements(std::string source, std::vector<Replacement> replacements);
double median(std::vector<double> values);
double mad(const std::vector<double>& values);

struct Options {
    std::string command;
    fs::path source = ".";
    fs::path work;
    fs::path baseline_source;
    std::string compiler = "clang++";
    std::string c_compiler = "clang";
    std::string tidy = "clang-tidy";
    std::vector<std::string> cmake_args;
    unsigned runs = 3;
    unsigned jobs = 2;
    bool allow_no_tests = false;
    bool fail_on_cycles = false;
    bool auto_pch = false;
    double max_regression_percent = 5.0;
    std::string session;
    std::string target;
    std::vector<std::string> headers;
    std::vector<fs::path> exclusions;
    std::string dependency;
    bool forward_declarations = false;
    unsigned max_header_edits = 8;
    std::vector<fs::path> header_paths;
    unsigned batch_size = 4;
    std::vector<fs::path> unity_exclusions;
};

struct CheckOptions {
    fs::path baseline;
    fs::path current;
    fs::path output = "buildray-comparison.json";
    double max_regression_percent = 5.0;
};

struct Comparison {
    json::Object report;
    int exit_code{};
};

json::Object measurement_context(const Options& options, const fs::path& build);
Comparison compare_reports(const json::Value& baseline, const json::Value& current, double budget);
std::string comparison_markdown(const json::Object& report);
int check_reports(const CheckOptions& options);
int pipeline(const Options& options, const fs::path& executable);
int launch(const std::vector<std::string>& args, bool trace = false, bool includes = false);
// Returns a trace line's depth and path; a zero depth means ordinary diagnostics.
std::pair<std::size_t, std::string> include_line(const std::string& line);
json::Object dependency_graph(const fs::path& records, const fs::path& source,
                              const fs::path& build, const std::vector<fs::path>& originals);
std::string dependencies_markdown(const json::Object& graph);
std::string dependencies_dot(const json::Object& graph);
json::Object cmake_dependencies(const fs::path& source, const fs::path& build);
json::Object link_candidates(const json::Object& graph, const fs::path& source,
                             const Options& options);
void verify_link_removal(const json::Object& before, const json::Object& after,
                         const json::Object& selection);
json::Object header_profile(const fs::path& records, const fs::path& source,
                            const fs::path& build);
std::string profile_markdown(const json::Object& profile);
json::Object select_pch(const json::Object& profile, const fs::path& source,
                        const std::string& target = "");
std::string pch_cmake(const Options& options, const fs::path& source);
std::string unity_cmake(const Options& options, const fs::path& source);
json::Object verify_unity_plan(const fs::path& baseline_source, const fs::path& baseline_build,
                              const fs::path& candidate_source, const fs::path& candidate_build,
                              const Options& options);
std::vector<std::string> compile_arguments(const json::Object& entry);
std::vector<Replacement> analyze_headers(const Options& options, const fs::path& source,
    const fs::path& build, const fs::path& work, const fs::path& executable,
    const std::map<fs::path, std::string>& originals, json::Object& report);
}  // namespace buildray
