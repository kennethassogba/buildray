#include "buildray/core.hpp"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace buildray {
namespace {
using Graph = std::map<std::string, std::set<std::string>>;

json::Array strings(const std::set<std::string>& values) {
    json::Array result;
    for (const auto& value : values) result.push_back(value);
    return result;
}

std::string required(const json::Object& object, llvm::StringRef key) {
    const auto value = object.getString(key);
    if (!value) throw std::runtime_error("Missing dependency field: " + key.str());
    return value->str();
}

std::string normalized(const fs::path& file, const fs::path& source, const fs::path& build) {
    const auto path = absolute_path(file);
    if (within(path, source)) return "<SOURCE>/" + path.lexically_relative(source).generic_string();
    if (within(path, build)) return "<BUILD>/" + path.lexically_relative(build).generic_string();
    return path.string();
}

// Iterative Kosaraju: large include trees must not exhaust the process stack.
json::Array cycles(const Graph& graph) {
    Graph reverse;
    for (const auto& [from, next] : graph) {
        reverse[from];
        for (const auto& to : next) reverse[to].insert(from);
    }
    std::set<std::string> visited;
    std::vector<std::string> order;
    for (const auto& [root, unused] : graph) {
        std::vector<std::pair<std::string, bool>> stack{{root, false}};
        while (!stack.empty()) {
            auto [node, expanded] = std::move(stack.back());
            stack.pop_back();
            if (expanded) { order.push_back(node); continue; }
            if (!visited.insert(node).second) continue;
            stack.emplace_back(node, true);
            if (const auto it = graph.find(node); it != graph.end())
                for (const auto& child : it->second) stack.emplace_back(child, false);
        }
    }
    visited.clear();
    std::vector<std::set<std::string>> components;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        if (visited.contains(*it)) continue;
        std::set<std::string> component;
        std::vector<std::string> stack{*it};
        while (!stack.empty()) {
            auto node = std::move(stack.back());
            stack.pop_back();
            if (!visited.insert(node).second) continue;
            component.insert(node);
            for (const auto& child : reverse.at(node)) stack.push_back(child);
        }
        if (component.size() > 1 ||
            (graph.contains(*it) && graph.at(*it).contains(*it)))
            components.push_back(std::move(component));
    }
    std::ranges::sort(components);
    json::Array result;
    for (const auto& component : components) {
        json::Array edges;
        for (const auto& from : component)
            if (graph.contains(from))
                for (const auto& to : graph.at(from))
                    if (component.contains(to))
                        edges.push_back(json::Object{{"from", from}, {"to", to}});
        result.push_back(json::Object{{"members", strings(component)}, {"edges", std::move(edges)}});
    }
    return result;
}

json::Value reply_file(const fs::path& directory, const std::string& name) {
    const fs::path path(name);
    if (path.is_absolute() || path.filename() != path || path.extension() != ".json")
        throw std::runtime_error("Invalid CMake File API reply filename");
    return read_json(directory / path);
}

json::Object cmake_graph(const fs::path& source, const fs::path& build) {
    const auto directory = build / ".cmake/api/v1/reply";
    fs::path index;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.path().filename().string().starts_with("index-") &&
            (index.empty() || entry.path().filename() > index.filename())) index = entry.path();
    if (index.empty()) throw std::runtime_error("Missing CMake File API index");
    const auto document = read_json(index);
    const auto* object = document.getAsObject();
    const auto* reply = object ? object->getObject("reply") : nullptr;
    const auto* model_reply = reply ? reply->getObject("codemodel-v2") : nullptr;
    if (!model_reply) throw std::runtime_error("Missing CMake codemodel reply");
    const auto model_doc = reply_file(directory, required(*model_reply, "jsonFile"));
    const auto* model = model_doc.getAsObject();
    const auto* version = model ? model->getObject("version") : nullptr;
    if (!version || version->getInteger("major") != 2 || version->getInteger("minor").value_or(0) < 9)
        throw std::runtime_error("Dependency analysis requires CMake codemodel 2.9+ (CMake 4.2+)");
    const auto* configurations = model->getArray("configurations");
    if (!configurations || configurations->size() != 1)
        throw std::runtime_error("Expected one CMake configuration");
    const auto* configuration = configurations->front().getAsObject();
    const auto* targets = configuration ? configuration->getArray("targets") : nullptr;
    if (!targets) throw std::runtime_error("Missing CMake targets");
    std::map<std::string, json::Object> definitions;
    std::map<std::string, std::string> names;
    std::map<std::string, std::size_t> name_counts;
    json::Array all_targets(*targets);
    if (const auto* abstract = configuration->getArray("abstractTargets"))
        for (const auto& target : *abstract) all_targets.push_back(target);
    for (const auto& value : all_targets) {
        const auto* entry = value.getAsObject();
        if (!entry) throw std::runtime_error("Invalid CMake target entry");
        const auto target_doc = reply_file(directory, required(*entry, "jsonFile"));
        const auto* target = target_doc.getAsObject();
        if (!target) throw std::runtime_error("Invalid CMake target definition");
        const auto id = required(*target, "id");
        names[id] = required(*target, "name");
        ++name_counts[names[id]];
        definitions.emplace(id, json::Object(*target));
    }
    // Imported targets may have the same local name in different directories.
    // Keep their File API identities distinct rather than inventing cross-scope edges.
    for (auto& [id, name] : names)
        if (name_counts.at(name) > 1) name += " [" + id + "]";
    json::Array nodes, edges, fragments;
    Graph links;
    for (const auto& [id, target] : definitions) {
        const auto& name = names.at(id);
        links[name];
        json::Array artifacts;
        if (const auto* paths = target.getArray("artifacts"))
            for (const auto& value : *paths)
                if (const auto* item = value.getAsObject())
                    artifacts.push_back(normalized(build / required(*item, "path"), source, build));
        nodes.push_back(json::Object{{"name", name}, {"id", id}, {"type", required(target, "type")},
                                     {"artifacts", std::move(artifacts)},
                                     {"abstract", target.getBoolean("abstract").value_or(false)}});
        // The legacy "dependencies" field is a transitive build-order graph.
        // It does not represent direct target_link_libraries and can omit cycles.
        for (const auto* kind : {"linkLibraries", "interfaceLinkLibraries", "compileDependencies",
                                 "interfaceCompileDependencies", "objectDependencies", "orderDependencies"}) {
            const auto* dependencies = target.getArray(kind);
            if (!dependencies) continue;
            for (const auto& value : *dependencies) {
                const auto* dependency = value.getAsObject();
                if (!dependency) throw std::runtime_error("Invalid CMake target dependency");
                const auto to_id = dependency->getString("id");
                const auto fragment = dependency->getString("fragment");
                const bool link_kind = std::string_view(kind) == "linkLibraries" ||
                                       std::string_view(kind) == "interfaceLinkLibraries";
                if (to_id.has_value() == fragment.has_value() || (fragment && !link_kind))
                    throw std::runtime_error("Invalid CMake relationship: expected target id or link fragment");
                json::Object edge{{"from", name}, {"kind", kind}};
                if (to_id) {
                    if (!names.contains(to_id->str()))
                        throw std::runtime_error("Unresolved CMake target dependency");
                    edge["to"] = names.at(to_id->str());
                    if (link_kind) links[name].insert(names.at(to_id->str()));
                } else {
                    // Libraries such as m/dl and raw linker arguments are not targets.
                    // Retain them without inventing vertices or target cycles.
                    edge["fragment"] = fragment->str();
                }
                if (const auto backtrace = dependency->getInteger("backtrace")) {
                    const auto* trace = target.getObject("backtraceGraph");
                    const auto* entries = trace ? trace->getArray("nodes") : nullptr;
                    const auto* files = trace ? trace->getArray("files") : nullptr;
                    if (entries && files && *backtrace >= 0 && static_cast<std::size_t>(*backtrace) < entries->size()) {
                        const auto* location = (*entries)[static_cast<std::size_t>(*backtrace)].getAsObject();
                        const auto file = location ? location->getInteger("file") : std::nullopt;
                        if (file && *file >= 0 && static_cast<std::size_t>(*file) < files->size()) {
                            const auto path = (*files)[static_cast<std::size_t>(*file)].getAsString();
                            if (path) edge["file"] = normalized(source / path->str(), source, build);
                            if (const auto line = location->getInteger("line")) edge["line"] = *line;
                        }
                    }
                }
                if (to_id) edges.push_back(std::move(edge));
                else fragments.push_back(std::move(edge));
            }
        }
    }
    return json::Object{{"codemodel_minor", *version->getInteger("minor")},
                         {"scope", "configured target relationships; includes excluded targets; no unused-link inference"},
                         {"targets", std::move(nodes)}, {"edges", std::move(edges)},
                         {"link_fragments", std::move(fragments)},
                         {"link_cycles", cycles(links)}};
}

std::string markdown_label(std::string value) {
    for (auto& c : value) if (c == '|' || c == '`' || c == '\n' || c == '\r' || c == '<' || c == '>') c = ' ';
    return value;
}

std::string quoted(const std::string& value) {
    std::string result = "\"";
    for (const auto c : value) {
        if (c == '\\' || c == '"') result += '\\';
        if (c == '\n' || c == '\r') result += "\\n";
        else result += c;
    }
    return result + '"';
}
}  // namespace

json::Object cmake_dependencies(const fs::path& source, const fs::path& build) {
    return cmake_graph(source, build);
}

std::pair<std::size_t, std::string> include_line(const std::string& line) {
    const auto depth = line.find_first_not_of('.');
    if (depth == std::string::npos || depth == 0 || depth + 1 >= line.size() || line[depth] != ' ')
        return {};
    auto file = line.substr(depth + 1);
    // Clang can annotate framework includes with this suffix.
    if (file.ends_with(" (framework directory)")) file.resize(file.size() - 22);
    return {depth, file};
}

json::Object dependency_graph(const fs::path& records, const fs::path& source,
                              const fs::path& build, const std::vector<fs::path>& originals) {
    std::set<fs::path> original_files;
    for (const auto& path : originals) original_files.insert(absolute_path(source / path));
    Graph graph;
    std::map<std::pair<std::string, std::string>, std::size_t> observed;
    std::map<std::string, std::size_t> reached, incoming;
    std::size_t count = 0, c_count = 0, cpp_count = 0;
    std::set<std::string> translation_units;
    for (const auto& entry : fs::directory_iterator(records)) {
        if (interrupted()) throw std::runtime_error("Interrupted");
        if (entry.path().extension() != ".json") continue;
        const auto document = read_json(entry.path());
        const auto* record = document.getAsObject();
        if (!record || record->getInteger("exit_code") != 0)
            throw std::runtime_error("Invalid include compiler record");
        const fs::path cwd = required(*record, "directory");
        const auto root = absolute_path(cwd / required(*record, "source"));
        const auto trace_file = required(*record, "include_file");
        if (trace_file.empty()) throw std::runtime_error("Missing compiler include trace");
        const auto root_name = normalized(root, source, build);
        translation_units.insert(root_name);
        if (original_files.contains(root)) graph[root_name];
        ++count;
        if (root.extension() == ".c") ++c_count;
        else ++cpp_count;
        std::vector<fs::path> stack{root};
        std::set<std::pair<std::string, std::string>> invocation_edges;
        std::set<std::string> invocation_headers;
        std::istringstream lines(read_file(trace_file));
        std::string line;
        while (std::getline(lines, line)) {
            const auto [depth, file] = include_line(line);
            if (!depth) continue;
            if (depth > stack.size()) throw std::runtime_error("Invalid include trace nesting");
            const auto child = absolute_path(cwd / file);
            const auto parent = stack[depth - 1];
            stack.resize(depth);
            stack.push_back(child);
            if (!original_files.contains(child)) continue;
            const auto to = normalized(child, source, build);
            graph[to];
            invocation_headers.insert(to);
            if (!original_files.contains(parent)) continue;
            const auto from = normalized(parent, source, build);
            graph[from].insert(to);
            invocation_edges.emplace(from, to);
        }
        for (const auto& edge : invocation_edges) ++observed[edge];
        for (const auto& header : invocation_headers) ++reached[header];
    }
    if (count == 0) throw std::runtime_error("No include traces captured");
    json::Array nodes, edges;
    for (const auto& [name, unused] : graph) nodes.push_back(name);
    for (const auto& [edge, invocations] : observed) {
        ++incoming[edge.second];
        edges.push_back(json::Object{{"from", edge.first}, {"to", edge.second},
                                     {"compilations", static_cast<std::int64_t>(invocations)}});
    }
    std::vector<std::pair<std::string, std::size_t>> ranking;
    for (const auto& [header, invocations] : reached)
        if (!translation_units.contains(header)) ranking.emplace_back(header, invocations);
    std::ranges::sort(ranking, [](const auto& a, const auto& b) {
        return a.second != b.second ? a.second > b.second : a.first < b.first;
    });
    json::Array headers;
    for (const auto& [header, invocations] : ranking)
        headers.push_back(json::Object{{"header", header},
                                      {"compilations", static_cast<std::int64_t>(invocations)},
                                      {"direct_includers", static_cast<std::int64_t>(incoming[header])}});
    return json::Object{{"schema_version", 1}, {"compilations", static_cast<std::int64_t>(count)},
                         {"c_compilations", static_cast<std::int64_t>(c_count)},
                         {"cxx_compilations", static_cast<std::int64_t>(cpp_count)},
                         {"scope", "observed default build; original snapshot files only (including vendored files); external and generated include nodes excluded"},
                         {"cycle_semantics", "strongly connected components; guarded include cycles and static-library link cycles may be valid; no automatic fixes"},
                         {"metric", "compilations reaching each header, including skipped includes; not parse time or predicted rebuild time"},
                         {"nodes", std::move(nodes)}, {"edges", std::move(edges)},
                         {"include_cycles", cycles(graph)}, {"headers", std::move(headers)},
                         {"cmake", cmake_graph(source, build)}};
}

std::string dependencies_markdown(const json::Object& graph) {
    std::ostringstream out;
    const auto& cmake = *graph.getObject("cmake");
    out << "# Build dependencies\n\n" << *graph.getInteger("compilations") << " captured compilations ("
        << *graph.getInteger("c_compilations") << " C, " << *graph.getInteger("cxx_compilations") << " C++).\n\n"
        << graph.getString("scope")->str() << ".\n\n" << graph.getString("cycle_semantics")->str() << ".\n\n";
    for (const auto& [label, findings] : std::vector<std::pair<std::string, const json::Array*>>{
             {"Include cycles", graph.getArray("include_cycles")}, {"Target-link cycles", cmake.getArray("link_cycles")}}) {
        out << "## " << label << " (" << findings->size() << ")\n\n";
        for (std::size_t i = 0; i < std::min<std::size_t>(10, findings->size()); ++i) {
            out << "- ";
            bool first = true;
            for (const auto& member : *(*findings)[i].getAsObject()->getArray("members")) {
                if (!first) out << ", ";
                out << '`' << markdown_label(member.getAsString()->str()) << '`';
                first = false;
            }
            out << '\n';
        }
        if (findings->empty()) out << "None observed.\n";
        out << '\n';
    }
    out << "## Shared headers\n\n" << graph.getString("metric")->str() << ".\n\n"
        << "| Header | Compilations | Direct includers |\n|---|---:|---:|\n";
    const auto& headers = *graph.getArray("headers");
    for (std::size_t i = 0; i < std::min<std::size_t>(30, headers.size()); ++i) {
        const auto& header = *headers[i].getAsObject();
        out << "| `" << markdown_label(required(header, "header")) << "` | "
            << *header.getInteger("compilations") << " | " << *header.getInteger("direct_includers") << " |\n";
    }
    out << "\nInspect cycle edges in dependencies.json before changing declarations or includes. "
           "CMake edges retain relationship kinds and available declaration locations. "
           "No missing or unused link dependency is inferred. Use profile for parse costs, "
           "then optimize or pch to validate and measure a candidate.\n";
    return out.str();
}

std::string dependencies_dot(const json::Object& graph) {
    std::ostringstream out;
    out << "digraph buildray {\n  subgraph cluster_includes {\n    label=\"Observed includes\";\n";
    for (const auto& node : *graph.getArray("nodes"))
        out << "    " << quoted("include:" + node.getAsString()->str()) << " [label=" << quoted(node.getAsString()->str()) << "];\n";
    for (const auto& value : *graph.getArray("edges")) {
        const auto& edge = *value.getAsObject();
        out << "    " << quoted("include:" + required(edge, "from")) << " -> " << quoted("include:" + required(edge, "to")) << ";\n";
    }
    out << "  }\n  subgraph cluster_targets {\n    label=\"Configured CMake targets\";\n";
    const auto& cmake = *graph.getObject("cmake");
    for (const auto& value : *cmake.getArray("targets")) {
        const auto& node = *value.getAsObject();
        out << "    " << quoted("target:" + required(node, "name")) << " [label=" << quoted(required(node, "name")) << "];\n";
    }
    for (const auto& value : *cmake.getArray("edges")) {
        const auto& edge = *value.getAsObject();
        out << "    " << quoted("target:" + required(edge, "from")) << " -> " << quoted("target:" + required(edge, "to"))
            << " [label=" << quoted(required(edge, "kind")) << "];\n";
    }
    return out.str() + "  }\n}\n";
}
}  // namespace buildray
