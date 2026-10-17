#include <algorithm>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>

#include "buildray/core.hpp"

namespace buildray {
namespace {
bool cpp(const fs::path& file) {
    const auto ext = file.extension();
    return ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".C" || ext == ".c++" ||
           ext == ".cp";
}
fs::path command_file(const json::Object& entry) {
    auto path = fs::path(entry.getString("file")->str());
    if (path.is_relative()) path = fs::path(entry.getString("directory")->str()) / path;
    return absolute_path(path);
}
json::Object header_command(const json::Object& entry, const fs::path& header) {
    const auto args = compile_arguments(entry);
    std::vector<std::string> adjusted{args.front()};
    const auto original = command_file(entry);
    unsigned inputs = 0;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const auto& arg = args[i];
        if (arg == "-o" || arg == "-MF" || arg == "-MT" || arg == "-MQ" || arg == "-x") {
            if (++i == args.size()) throw std::runtime_error("Missing compiler option value");
            continue;
        }
        if (arg == "-c" || arg == "-MD" || arg == "-MMD" || arg == "-MP" || arg == "-M" ||
            arg == "-MM")
            continue;
        if (arg.starts_with("-o") || arg.starts_with("-MF") || arg.starts_with("-MT") ||
            arg.starts_with("-MQ"))
            continue;
        if (!arg.starts_with('-') &&
            absolute_path(fs::path(arg).is_absolute()
                              ? fs::path(arg)
                              : fs::path(entry.getString("directory")->str()) / arg) == original) {
            ++inputs;
            continue;
        }
        adjusted.push_back(arg);
    }
    if (inputs != 1)
        throw std::runtime_error("Cannot identify one compiler input for header validation");
    adjusted.insert(adjusted.end(), {"-x", "c++-header", "-fsyntax-only", header.string()});
    json::Array arguments;
    for (const auto& arg : adjusted) arguments.push_back(arg);
    return json::Object{{"file", header.string()},
                        {"directory", entry.getString("directory")->str()},
                        {"arguments", std::move(arguments)}};
}
struct Header {
    std::set<std::size_t> users;
    std::map<std::pair<std::size_t, std::size_t>, std::set<std::string>> forwards;
};
}  // namespace

std::vector<Replacement> analyze_headers(const Options& options, const fs::path& source,
                                         const fs::path& build, const fs::path& work,
                                         const fs::path& executable,
                                         const std::map<fs::path, std::string>& originals,
                                         json::Object& report) {
    const auto helper = executable.parent_path() / "buildray-header-tool";
    const auto logs = work / "logs";
    const auto checked = [&](const std::vector<std::string>& args, const fs::path& cwd,
                             const fs::path& log) {
        const auto result = run(args, cwd, log);
        if (result.code != 0)
            throw std::runtime_error("Header analysis failed (exit " + std::to_string(result.code) +
                                     "); see " + log.string());
    };
    checked({helper.string(), "--version"}, source, logs / "header-tool-version.json");
    const auto helper_version = read_json(logs / "header-tool-version.json");
    std::smatch version;
    const auto compiler_version = report.getString("compiler_version")->str();
    if (!std::regex_search(compiler_version, version, std::regex(R"(clang version ([0-9]+)\.)")) ||
        !helper_version.getAsObject() ||
        helper_version.getAsObject()->getInteger("clang_major") != std::stoi(version[1]))
        throw std::runtime_error(
            "Header AST helper and project compiler must have the same Clang major version");
    checked({options.compiler, "-print-resource-dir"}, source, logs / "header-resource-dir.log");
    auto resources = read_file(logs / "header-resource-dir.log");
    while (!resources.empty() && (resources.back() == '\n' || resources.back() == '\r'))
        resources.pop_back();
    const auto document = read_json(build / "compile_commands.json");
    const auto* entries = document.getAsArray();
    if (!entries || entries->empty()) throw std::runtime_error("Empty compilation database");
    std::map<fs::path, Header> headers;
    json::Array skipped, rejected, findings;
    unsigned scan_count = 0, standalone = 0, preflight = 0, tidy_count = 0;
    const auto editable = [&](const fs::path& file) {
        if (!within(file, source) || !fs::is_regular_file(file)) return false;
        const auto relative = file.lexically_relative(source);
        return originals.contains(relative) &&
               !std::ranges::any_of(options.exclusions, [&](const fs::path& excluded) {
                   return within(relative, excluded);
               });
    };
    std::cout << "Indexing header users and record types with Clang AST...\n" << std::flush;
    for (std::size_t index = 0; index < entries->size(); ++index) {
        if (interrupted()) throw std::runtime_error("Interrupted during header indexing");
        const auto* entry = (*entries)[index].getAsObject();
        if (!entry || !entry->getString("file") || !entry->getString("directory"))
            throw std::runtime_error("Invalid compilation command");
        const auto input = command_file(*entry);
        if (!cpp(input) && input.extension() != ".c") continue;
        const auto directory = work / "header-analysis/index" / std::to_string(index);
        fs::create_directories(directory);
        write_json(directory / "compile_commands.json", json::Array{(*entries)[index]});
        checked({helper.string(), (directory / "compile_commands.json").string(), input.string(),
                 source.string(), (directory / "facts.json").string(), resources},
                source, logs / ("header-scan-" + std::to_string(index) + ".log"));
        ++scan_count;
        const auto facts_doc = read_json(directory / "facts.json");
        const auto* facts = facts_doc.getAsObject();
        if (!facts || !facts->getArray("headers") || !facts->getArray("forwards"))
            throw std::runtime_error("Invalid header AST facts");
        for (const auto& value : *facts->getArray("headers")) {
            if (!value.getAsString()) throw std::runtime_error("Invalid observed header");
            const auto file = absolute_path(value.getAsString()->str());
            if (editable(file)) headers[file].users.insert(index);
        }
        if (!options.forward_declarations) continue;
        for (const auto& value : *facts->getArray("forwards")) {
            const auto* row = value.getAsObject();
            if (!row || !row->getString("header") || !row->getString("provider") ||
                !row->getInteger("offset") || !row->getInteger("length") ||
                !row->getArray("declarations"))
                throw std::runtime_error("Invalid forward-declaration facts");
            const auto file = absolute_path(row->getString("header")->str());
            if (!editable(file) || !editable(absolute_path(row->getString("provider")->str())) ||
                *row->getInteger("offset") < 0 || *row->getInteger("length") < 1)
                continue;
            auto& group =
                headers[file].forwards[{static_cast<std::size_t>(*row->getInteger("offset")),
                                        static_cast<std::size_t>(*row->getInteger("length"))}];
            for (const auto& declaration : *row->getArray("declarations")) {
                if (!declaration.getAsString())
                    throw std::runtime_error("Invalid record declaration");
                group.insert(declaration.getAsString()->str());
            }
        }
    }
    std::vector<fs::path> order;
    for (const auto& [file, data] : headers) order.push_back(file);
    std::ranges::sort(order, [&](const auto& a, const auto& b) {
        return headers.at(a).users.size() != headers.at(b).users.size()
                   ? headers.at(a).users.size() > headers.at(b).users.size()
                   : a < b;
    });
    std::vector<Replacement> result;
    std::map<fs::path, std::vector<json::Object>> accepted_contexts;
    std::map<fs::path, std::string> saved;
    const auto restore = [&] {
        for (const auto& [file, original] : saved) write_file(file, original);
    };
    try {
        for (const auto& file : order) {
            if (interrupted()) throw std::runtime_error("Interrupted during header cleanup");
            if (saved.size() >= options.max_header_edits) break;
            const auto relative = file.lexically_relative(source);
            if (!options.header_paths.empty() &&
                !std::ranges::any_of(options.header_paths,
                                     [&](const fs::path& path) { return within(relative, path); }))
                continue;
            const auto original = read_file(file);
            if (original != originals.at(relative)) {
                skipped.push_back(
                    json::Object{{"file", relative.generic_string()},
                                 {"reason", "configuration rewrote the input header"}});
                continue;
            }
            if (relative.string().find_first_of("\n\r\t") != std::string::npos)
                throw std::runtime_error("Control characters in header patch paths");
            std::vector<json::Object> contexts;
            std::set<std::vector<std::string>> signatures;
            for (const auto index : headers.at(file).users) {
                const auto& entry = *(*entries)[index].getAsObject();
                if (!cpp(command_file(entry))) continue;
                auto context = header_command(entry, file);
                auto signature = compile_arguments(context);
                signature.insert(signature.begin(), context.getString("directory")->str());
                if (signatures.insert(std::move(signature)).second)
                    contexts.push_back(std::move(context));
            }
            if (contexts.empty()) continue;
            bool self_contained = true;
            for (const auto& context : contexts) {
                const auto log =
                    logs / ("header-standalone-" + std::to_string(standalone++) + ".log");
                if (run(compile_arguments(context), context.getString("directory")->str(), log)
                        .code != 0) {
                    skipped.push_back(json::Object{
                        {"file", relative.generic_string()},
                        {"reason", "original header is not standalone in an observed context"},
                        {"log", log.lexically_relative(work).generic_string()}});
                    self_contained = false;
                    break;
                }
            }
            if (!self_contained) continue;
            std::optional<std::set<Replacement>> consensus;
            for (const auto& context : contexts) {
                const auto directory = work / "header-analysis/tidy" / std::to_string(tidy_count++);
                fs::create_directories(directory);
                write_json(directory / "compile_commands.json", json::Array{json::Object(context)});
                const auto fixes = directory / "fixes.yaml";
                checked({options.tidy,
                         "--config={\"Checks\":\"-*,misc-include-cleaner\",\"WarningsAsErrors\":"
                         "\"\",\"CheckOptions\":{\"misc-include-cleaner.MissingIncludes\":\"true\","
                         "\"misc-include-cleaner.UnusedIncludes\":\"true\"}}",
                         "--quiet", "-p", directory.string(), "--export-fixes=" + fixes.string(),
                         file.string()},
                        source, logs / ("header-tidy-" + std::to_string(tidy_count) + ".log"));
                std::set<Replacement> removals;
                for (auto item : read_fixes(fixes)) {
                    auto path = fs::path(item.file);
                    if (path.is_relative())
                        path = fs::path(context.getString("directory")->str()) / path;
                    if (absolute_path(path) != file || !removable_include(original, item)) continue;
                    item.file = relative.generic_string();
                    removals.insert(std::move(item));
                }
                if (!consensus)
                    consensus = std::move(removals);
                else {
                    std::set<Replacement> common;
                    std::set_intersection(consensus->begin(), consensus->end(), removals.begin(),
                                          removals.end(), std::inserter(common, common.end()));
                    consensus = std::move(common);
                }
            }
            std::vector<Replacement> accepted;
            const auto attempt = [&](const std::vector<Replacement>& proposals, const char* kind) {
                const auto before = read_file(file);
                write_file(file, apply_include_replacements(original, proposals));
                fs::path failure;
                try {
                    for (const auto& context : contexts) {
                        const auto log =
                            logs / ("header-preflight-" + std::to_string(preflight++) + ".log");
                        if (run(compile_arguments(context), context.getString("directory")->str(),
                                log)
                                .code != 0) {
                            failure = log;
                            break;
                        }
                    }
                    if (failure.empty())
                        for (const auto index : headers.at(file).users) {
                            const auto& context = *(*entries)[index].getAsObject();
                            const auto log =
                                logs / ("header-preflight-" + std::to_string(preflight++) + ".log");
                            if (run(compile_arguments(context),
                                    context.getString("directory")->str(), log)
                                    .code != 0) {
                                failure = log;
                                break;
                            }
                        }
                    if (interrupted())
                        throw std::runtime_error("Interrupted during header preflight");
                } catch (...) {
                    write_file(file, before);
                    throw;
                }
                if (!failure.empty()) {
                    write_file(file, before);
                    rejected.push_back(json::Object{
                        {"file", relative.generic_string()},
                        {"kind", kind},
                        {"reason", "standalone header or observed user failed compilation"},
                        {"log", failure.lexically_relative(work).generic_string()}});
                    return false;
                }
                saved.try_emplace(file, original);
                return true;
            };
            if (consensus && !consensus->empty()) {
                const std::vector<Replacement> proposed(consensus->begin(), consensus->end());
                if (attempt(proposed, "unused_includes")) accepted = proposed;
            }
            if (options.forward_declarations)
                for (const auto& [range, declarations] : headers.at(file).forwards) {
                    if (std::ranges::any_of(
                            accepted, [&](const auto& item) { return item.offset == range.first; }))
                        continue;
                    Replacement item{relative.generic_string(), range.first, range.second, ""};
                    if (!removable_include(original, item) ||
                        original.substr(item.offset, item.length).find("NOLINT") !=
                            std::string::npos)
                        continue;
                    for (const auto& declaration : declarations) item.text += declaration + '\n';
                    if (item.text.empty()) continue;
                    auto proposed = accepted;
                    proposed.push_back(std::move(item));
                    if (attempt(proposed, "forward_declarations")) {
                        accepted = std::move(proposed);
                        break;
                    }
                }
            if (accepted.empty()) continue;
            saved[file] = original;
            accepted_contexts[file] = contexts;
            for (auto& item : accepted) {
                findings.push_back(json::Object{
                    {"file", relative.generic_string()},
                    {"offset", static_cast<std::int64_t>(item.offset)},
                    {"directive", original.substr(item.offset, item.length)},
                    {"replacement", item.text},
                    {"kind", item.text.empty() ? "unused_include" : "forward_declaration"},
                    {"observed_users", static_cast<std::int64_t>(headers.at(file).users.size())},
                    {"standalone_contexts", static_cast<std::int64_t>(contexts.size())}});
                result.push_back(std::move(item));
            }
        }
        // Later header edits must not invalidate earlier standalone checks.
        for (const auto& [file, contexts] : accepted_contexts)
            for (const auto& context : contexts)
                checked(compile_arguments(context), context.getString("directory")->str(),
                        logs / ("header-final-" + std::to_string(standalone++) + ".log"));
        restore();
    } catch (...) {
        restore();
        throw;
    }
    unsigned unused = 0, forwarded = 0, declarations = 0;
    for (const auto& item : result) {
        if (item.text.empty())
            ++unused;
        else {
            ++forwarded;
            declarations +=
                static_cast<unsigned>(std::count(item.text.begin(), item.text.end(), ';'));
        }
    }
    json::Array selected_paths;
    for (const auto& path : options.header_paths) selected_paths.push_back(path.generic_string());
    json::Object summary{{"observed_headers", static_cast<std::int64_t>(headers.size())},
                         {"ast_commands", scan_count},
                         {"standalone_checks", standalone},
                         {"preflight_commands", preflight},
                         {"tidy_commands", tidy_count},
                         {"modified_headers", static_cast<std::int64_t>(saved.size())},
                         {"max_header_edits", options.max_header_edits},
                         {"forward_declarations_enabled", options.forward_declarations},
                         {"unused_includes", unused},
                         {"forwarded_includes", forwarded},
                         {"added_forward_declarations", declarations},
                         {"header_paths", std::move(selected_paths)},
                         {"budget_reached", saved.size() >= options.max_header_edits},
                         {"selection_order", "descending observed user count, then path"},
                         {"findings", std::move(findings)},
                         {"skipped", std::move(skipped)},
                         {"rejected", std::move(rejected)},
                         {"scope",
                          "observed original project headers; standalone checks use observed C++ "
                          "flags; all observed C/C++ users are preflighted; one accepted forward "
                          "include per header; full build and CTest still required"}};
    report["modified_headers"] = static_cast<std::int64_t>(saved.size());
    report["header_changes"] = static_cast<std::int64_t>(result.size());
    report["header_unused_includes"] = unused;
    report["forwarded_includes"] = forwarded;
    report["added_forward_declarations"] = declarations;
    report["header_cleanup"] = json::Object(summary);
    write_json(work / "header-cleanup.json", std::move(summary));
    return result;
}
}  // namespace buildray
