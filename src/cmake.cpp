#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

#include "buildray/core.hpp"

namespace buildray {
namespace {
using Edges = std::map<std::string, std::set<std::string>>;
using Relation = std::tuple<std::string, std::string, std::string>;

std::string string(const json::Object& object, const char* key) {
    const auto value = object.getString(key);
    if (!value) throw std::runtime_error(std::string("Missing CMake field: ") + key);
    return value->str();
}

const json::Array& array(const json::Object& object, const char* key) {
    const auto* value = object.getArray(key);
    if (!value) throw std::runtime_error(std::string("Missing CMake array: ") + key);
    return *value;
}

bool name(const std::string& value) {
    return !value.empty() && std::ranges::all_of(value, [](unsigned char c) {
        return std::isalnum(c) || c == '_' || c == '.' || c == ':' || c == '+' || c == '-';
    });
}

std::set<Relation> relations(const json::Object& graph) {
    std::set<Relation> result;
    for (const auto& value : array(graph, "edges")) {
        const auto* row = value.getAsObject();
        if (!row) throw std::runtime_error("Invalid CMake relationship");
        result.emplace(string(*row, "from"), string(*row, "to"), string(*row, "kind"));
    }
    return result;
}

Edges edges(const std::set<Relation>& all, const std::string& kind) {
    Edges result;
    for (const auto& [from, to, type] : all)
        if (type == kind) result[from].insert(to);
    return result;
}

bool has(const Edges& graph, const std::string& from, const std::string& to) {
    const auto it = graph.find(from);
    return it != graph.end() && it->second.contains(to);
}

// Both linking and compilation usage requirements must survive each hop.
// LINK_ONLY and COMPILE_ONLY paths alone are deliberately insufficient.
std::vector<std::string> alternate(const Edges& direct, const Edges& compile,
                                   const Edges& interface, const Edges& interface_compile,
                                   const std::string& from, const std::string& to) {
    std::queue<std::vector<std::string>> pending;
    std::set<std::string> visited{from};
    if (const auto it = direct.find(from); it != direct.end())
        for (const auto& next : it->second)
            if (next != to && next != from && has(compile, from, next)) {
                pending.push({from, next});
                visited.insert(next);
            }
    while (!pending.empty()) {
        auto path = std::move(pending.front());
        pending.pop();
        const auto it = interface.find(path.back());
        if (it == interface.end()) continue;
        for (const auto& next : it->second) {
            if (!has(interface_compile, path.back(), next) || visited.contains(next)) continue;
            auto extended = path;
            extended.push_back(next);
            if (next == to) return extended;
            visited.insert(next);
            pending.push(std::move(extended));
        }
    }
    return {};
}

struct Token {
    std::string text;
    std::size_t offset;
    std::size_t line;
    bool literal;
};

// A deliberately limited CMake lexer. Comments and quoted/bracket arguments are
// consumed whole so command-looking text inside them cannot become a patch.
std::vector<Token> tokens(const std::string& source) {
    std::vector<Token> result;
    std::size_t i = 0, line = 1;
    const auto advance = [&](std::size_t end) {
        while (i < end)
            if (source[i++] == '\n') ++line;
    };
    const auto bracket = [&](std::size_t start) -> std::optional<std::size_t> {
        if (start >= source.size() || source[start] != '[') return std::nullopt;
        auto pos = start + 1;
        while (pos < source.size() && source[pos] == '=') ++pos;
        if (pos == source.size() || source[pos] != '[') return std::nullopt;
        const auto close = "]" + source.substr(start + 1, pos - start - 1) + "]";
        const auto end = source.find(close, pos + 1);
        if (end == std::string::npos)
            throw std::runtime_error("Unterminated CMake bracket argument");
        return end + close.size();
    };
    while (i < source.size()) {
        if (std::isspace(static_cast<unsigned char>(source[i]))) {
            advance(i + 1);
            continue;
        }
        if (source[i] == '#') {
            if (const auto end = bracket(i + 1))
                advance(*end);
            else {
                const auto comment_end = source.find('\n', i);
                advance(comment_end == std::string::npos ? source.size() : comment_end);
            }
            continue;
        }
        const auto start = i, start_line = line;
        if (const auto end = bracket(i)) {
            advance(*end);
            result.push_back({source.substr(start, i - start), start, start_line, false});
        } else if (source[i] == '"') {
            advance(i + 1);
            bool closed = false;
            while (i < source.size()) {
                if (source[i] == '\\' && i + 1 < source.size())
                    advance(i + 2);
                else if (source[i] == '"') {
                    advance(i + 1);
                    closed = true;
                    break;
                } else
                    advance(i + 1);
            }
            if (!closed) throw std::runtime_error("Unterminated CMake quoted argument");
            result.push_back({source.substr(start, i - start), start, start_line, false});
        } else if (source[i] == '(' || source[i] == ')') {
            advance(i + 1);
            result.push_back({source.substr(start, 1), start, start_line, true});
        } else {
            while (i < source.size() && !std::isspace(static_cast<unsigned char>(source[i])) &&
                   source[i] != '(' && source[i] != ')') {
                if (source[i] == '\\' && i + 1 < source.size())
                    advance(i + 2);
                else
                    advance(i + 1);
            }
            const auto text = source.substr(start, i - start);
            result.push_back({text, start, start_line, name(text)});
        }
    }
    return result;
}

std::optional<Token> literal_link(const std::string& source, const std::string& target,
                                  const std::string& dependency, std::size_t line,
                                  bool private_only = false, bool whole_command = false) {
    const auto all = tokens(source);
    for (std::size_t i = 0; i + 1 < all.size(); ++i) {
        auto command = all[i].text;
        std::ranges::transform(command, command.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!all[i].literal || all[i].line != line || command != "target_link_libraries" ||
            all[i + 1].text != "(")
            continue;
        const auto start = i + 2;
        auto end = start;
        while (end < all.size() && all[end].text != ")") ++end;
        if (end == all.size() || start == end || all[start].text != target) return std::nullopt;
        std::optional<Token> selected;
        std::string scope;
        for (auto j = start; j < end; ++j) {
            const auto& token = all[j];
            if (!token.literal || token.text == "(" || token.text == "debug" ||
                token.text == "optimized" || token.text == "general" ||
                token.text == "LINK_PUBLIC" || token.text == "LINK_PRIVATE" ||
                token.text == "LINK_INTERFACE_LIBRARIES")
                return std::nullopt;
            if (j == start) continue;
            if (token.text == "PRIVATE" || token.text == "PUBLIC" || token.text == "INTERFACE")
                scope = token.text;
            else if (token.text == dependency) {
                if (selected || scope == "INTERFACE" || (private_only && !scope.empty() && scope != "PRIVATE")) return std::nullopt;
                selected = token;
            }
        }
        if (selected && whole_command)
            return Token{source.substr(all[i].offset, all[end].offset + 1 - all[i].offset),
                         all[i].offset, all[i].line, true};
        return selected;
    }
    return std::nullopt;
}

using Location = std::pair<std::string, std::size_t>;
using Trace = std::map<Location, std::vector<json::Array>>;

Trace link_trace(const fs::path& path, const fs::path& source) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Missing expanded CMake trace: " + path.string());
    Trace result;
    std::string line;
    while (std::getline(input, line)) {
        auto value = json::parse(line);
        if (!value) throw std::runtime_error("Invalid expanded CMake trace: " + llvm::toString(value.takeError()));
        const auto* row = value->getAsObject();
        if (!row) continue;
        const auto command = row->getString("cmd");
        if (!command || !command->equals_insensitive("target_link_libraries")) continue;
        const auto file = row->getString("file");
        const auto number = row->getInteger("line");
        const auto* args = row->getArray("args");
        if (!file || !number || *number < 1 || !args) continue;
        const auto absolute = absolute_path(file->str());
        if (!within(absolute, source)) continue;
        result[{absolute.lexically_relative(source).generic_string(), static_cast<std::size_t>(*number)}]
            .push_back(json::Array(*args));
    }
    return result;
}

// Read CMake's evaluated arguments instead of trying to interpret its variable
// scopes, functions or loops. Guard the patch with those exact expansions so
// other expansions retain the original declaration.
std::optional<json::Object> variable_link(const std::string& source, const std::string& target,
                                         const std::string& dependency, std::size_t line,
                                         const std::vector<json::Array>& executions) {
    const auto all = tokens(source);
    static const std::regex expression(R"((?:[A-Za-z0-9_:.+\-]|\$\{[A-Za-z_][A-Za-z0-9_]*\})+)");
    for (std::size_t i = 0; i + 2 < all.size(); ++i) {
        auto command = all[i].text;
        std::ranges::transform(command, command.begin(), [](unsigned char c) { return std::tolower(c); });
        if (all[i].line != line || command != "target_link_libraries" || all[i + 1].text != "(") continue;
        const auto start = i + 2;
        auto end = start;
        while (end < all.size() && all[end].text != ")") ++end;
        if (end == all.size()) return std::nullopt;
        std::vector<std::string> expressions;
        bool variable = false;
        for (auto j = start; j < end; ++j) {
            auto text = all[j].text;
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') text = text.substr(1, text.size() - 2);
            if (!std::regex_match(text, expression)) return std::nullopt;
            variable |= text.find("${") != std::string::npos;
            expressions.push_back(std::move(text));
        }
        if (!variable) return std::nullopt;
        std::optional<json::Object> selected;
        for (const auto& args : executions) {
            if (args.size() != expressions.size() || args.empty() || args.front().getAsString() != target) continue;
            json::Array expanded;
            std::string condition;
            bool safe = true;
            for (std::size_t j = 0; j < args.size(); ++j) {
                const auto value = args[j].getAsString();
                if (!value) { safe = false; break; }
                std::size_t pos = 0;
                while (pos < value->size()) {
                    const auto stop = value->find(';', pos);
                    const auto word = value->substr(pos, stop == llvm::StringRef::npos ? value->size() - pos : stop - pos).str();
                    if (!word.empty()) {
                        if (!name(word) || word == "debug" || word == "optimized" || word == "general" ||
                            word == "LINK_PRIVATE" || word == "LINK_PUBLIC" || word == "LINK_INTERFACE_LIBRARIES") safe = false;
                        expanded.push_back(word);
                    }
                    if (stop == llvm::StringRef::npos) break;
                    pos = stop + 1;
                }
                if (expressions[j].find("${") != std::string::npos) {
                    if (!condition.empty()) condition += " AND ";
                    condition += "\"" + expressions[j] + "\" STREQUAL \"" + value->str() + "\"";
                } else if (expressions[j] != *value) safe = false;
            }
            if (!safe || expanded.empty() || expanded.front().getAsString() != target) continue;
            std::string scope;
            unsigned matches = 0;
            for (std::size_t j = 1; j < expanded.size(); ++j) {
                const auto word = expanded[j].getAsString()->str();
                if (word == "PRIVATE" || word == "PUBLIC" || word == "INTERFACE") scope = word;
                else if (word == dependency) {
                    if (scope == "PUBLIC" || scope == "INTERFACE") { safe = false; break; }
                    ++matches;
                }
            }
            if (!safe || matches != 1) continue;
            if (selected) return std::nullopt; // Repeated declarations need a separate proof.
            selected = json::Object{{"expanded_arguments", std::move(expanded)}, {"condition", condition},
                {"offset", static_cast<std::int64_t>(all[i].offset)}, {"length", 0},
                {"call_offset", static_cast<std::int64_t>(all[i].offset)},
                {"call_length", static_cast<std::int64_t>(all[end].offset + 1 - all[i].offset)}};
        }
        return selected;
    }
    return std::nullopt;
}
}  // namespace

json::Object unused_link_candidates(const json::Object& graph, const fs::path& source,
                                    const Options& options, const fs::path& trace,
                                    const fs::path& traced_source) {
    const auto executions = link_trace(trace, absolute_path(traced_source));
    std::map<std::string, std::string> types, artifacts;
    for (const auto& value : array(graph, "targets")) {
        const auto& row = *value.getAsObject();
        const auto target = string(row, "name");
        types[target] = string(row, "type");
        if (const auto* files = row.getArray("artifacts"); files && files->size() == 1)
            artifacts[target] = files->front().getAsString()->str();
    }
    const auto interface = edges(relations(graph), "interfaceLinkLibraries");
    std::map<std::pair<std::string, std::string>, json::Object> selected;
    for (const auto& value : array(graph, "edges")) {
        const auto& edge = *value.getAsObject();
        const auto from = string(edge, "from"), to = string(edge, "to");
        if (edge.getString("kind") != "linkLibraries" || !name(from) || !name(to) ||
            (types[from] != "EXECUTABLE" && types[from] != "SHARED_LIBRARY" && types[from] != "MODULE_LIBRARY") ||
            types[to] != "STATIC_LIBRARY" || has(interface, from, to) ||
            !artifacts[from].starts_with("<BUILD>/") || !artifacts[to].starts_with("<BUILD>/") ||
            (!options.target.empty() && options.target != from) ||
            (!options.dependency.empty() && options.dependency != to)) continue;
        const auto file = edge.getString("file");
        const auto line = edge.getInteger("line");
        if (!file || !file->starts_with("<SOURCE>/") || !line || *line < 1) continue;
        const fs::path path = file->drop_front(9).str();
        if (path.is_absolute() || !within(absolute_path(source / path), absolute_path(source)) ||
            !fs::is_regular_file(source / path) || path.string().find_first_of("\n\r\t") != std::string::npos ||
            std::ranges::any_of(options.exclusions, [&](const fs::path& p) { return within(path, p); })) continue;
        const auto contents = read_file(source / path);
        const auto token = literal_link(contents, from, to, static_cast<std::size_t>(*line), true);
        json::Object declaration;
        if (token) {
            const auto call = literal_link(contents, from, to, static_cast<std::size_t>(*line), true, true);
            declaration = json::Object{{"offset", static_cast<std::int64_t>(token->offset)},
                {"length", static_cast<std::int64_t>(token->text.size())},
                {"call_offset", static_cast<std::int64_t>(call->offset)},
                {"call_length", static_cast<std::int64_t>(call->text.size())}};
        } else {
            const auto found = executions.find({path.generic_string(), static_cast<std::size_t>(*line)});
            if (found == executions.end()) continue;
            auto expanded = variable_link(contents, from, to, static_cast<std::size_t>(*line), found->second);
            if (!expanded) continue;
            declaration = std::move(*expanded);
        }
        const auto call_offset = static_cast<std::size_t>(*declaration.getInteger("call_offset"));
        const auto newline = contents.rfind('\n', call_offset);
        const auto begin = newline == std::string::npos ? 0 : newline + 1;
        if (contents.substr(begin, call_offset - begin).find_first_not_of(" \t") != std::string::npos) continue;
        declaration["target"] = from;
        declaration["dependency"] = to;
        declaration["file"] = path.generic_string();
        declaration["line"] = *line;
        declaration["output"] = artifacts[from];
        declaration["archive"] = artifacts[to];
        declaration["compile_only"] = false;
        selected.emplace(std::pair{from, to}, std::move(declaration));
    }
    json::Array rows;
    for (auto& [key, row] : selected) rows.push_back(std::move(row));
    return json::Object{{"candidates", std::move(rows)},
        {"candidate_links", static_cast<std::int64_t>(selected.size())}, {"attempts", json::Array{}},
        {"rule", "global zero-extraction screening; cumulative batch trials; preserved compiler commands; identical ELF replay; final complete build and CTest"},
        {"scope", "private static links with literal or traced variable arguments in one ELF LLD configuration; other expansions retain the original declaration"}};
}

std::vector<Replacement> dependency_replacements(const fs::path& source, const json::Array& selections) {
    using Call = std::pair<std::string, std::size_t>;
    std::map<Call, std::vector<const json::Object*>> calls;
    for (const auto& value : selections) {
        const auto& row = *value.getAsObject();
        calls[{string(row, "file"), static_cast<std::size_t>(*row.getInteger("call_offset"))}].push_back(&row);
    }
    std::vector<Replacement> result;
    for (auto& [key, rows] : calls) {
        const auto contents = read_file(source / key.first);
        const auto offset = key.second;
        const auto length = static_cast<std::size_t>(*rows.front()->getInteger("call_length"));
        if (offset > contents.size() || length > contents.size() - offset)
            throw std::runtime_error("Invalid CMake call range");
        const auto original = contents.substr(offset, length);
        if (rows.front()->getArray("expanded_arguments")) {
            const auto newline = contents.rfind('\n', offset);
            const auto begin = newline == std::string::npos ? 0 : newline + 1;
            const auto prefix = contents.substr(begin, offset - begin);
            const std::string indent = prefix.find('\t') == std::string::npos ? "  " : "\t";
            const auto nested = prefix + indent;
            std::map<std::string, std::vector<const json::Object*>> variants;
            for (const auto* row : rows) variants[string(*row, "condition")].push_back(row);
            std::string modified;
            for (const auto& [condition, edits] : variants) {
                const auto& args = *edits.front()->getArray("expanded_arguments");
                std::map<std::string, bool> removals;
                bool compile_only = false;
                for (const auto* row : edits) {
                    const bool keep = row->getBoolean("compile_only").value_or(false);
                    removals[string(*row, "dependency")] = keep;
                    compile_only |= keep;
                    if (args != *row->getArray("expanded_arguments"))
                        throw std::runtime_error("Conflicting expanded CMake calls");
                }
                std::string call = "target_link_libraries(";
                for (const auto& arg : args) {
                    const auto word = arg.getAsString()->str();
                    const auto found = removals.find(word);
                    if (found != removals.end()) {
                        if (found->second) call += " \"$<COMPILE_ONLY:" + word + ">\"";
                    } else {
                        if (call.back() != '(') call += ' ';
                        call += word;
                    }
                }
                call += ')';
                modified += (modified.empty() ? "if(" : prefix + "elseif(") + condition;
                if (compile_only) modified += " AND CMAKE_VERSION VERSION_GREATER_EQUAL 3.27";
                modified += ")\n";
                if (compile_only) modified += nested + "cmake_policy(PUSH)\n" + nested + "cmake_policy(SET CMP0099 NEW)\n";
                modified += nested + call + '\n';
                if (compile_only) modified += nested + "cmake_policy(POP)\n";
            }
            auto fallback = original;
            for (auto pos = fallback.find('\n'); pos != std::string::npos;
                 pos = fallback.find('\n', pos + 1 + indent.size())) fallback.insert(pos + 1, indent);
            modified += prefix + "else()\n" + nested + fallback + '\n' + prefix + "endif()";
            result.push_back({key.first, offset, length, std::move(modified)});
            continue;
        }
        auto modified = original;
        std::ranges::sort(rows, [](const auto* a, const auto* b) { return *a->getInteger("offset") > *b->getInteger("offset"); });
        auto previous = offset + length;
        bool compile_only = false;
        for (const auto* row : rows) {
            const auto start = static_cast<std::size_t>(*row->getInteger("offset"));
            const auto size = static_cast<std::size_t>(*row->getInteger("length"));
            if (start < offset || start > previous || size > previous - start)
                throw std::runtime_error("Overlapping CMake dependency edits");
            const bool keep = row->getBoolean("compile_only").value_or(false);
            modified.replace(start - offset, size, keep ? "\"$<COMPILE_ONLY:" + string(*row, "dependency") + ">\"" : "");
            previous = start;
            compile_only |= keep;
        }
        if (compile_only) {
            const auto newline = contents.rfind('\n', offset);
            const auto begin = newline == std::string::npos ? 0 : newline + 1;
            const auto prefix = contents.substr(begin, offset - begin);
            const std::string indent = prefix.find('\t') == std::string::npos ? "  " : "\t";
            const auto nested = prefix + indent;
            const auto indent_call = [&](std::string command) {
                for (auto pos = command.find('\n'); pos != std::string::npos;
                     pos = command.find('\n', pos + 1 + indent.size())) command.insert(pos + 1, indent);
                return nested + command;
            };
            modified = "if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.27)\n" + nested + "cmake_policy(PUSH)\n" +
                nested + "cmake_policy(SET CMP0099 NEW)\n" + indent_call(modified) + "\n" + nested +
                "cmake_policy(POP)\n" + prefix + "else()\n" + indent_call(original) + "\n" + prefix + "endif()";
        }
        result.push_back({key.first, offset, length, std::move(modified)});
    }
    return result;
}

json::Object link_candidates(const json::Object& graph, const fs::path& source,
                             const Options& options) {
    const auto all = relations(graph);
    const auto direct = edges(all, "linkLibraries"), compile = edges(all, "compileDependencies");
    const auto interface = edges(all, "interfaceLinkLibraries"),
               interface_compile = edges(all, "interfaceCompileDependencies");
    std::map<std::string, std::string> types;
    for (const auto& value : array(graph, "targets")) {
        const auto* row = value.getAsObject();
        if (!row) throw std::runtime_error("Invalid CMake target");
        if (!row->getBoolean("abstract").value_or(false))
            types[string(*row, "name")] = string(*row, "type");
    }
    std::map<std::pair<std::string, std::string>, json::Object> candidates;
    for (const auto& value : array(graph, "edges")) {
        if (interrupted()) throw std::runtime_error("Interrupted during CMake selection");
        const auto& row = *value.getAsObject();
        const auto from = string(row, "from"), to = string(row, "to");
        if (row.getString("kind") != "linkLibraries" || !name(from) || !name(to) || from == to ||
            !types.contains(from) ||
            (types.at(from) != "EXECUTABLE" && types.at(from) != "STATIC_LIBRARY" &&
             types.at(from) != "SHARED_LIBRARY") ||
            (!options.target.empty() && options.target != from) ||
            (!options.dependency.empty() && options.dependency != to) || !has(compile, from, to))
            continue;
        // Object libraries can add object files rather than just linking/usage requirements.
        const auto* targets = graph.getArray("targets");
        bool supported = false;
        for (const auto& target : *targets) {
            const auto& item = *target.getAsObject();
            const auto type = string(item, "type");
            if (item.getString("name") == to &&
                (type == "STATIC_LIBRARY" || type == "SHARED_LIBRARY" ||
                 type == "INTERFACE_LIBRARY"))
                supported = true;
        }
        if (!supported) continue;
        const auto path = alternate(direct, compile, interface, interface_compile, from, to);
        if (path.empty()) continue;
        const auto exported = has(interface, from, to);
        const auto exported_path = exported ? alternate(interface, interface_compile, interface,
                                                        interface_compile, from, to)
                                            : std::vector<std::string>{};
        if (exported && exported_path.empty()) continue;
        const auto file = row.getString("file");
        const auto line = row.getInteger("line");
        if (!file || !file->starts_with("<SOURCE>/") || !line || *line < 1) continue;
        const fs::path relative = file->drop_front(9).str();
        if (relative.is_absolute() ||
            !within(absolute_path(source / relative), absolute_path(source)) ||
            !fs::is_regular_file(source / relative) ||
            relative.string().find_first_of("\n\r\t") != std::string::npos ||
            std::ranges::any_of(options.exclusions, [&](const fs::path& excluded) {
                return within(relative, excluded);
            }))
            continue;
        const auto token =
            literal_link(read_file(source / relative), from, to, static_cast<std::size_t>(*line));
        if (!token) continue;
        json::Array via, exported_via;
        for (const auto& node : path) via.push_back(node);
        for (const auto& node : exported_path) exported_via.push_back(node);
        candidates.emplace(std::pair{from, to},
                           json::Object{{"target", from},
                                        {"dependency", to},
                                        {"file", relative.generic_string()},
                                        {"line", *line},
                                        {"offset", static_cast<std::int64_t>(token->offset)},
                                        {"length", static_cast<std::int64_t>(token->text.size())},
                                        {"alternate_path", std::move(via)},
                                        {"interface_path", std::move(exported_via)}});
    }
    json::Array rows;
    for (auto& [key, row] : candidates) rows.push_back(std::move(row));
    return json::Object{
        {"candidates", std::move(rows)},
        {"rule",
         "one literal direct link with an alternate transitive path preserving both link and "
         "compilation usage requirements; exported links require an alternate interface path too"},
        {"scope",
         "configured graph only; no unused-symbol inference; first candidate by target and "
         "dependency name; full build and tests still required"}};
}

void verify_link_removal(const json::Object& before, const json::Object& after,
                         const json::Object& selection, bool compile_only) {
    auto row = json::Object(selection);
    row["compile_only"] = compile_only;
    json::Array rows;
    rows.push_back(std::move(row));
    verify_link_removals(before, after, rows);
}

void verify_link_removals(const json::Object& before, const json::Object& after,
                          const json::Array& selections) {
    auto expected = relations(before), actual = relations(after);
    for (const auto& value : selections) {
        const auto& row = *value.getAsObject();
        const auto from = string(row, "target"), to = string(row, "dependency");
        for (const auto* kind : {"linkLibraries", "compileDependencies", "interfaceLinkLibraries", "interfaceCompileDependencies"})
            if (!row.getBoolean("compile_only").value_or(false) ||
                (std::string_view(kind) != "compileDependencies" && std::string_view(kind) != "interfaceCompileDependencies"))
                expected.erase({from, to, kind});
        expected.erase({from, to, "orderDependencies"});
        actual.erase({from, to, "orderDependencies"});
    }
    if (actual != expected)
        throw std::runtime_error(
            "CMake removal changed unexpected target relationships or retained the direct link");
    const auto canonical = [](const json::Object& graph, const char* key) {
        std::map<std::string, std::vector<std::string>> rows;
        for (const auto& value : array(graph, key)) {
            const auto& row = *value.getAsObject();
            if (std::string_view(key) == "targets")
                rows[string(row, "name")].push_back(
                    string(row, "type") +
                    (row.getBoolean("abstract").value_or(false) ? ":abstract" : ":compiled"));
            else
                rows[string(row, "from") + "\n" + string(row, "kind")].push_back(
                    string(row, "fragment"));
        }
        return rows;
    };
    if (canonical(before, "targets") != canonical(after, "targets") ||
        canonical(before, "link_fragments") != canonical(after, "link_fragments"))
        throw std::runtime_error("CMake removal changed targets or raw linker arguments");
}
}  // namespace buildray
