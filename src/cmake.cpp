#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <queue>
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
                                  const std::string& dependency, std::size_t line) {
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
                if (selected || scope == "INTERFACE") return std::nullopt;
                selected = token;
            }
        }
        return selected;
    }
    return std::nullopt;
}
}  // namespace

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
                         const json::Object& selection) {
    auto expected = relations(before);
    const auto from = string(selection, "target"), to = string(selection, "dependency");
    for (const auto* kind : {"linkLibraries", "compileDependencies", "interfaceLinkLibraries",
                             "interfaceCompileDependencies"})
        expected.erase({from, to, kind});
    // Order-only dependencies may disappear with a direct link; all other
    // relationships must remain, including raw linker fragments.
    auto actual = relations(after);
    expected.erase({from, to, "orderDependencies"});
    actual.erase({from, to, "orderDependencies"});
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
