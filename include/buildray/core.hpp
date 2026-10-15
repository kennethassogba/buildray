#pragma once

#include <llvm/Support/JSON.h>

#include <compare>
#include <cstddef>
#include <filesystem>
#include <string>
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
double median(std::vector<double> values);
double mad(const std::vector<double>& values);

struct Options {
    std::string command;
    fs::path source = ".";
    fs::path work;
    std::string compiler = "clang++";
    std::string tidy = "clang-tidy";
    std::vector<std::string> cmake_args;
    unsigned runs = 3;
    unsigned jobs = 2;
    bool allow_no_tests = false;
};

int pipeline(const Options& options, const fs::path& executable);
int launch(const std::vector<std::string>& args);
}  // namespace buildray
