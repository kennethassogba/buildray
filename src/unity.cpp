#include "buildray/core.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace buildray {
namespace {
bool safe(const std::string& value, bool path = false) {
    return !value.empty() && std::ranges::all_of(value, [path](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+' ||
               (path && (c == '/' || c == ' '));
    });
}

bool compiled(const fs::path& file) {
    const auto ext = file.extension();
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx" ||
           ext == ".C" || ext == ".c++" || ext == ".cp";
}

std::string source_key(const fs::path& path, const fs::path& source, const fs::path& build) {
    if (within(path, source)) return "<SOURCE>/" + path.lexically_relative(source).generic_string();
    if (within(path, build)) return "<BUILD>/" + path.lexically_relative(build).generic_string();
    throw std::runtime_error("Unity input is outside the isolated source/build trees: " + path.string());
}

struct Plan {
    std::multiset<std::pair<std::string, std::string>> originals;
    std::map<std::string, std::size_t> target_counts;
    json::Array groups;
    std::size_t invocations{};
};

Plan plan(const fs::path& source, const fs::path& build, const Options& options, bool candidate) {
    std::set<fs::path> active;
    const auto active_document = read_json(build / "active-commands.json");
    const auto* active_commands = active_document.getAsArray();
    if (!active_commands) throw std::runtime_error("Missing Ninja default-target commands");
    for (const auto& entry : *active_commands) {
        const auto* command = entry.getAsObject();
        if (!command || !command->getString("output") || !command->getString("directory"))
            throw std::runtime_error("Malformed Ninja default-target command");
        active.insert(absolute_path(fs::path(command->getString("directory")->str()) /
                                    command->getString("output")->str()));
    }
    const auto document = read_json(build / "compile_commands.json");
    const auto* commands = document.getAsArray();
    if (!commands) throw std::runtime_error("Missing unity compilation database");
    Plan result;
    for (const auto& value : *commands) {
        if (interrupted()) throw std::runtime_error("Interrupted");
        const auto* entry = value.getAsObject();
        if (!entry || !entry->getString("file") || !entry->getString("directory") ||
            !entry->getString("output")) throw std::runtime_error("Incomplete unity compile command");
        const fs::path directory = entry->getString("directory")->str();
        const auto file = absolute_path(directory / entry->getString("file")->str());
        const auto output = absolute_path(directory / entry->getString("output")->str());
        if (!compiled(file) || !active.contains(output)) continue;
        if (!within(output, build)) throw std::runtime_error("Unity object outside the build tree");
        const auto relative = output.lexically_relative(build).generic_string();
        const auto marker = relative.find("CMakeFiles/");
        const auto end = relative.find(".dir/", marker);
        if (marker == std::string::npos || end == std::string::npos)
            throw std::runtime_error("Cannot identify target for unity coverage");
        const auto target = relative.substr(marker + 11, end - marker - 11);
        const auto target_key = relative.substr(0, end + 4);
        ++result.invocations;
        const bool unity = within(file, build) && file.parent_path().filename() == "Unity" &&
                           file.filename().string().starts_with("unity_");
        if (!unity) {
            result.originals.emplace(target_key, source_key(file, source, build));
            ++result.target_counts[target];
            continue;
        }
        if (!candidate || target != options.target ||
            file.parent_path().parent_path().filename() != target + ".dir")
            throw std::runtime_error("Unity compilation outside the selected candidate target");
        json::Array sources;
        std::istringstream lines(read_file(file));
        std::string line;
        while (std::getline(lines, line)) {
            if (!line.starts_with("#include ")) continue;
            const auto include = line.substr(9);
            if (include.size() < 3 || include.front() != '"' || include.back() != '"' ||
                include.find('"', 1) != include.size() - 1 || include.find('\\') != std::string::npos)
                throw std::runtime_error("Unsupported generated unity include: " + line);
            const auto original = absolute_path(file.parent_path() / include.substr(1, include.size() - 2));
            if (!compiled(original) || !fs::is_regular_file(original))
                throw std::runtime_error("Invalid generated unity source include");
            const auto key = source_key(original, source, build);
            result.originals.emplace(target_key, key);
            ++result.target_counts[target];
            sources.push_back(key);
        }
        if (sources.empty() || sources.size() > options.batch_size)
            throw std::runtime_error("Generated unity batch does not match the requested size");
        result.groups.push_back(json::Object{{"file", source_key(file, source, build)},
                                             {"sources", std::move(sources)}});
    }
    return result;
}
}  // namespace

std::string unity_cmake(const Options& options, const fs::path& source) {
    if (!safe(options.target)) throw std::runtime_error("Unsupported unity target name");
    if (options.batch_size < 2 || options.batch_size > 32)
        throw std::runtime_error("Unity batch size must be between 2 and 32");
    std::string exclusions;
    std::set<fs::path> unique;
    std::set<fs::path> files;
    for (const auto& path : options.unity_exclusions) {
        if (!safe(path.generic_string(), true) || path.is_absolute() ||
            !within(absolute_path(source / path), absolute_path(source)) ||
            !fs::exists(source / path) || !unique.insert(path).second)
            throw std::runtime_error("Unity exclusions must be distinct source paths inside the project");
        bool supported = false;
        const auto add = [&](const fs::path& file) {
            if (!compiled(file)) return;
            const auto relative = file.lexically_relative(source);
            if (!safe(relative.generic_string(), true) ||
                !within(absolute_path(file), absolute_path(source)))
                throw std::runtime_error("Unsupported unity exclusion file: " + file.string());
            files.insert(relative);
            supported = true;
        };
        if (fs::is_directory(source / path)) {
            for (auto it = fs::recursive_directory_iterator(source / path);
                 it != fs::recursive_directory_iterator(); ++it) {
                if (interrupted()) throw std::runtime_error("Interrupted");
                const auto name = it->path().filename();
                if (it->is_directory() && (name == ".git" || name == ".cache" ||
                    name == ".buildray" || fs::exists(it->path() / "CMakeCache.txt"))) {
                    // These directories are absent from the source snapshots too.
                    it.disable_recursion_pending();
                    continue;
                }
                if (it->is_regular_file()) add(it->path());
            }
        } else if (fs::is_regular_file(source / path)) add(source / path);
        if (!supported) throw std::runtime_error("Unity exclusion contains no supported source files");
    }
    for (const auto& path : files) {
        exclusions += "  set_source_files_properties(\"${CMAKE_CURRENT_SOURCE_DIR}/" +
                      path.generic_string() + "\" TARGET_DIRECTORY " + options.target +
                      " PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)\n";
    }
    const auto& target = options.target;
    return "\n# Buildray: opt-in unity experiment, validated for the measured configuration.\n"
           "option(BUILDRAY_UNITY \"Enable the measured unity build on " + target + "\" OFF)\n"
           "if(BUILDRAY_UNITY)\n"
           "  if(NOT TARGET " + target + ")\n    message(FATAL_ERROR \"Unity target does not exist\")\n  endif()\n"
           "  get_target_property(_buildray_unity_type " + target + " TYPE)\n"
           "  get_target_property(_buildray_unity_imported " + target + " IMPORTED)\n"
           "  get_target_property(_buildray_unity_alias " + target + " ALIASED_TARGET)\n"
           "  if(_buildray_unity_imported OR _buildray_unity_alias OR NOT _buildray_unity_type\n"
           "     MATCHES \"^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$\")\n"
           "    message(FATAL_ERROR \"Unity requires a local compiled target\")\n  endif()\n" +
           exclusions + "  set_target_properties(" + target + " PROPERTIES\n"
           "    UNITY_BUILD ON UNITY_BUILD_MODE BATCH UNITY_BUILD_BATCH_SIZE " +
           std::to_string(options.batch_size) + ")\nendif()\n";
}

json::Object verify_unity_plan(const fs::path& baseline_source, const fs::path& baseline_build,
                              const fs::path& candidate_source, const fs::path& candidate_build,
                              const Options& options) {
    const auto before = plan(baseline_source, baseline_build, options, false);
    auto after = plan(candidate_source, candidate_build, options, true);
    if (before.originals != after.originals)
        throw std::runtime_error("Unity changed source/target coverage; original compilations must match with multiplicity");
    const auto count = before.target_counts.find(options.target);
    if (count == before.target_counts.end() || count->second < 2 || after.groups.empty() ||
        after.invocations >= before.invocations)
        throw std::runtime_error("Selected target did not combine at least two active source compilations");
    return json::Object{{"target", options.target}, {"batch_size", options.batch_size},
        {"baseline_compiler_invocations", static_cast<std::int64_t>(before.invocations)},
        {"candidate_compiler_invocations", static_cast<std::int64_t>(after.invocations)},
        {"original_compilations", static_cast<std::int64_t>(before.originals.size())},
        {"selected_target_sources", static_cast<std::int64_t>(count->second)},
        {"source_target_coverage_verified", true}, {"groups", std::move(after.groups)},
        {"scope", "one configured Ninja default all target; C/C++ commands; original source/target multiset preserved; generated unity groups only on the selected target"}};
}
}  // namespace buildray
