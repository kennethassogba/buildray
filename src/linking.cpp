#include <unistd.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

#include "buildray/core.hpp"

namespace buildray {
namespace {
std::string text(const json::Object& row, const char* key) {
    const auto value = row.getString(key);
    if (!value) throw std::runtime_error(std::string("Missing link field: ") + key);
    return value->str();
}

std::vector<std::string> arguments(const json::Object& row) {
    const auto* values = row.getArray("command");
    if (!values || values->empty()) throw std::runtime_error("Missing linker argv");
    std::vector<std::string> result;
    for (const auto& value : *values) {
        const auto str = value.getAsString();
        if (!str) throw std::runtime_error("Invalid linker argument");
        result.push_back(str->str());
    }
    return result;
}

std::size_t output_index(const std::vector<std::string>& args) {
    std::size_t found = 0;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "-o") {
            if (found || i + 1 == args.size()) throw std::runtime_error("Ambiguous link output");
            found = i + 1;
        }
    }
    if (!found) throw std::runtime_error("Missing -o in linker driver command");
    return found;
}

std::string normalize(std::string value, const fs::path& source, const fs::path& build) {
    for (const auto& [root, tag] : std::vector<std::pair<std::string, std::string>>{
             {build.string(), "<BUILD>"}, {source.string(), "<SOURCE>"}}) {
        std::size_t pos = 0;
        while ((pos = value.find(root, pos)) != std::string::npos) {
            value.replace(pos, root.size(), tag);
            pos += tag.size();
        }
    }
    return value;
}

std::string baseline_path(std::string value, const fs::path& source, const fs::path& build) {
    for (const auto& [tag, root] : std::vector<std::pair<std::string, std::string>>{
             {"<BUILD>", build.string()}, {"<SOURCE>", source.string()}}) {
        std::size_t pos = 0;
        while ((pos = value.find(tag, pos)) != std::string::npos) {
            value.replace(pos, tag.size(), root);
            pos += root.size();
        }
    }
    return value;
}

std::set<fs::path> active_outputs(const fs::path& build) {
    const auto doc = read_json(build / "active-commands.json");
    const auto* rows = doc.getAsArray();
    if (!rows) throw std::runtime_error("Missing Ninja active commands");
    std::set<fs::path> result;
    for (const auto& value : *rows) {
        const auto* row = value.getAsObject();
        if (!row) throw std::runtime_error("Invalid active command");
        result.insert(absolute_path(fs::path(text(*row, "directory")) / text(*row, "output")));
    }
    return result;
}

using Commands = std::map<std::string, std::vector<std::string>>;
Commands configured_links(const fs::path& source, const fs::path& build, const fs::path& executable) {
    const auto doc = read_json(build / "active-commands.json");
    Commands result;
    for (const auto& value : *doc.getAsArray()) {
        const auto& row = *value.getAsObject();
        const auto tokens = compile_arguments(row);
        const auto marker = std::ranges::find(tokens, "_link-launch");
        if (marker == tokens.end()) continue;
        const auto index = static_cast<std::size_t>(marker - tokens.begin());
        // Parse only the standard CMake Ninja launcher rule; never evaluate shell text.
        if (index != 3 || tokens[0] != ":" || tokens[1] != "&&" ||
            tokens[2] != executable.string() || index + 3 >= tokens.size() || tokens[index + 2] != "--")
            throw std::runtime_error("Unsupported Ninja link rule");
        std::vector<std::string> command;
        for (auto i = index + 3; i < tokens.size() && tokens[i] != "&&"; ++i) {
            if (tokens[i] == ";" || tokens[i] == "||" || tokens[i] == "|")
                throw std::runtime_error("Unsupported shell operator in link rule");
            command.push_back(tokens[i]);
        }
        const auto output = absolute_path(build / command.at(output_index(command)));
        if (output != absolute_path(build / text(row, "output")))
            throw std::runtime_error("Ninja link output differs from driver output");
        for (auto& arg : command) arg = normalize(std::move(arg), source, build);
        if (!result.emplace(normalize(output.string(), source, build), std::move(command)).second)
            throw std::runtime_error("Duplicate active link command");
    }
    return result;
}

std::multiset<std::string> compilations(const fs::path& source, const fs::path& build) {
    const auto doc = read_json(build / "compile_commands.json");
    const auto* rows = doc.getAsArray();
    if (!rows) throw std::runtime_error("Invalid compilation database");
    std::multiset<std::string> result;
    for (const auto& value : *rows) {
        const auto& row = *value.getAsObject();
        std::string identity = normalize(text(row, "directory"), source, build);
        for (auto arg : compile_arguments(row)) {
            if (arg.starts_with('@')) throw std::runtime_error("Compiler response files are not supported by dependency cleanup");
            arg = normalize(std::move(arg), source, build);
            identity += "\n" + std::to_string(arg.size()) + ":" + arg;
        }
        result.insert(std::move(identity));
    }
    return result;
}

void replay(std::vector<std::string> command, const fs::path& cwd, const fs::path& output,
            const fs::path& log, const fs::path& stats = {}) {
    if (std::ranges::any_of(command, [](const std::string& arg) { return arg.starts_with('@'); }))
        throw std::runtime_error("Link response files are not supported by dependency cleanup");
    command[output_index(command)] = output.string();
    // CMake 4.2 requests linker depfiles. Replays must not overwrite Ninja's own file.
    for (std::size_t i = 0; i < command.size(); ++i) {
        auto& arg = command[i];
        if (arg == "--dependency-file") {
            const auto next = i + (i + 1 < command.size() && command[i + 1] == "-Xlinker" ? 2 : 1);
            if (next >= command.size()) throw std::runtime_error("Missing linker dependency-file output");
            command[next] = output.string() + ".d";
        }
        if (arg == "-Wl,--dependency-file")
            throw std::runtime_error("Split -Wl dependency-file arguments are unsupported for link replay");
        for (const auto* prefix : {"-Wl,--dependency-file=", "-Wl,--dependency-file,", "--dependency-file="})
            if (arg.starts_with(prefix)) arg = std::string(prefix) + output.string() + ".d";
        if (arg.find("--print-archive-stats") != std::string::npos ||
            arg.find("--why-extract") != std::string::npos || arg.find("-Map") != std::string::npos ||
            arg.find("--Map") != std::string::npos || arg.find("--save-temps") != std::string::npos ||
            arg.find("--reproduce") != std::string::npos)
            throw std::runtime_error("Link replay does not support this diagnostic output option");
    }
    if (!stats.empty()) command.insert(command.end(), {"-Xlinker", "--print-archive-stats=" + stats.string()});
    const auto status = run(command, cwd, log);
    if (status.code != 0)
        throw std::runtime_error("Link replay failed; unused-archive analysis requires ELF LLD with --print-archive-stats; see " + log.string());
}

std::map<fs::path, std::pair<unsigned long long, unsigned long long>> archive_stats(const fs::path& file, const fs::path& cwd) {
    std::ifstream input(file);
    std::string line;
    if (!std::getline(input, line) || (line != "members\textracted\tarchive" && line != "Members\tExtracted\tArchive name"))
        throw std::runtime_error("Unrecognized LLD archive statistics");
    std::map<fs::path, std::pair<unsigned long long, unsigned long long>> result;
    while (std::getline(input, line)) {
        const auto first = line.find('\t'), second = line.find('\t', first + 1);
        if (first == std::string::npos || second == std::string::npos)
            throw std::runtime_error("Invalid archive statistics row");
        const auto count = [](const std::string& value) {
            if (value.empty() || value.find_first_not_of("0123456789") != std::string::npos)
                throw std::runtime_error("Invalid archive member count");
            return std::stoull(value);
        };
        const auto members = count(line.substr(0, first)), extracted = count(line.substr(first + 1, second - first - 1));
        const auto path = absolute_path(cwd / line.substr(second + 1));
        if (extracted > members) throw std::runtime_error("Invalid archive statistics");
        // Drivers commonly repeat libgcc.a. Every occurrence must have zero extractions.
        auto& entry = result[path];
        entry.first = std::max(entry.first, members);
        entry.second = std::max(entry.second, extracted);
    }
    return result;
}

bool identical_elf(const fs::path& a, const fs::path& b) {
    std::ifstream first(a, std::ios::binary), second(b, std::ios::binary);
    std::array<char, 65536> x{}, y{};
    bool header = false;
    while (first && second) {
        first.read(x.data(), static_cast<std::streamsize>(x.size()));
        second.read(y.data(), static_cast<std::streamsize>(y.size()));
        if (first.gcount() != second.gcount() || !std::equal(x.begin(), x.begin() + first.gcount(), y.begin())) return false;
        if (!header) {
            if (first.gcount() < 4 || std::string(x.data(), 4) != std::string("\177ELF", 4)) return false;
            header = true;
        }
    }
    return header && first.eof() && second.eof();
}
}  // namespace

int launch_link(const std::vector<std::string>& args) {
    if (args.size() < 3 || args[1] != "--") throw std::runtime_error("Link launcher expects RECORD_DIR -- DRIVER [ARGS...]");
    const std::vector<std::string> command(args.begin() + 2, args.end());
    const auto output = output_index(command);
    const auto result = run(command, fs::current_path());
    json::Array argv;
    for (const auto& arg : command) argv.push_back(arg);
    write_json(fs::path(args[0]) / (std::to_string(::getpid()) + ".json"), json::Object{
        {"output", absolute_path(fs::current_path() / command[output]).string()},
        {"directory", fs::current_path().string()}, {"command", std::move(argv)},
        {"wall_seconds", result.seconds}, {"user_seconds", result.user_seconds},
        {"system_seconds", result.system_seconds}, {"peak_rss_kib", result.peak_rss_kib}, {"exit_code", result.code}});
    return result.code;
}

json::Object link_sample(const json::Array& records, const fs::path& source, const fs::path& build) {
    double wall = 0, cpu = 0;
    std::int64_t rss = 0;
    json::Array links;
    for (const auto& value : records) {
        const auto* row = value.getAsObject();
        if (!row || row->getInteger("exit_code") != 0) throw std::runtime_error("Invalid link measurement");
        wall += row->getNumber("wall_seconds").value_or(0);
        cpu += row->getNumber("user_seconds").value_or(0) + row->getNumber("system_seconds").value_or(0);
        rss = std::max(rss, row->getInteger("peak_rss_kib").value_or(0));
        links.push_back(json::Object{{"output", normalize(text(*row, "output"), source, build)},
            {"wall_seconds", *row->getNumber("wall_seconds")}});
    }
    return json::Object{{"linker_invocations", static_cast<std::int64_t>(records.size())},
        {"linker_driver_wall_sum_seconds", wall}, {"linker_driver_cpu_seconds", cpu},
        {"max_linker_driver_rss_kib", rss}, {"linked_outputs", std::move(links)}};
}

void verify_link_coverage(const json::Object& graph, const fs::path& build, const json::Array& records) {
    const auto active = active_outputs(build);
    std::set<fs::path> expected, observed;
    for (const auto& value : *graph.getArray("targets")) {
        const auto& row = *value.getAsObject();
        const auto type = row.getString("type");
        if (type != "EXECUTABLE" && type != "SHARED_LIBRARY" && type != "MODULE_LIBRARY") continue;
        for (const auto& artifact : *row.getArray("artifacts")) {
            const auto name = artifact.getAsString();
            if (!name || !name->starts_with("<BUILD>/")) continue;
            const auto path = absolute_path(build / name->drop_front(8).str());
            if (active.contains(path)) expected.insert(path);
        }
    }
    for (const auto& value : records) {
        const auto& row = *value.getAsObject();
        if (row.getInteger("exit_code") != 0 || !observed.insert(absolute_path(text(row, "output"))).second)
            throw std::runtime_error("Invalid or duplicate link measurement");
    }
    if (expected != observed)
        throw std::runtime_error("Link coverage does not match active CMake executable/shared/module targets; custom linker launchers are unsupported");
}

json::Object profile_link_archives(const json::Object& link, const fs::path& source,
                                   const fs::path& build, const fs::path& analysis) {
    fs::create_directories(analysis);
    const auto binary = analysis.parent_path() / "binaries" / (analysis.filename().string() + ".elf");
    fs::create_directories(binary.parent_path());
    const auto cwd = fs::path(text(link, "directory"));
    replay(arguments(link), cwd, binary, analysis / "baseline.log", analysis / "archives.tsv");
    json::Object archives;
    for (const auto& [path, counts] : archive_stats(analysis / "archives.tsv", cwd))
        archives[normalize(path.string(), source, build)] = json::Object{
            {"members", static_cast<std::int64_t>(counts.first)}, {"extracted", static_cast<std::int64_t>(counts.second)}};
    json::Array command;
    for (const auto& arg : arguments(link)) command.push_back(normalize(arg, source, build));
    return json::Object{{"output", normalize(text(link, "output"), source, build)},
        {"archives", std::move(archives)}, {"baseline_elf", binary.string()},
        {"command", std::move(command)}, {"evidence_directory", analysis.string()}};
}

void cache_link_compilations(const fs::path& source, const fs::path& build, json::Object& profiles) {
    json::Array inventory;
    for (const auto& command : compilations(source, build)) inventory.push_back(command);
    profiles["compiler_inventory"] = std::move(inventory);
}

json::Object verify_unused_link_inputs(const fs::path& bs, const fs::path& bb,
    const fs::path& cs, const fs::path& cb, const json::Array& baseline_links,
    const json::Object& baseline_graph, const fs::path& analysis, const fs::path& executable,
    const json::Object& profiles) {
    std::multiset<std::string> inventory;
    for (const auto& command : *profiles.getArray("compiler_inventory"))
        inventory.insert(command.getAsString()->str());
    if (inventory != compilations(cs, cb))
        throw std::runtime_error("Dependency removal changed compiler commands or usage requirements");
    Commands before;
    std::map<std::string, const json::Object*> original;
    for (const auto& value : baseline_links) {
        const auto& row = *value.getAsObject();
        auto argv = arguments(row);
        for (auto& arg : argv) arg = normalize(std::move(arg), bs, bb);
        const auto key = normalize(text(row, "output"), bs, bb);
        before.emplace(key, std::move(argv));
        original.emplace(key, &row);
    }
    const auto after = configured_links(cs, cb, executable);
    if (before.size() != after.size()) throw std::runtime_error("Dependency removal changed the active link target inventory");
    fs::create_directories(analysis);
    json::Array evidence;
    std::set<std::string> removed_paths;
    unsigned index = 0;
    for (const auto& [output, argv] : before) {
        const auto found = after.find(output);
        if (found == after.end()) throw std::runtime_error("Dependency removal removed a link target");
        if (argv == found->second) continue;
        std::vector<std::string> removed;
        std::size_t current = 0;
        for (const auto& arg : argv) {
            if (current < found->second.size() && arg == found->second[current]) ++current;
            else removed.push_back(arg);
        }
        if (current != found->second.size() || removed.empty() ||
            std::ranges::any_of(removed, [](const std::string& arg) { return arg.starts_with('-') || !arg.ends_with(".a"); }))
            throw std::runtime_error("Link command changed more than removing explicit static archives");
        const auto folder = analysis / std::to_string(index++);
        fs::create_directories(folder);
        struct ReplayCleanup {
            fs::path file;
            ~ReplayCleanup() { std::error_code error; fs::remove(file, error); }
        } replay_cleanup{folder / "candidate.elf"};
        const auto& row = *original.at(output);
        const auto cwd = fs::path(text(row, "directory"));
        const auto* profile = profiles.getObject(output);
        if (!profile || !profile->getObject("archives"))
            throw std::runtime_error("No reusable baseline archive evidence for " + output);
        json::Array baseline_command;
        for (const auto& arg : argv) baseline_command.push_back(arg);
        if (json::Value(std::move(baseline_command)) != *profile->get("command"))
            throw std::runtime_error("Baseline link command changed since archive profiling");
        const auto& stats = *profile->getObject("archives");
        json::Array archives;
        for (const auto& arg : removed) {
            const auto path = absolute_path(cwd / baseline_path(arg, bs, bb));
            const auto* item = stats.getObject(normalize(path.string(), bs, bb));
            if (!item || item->getInteger("extracted") != 0)
                throw std::runtime_error("Removed archive contributed objects or lacks extraction evidence: " + arg);
            archives.push_back(json::Object{{"archive", arg}, {"members", *item->getInteger("members")}, {"extracted", 0}});
            removed_paths.insert(normalize(path.string(), bs, bb));
        }
        auto candidate = found->second;
        for (auto& arg : candidate) arg = baseline_path(std::move(arg), bs, bb);
        replay(std::move(candidate), cwd, folder / "candidate.elf", folder / "candidate.log");
        if (!identical_elf(text(*profile, "baseline_elf"), folder / "candidate.elf"))
            throw std::runtime_error("Dependency removal changed the ELF binary when replaying identical baseline objects");
        evidence.push_back(json::Object{{"output", output}, {"removed_archives", std::move(archives)},
            {"identical_elf", true}, {"baseline_evidence_directory", text(*profile, "evidence_directory")},
            {"evidence_directory", folder.string()}});
        // ReplayCleanup also removes a failed or interrupted replay's executable.
    }
    if (evidence.empty()) throw std::runtime_error("Declaration removal did not remove any actual linker inputs");
    const auto active_compilations = [](const fs::path& source, const fs::path& build) {
        const auto active = active_outputs(build);
        const auto doc = read_json(build / "compile_commands.json");
        std::set<std::string> result;
        for (const auto& value : *doc.getAsArray()) {
            const auto& row = *value.getAsObject();
            const auto output = absolute_path(fs::path(text(row, "directory")) / text(row, "output"));
            if (active.contains(output)) result.insert(normalize(output.string(), source, build));
        }
        return result;
    };
    const auto before_active = active_compilations(bs, bb), after_active = active_compilations(cs, cb);
    std::set<std::string> permitted_targets;
    for (const auto& value : *baseline_graph.getArray("targets")) {
        const auto& row = *value.getAsObject();
        if (row.getString("type") != "STATIC_LIBRARY") continue;
        for (const auto& value_path : *row.getArray("artifacts")) {
            const auto path = value_path.getAsString();
            if (path && removed_paths.contains(path->str())) permitted_targets.insert(text(row, "name"));
        }
    }
    for (const auto& output : after_active)
        if (!before_active.contains(output)) throw std::runtime_error("Dependency removal added an active compilation");
    json::Array omitted;
    for (const auto& output : before_active) {
        if (after_active.contains(output)) continue;
        if (!std::ranges::any_of(permitted_targets, [&](const auto& target) {
                return output.find("/CMakeFiles/" + target + ".dir/") != std::string::npos;
            })) throw std::runtime_error("Dependency removal skipped compilation outside the removed archives");
        omitted.push_back(output);
    }
    return json::Object{{"compiler_commands_unchanged", true}, {"changed_links", std::move(evidence)},
        {"omitted_archive_compilations", std::move(omitted)},
        {"scope", "ELF LLD archive extraction and byte-identical link replay using the same baseline objects; one configured build"}};
}
}  // namespace buildray
