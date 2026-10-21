#include "buildray/core.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace buildray {
namespace {
struct Cost {
    double seconds{};
    double maximum{};
    std::size_t events{};
    std::set<std::string> compilations;
};

std::string normalized(const fs::path& file, const fs::path& source, const fs::path& build) {
    const auto path = absolute_path(file);
    if (within(path, source)) return "<SOURCE>/" + path.lexically_relative(source).generic_string();
    if (within(path, build)) return "<BUILD>/" + path.lexically_relative(build).generic_string();
    return path.string();
}

std::string target_name(const json::Object& record) {
    const auto* command = record.getArray("command");
    if (!command) throw std::runtime_error("Missing profiling compiler command");
    for (std::size_t i = 0; i + 1 < command->size(); ++i) {
        if ((*command)[i].getAsString() != "-o") continue;
        const auto output = (*command)[i + 1].getAsString();
        if (!output) break;
        const auto marker = output->find("CMakeFiles/");
        if (marker == llvm::StringRef::npos) break;
        auto suffix = output->drop_front(marker + 11);
        const auto end = suffix.find(".dir/");
        if (end != llvm::StringRef::npos) return suffix.take_front(end).str();
    }
    throw std::runtime_error("Cannot identify CMake target from compiler output");
}

json::Array ranked(const std::map<std::string, Cost>& costs) {
    std::vector<std::pair<std::string, Cost>> entries(costs.begin(), costs.end());
    std::ranges::sort(entries, [](const auto& a, const auto& b) {
        return a.second.seconds != b.second.seconds ? a.second.seconds > b.second.seconds
                                                   : a.first < b.first;
    });
    json::Array result;
    for (const auto& [header, cost] : entries)
        result.push_back(json::Object{{"header", header},
                                     {"inclusive_seconds", cost.seconds},
                                     {"max_event_seconds", cost.maximum},
                                     {"events", static_cast<std::int64_t>(cost.events)},
                                     {"compilations", static_cast<std::int64_t>(cost.compilations.size())}});
    return result;
}

bool safe_name(const std::string& value, bool path) {
    return !value.empty() && std::ranges::all_of(value, [path](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.' || c == '+' ||
               (path && (c == '/' || c == ' '));
    });
}
}  // namespace

json::Object header_profile(const fs::path& records, const fs::path& source,
                            const fs::path& build) {
    std::map<std::string, Cost> totals;
    std::map<std::string, std::map<std::string, Cost>> targets;
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator(records)) {
        if (interrupted()) throw std::runtime_error("Interrupted");
        if (entry.path().extension() != ".json") continue;
        const auto document = read_json(entry.path());
        const auto* record = document.getAsObject();
        if (!record || record->getInteger("exit_code") != 0 || !record->getString("trace_file") ||
            !record->getString("directory"))
            throw std::runtime_error("Invalid profiling compiler record");
        const auto trace = read_json(record->getString("trace_file")->str());
        const auto* object = trace.getAsObject();
        const auto* events = object ? object->getArray("traceEvents") : nullptr;
        if (!events) throw std::runtime_error("Missing Clang time trace events");
        const auto target = target_name(*record);
        ++count;
        std::map<std::string, std::pair<double, std::string>> pending;
        const auto accumulate = [&](const std::string& detail, double duration) {
            if (!std::isfinite(duration) || duration < 0)
                throw std::runtime_error("Invalid header duration in Clang time trace");
            fs::path file = detail;
            if (file.is_relative()) file = fs::path(record->getString("directory")->str()) / file;
            const auto header = normalized(file, source, build);
            for (auto* cost : {&totals[header], &targets[target][header]}) {
                const double seconds = duration / 1e6;
                cost->seconds += seconds;
                cost->maximum = std::max(cost->maximum, seconds);
                ++cost->events;
                cost->compilations.insert(entry.path().filename().string());
            }
        };
        for (const auto& value : *events) {
            const auto* event = value.getAsObject();
            if (!event || event->getString("name") != "Source")
                continue;
            const auto* args = event->getObject("args");
            const auto detail = args ? args->getString("detail") : std::nullopt;
            const auto phase = event->getString("ph");
            if (phase == "X") {
                const auto duration = event->getNumber("dur");
                if (!duration || !detail) throw std::runtime_error("Malformed complete Source event");
                accumulate(detail->str(), *duration);
            } else if (phase == "b" || phase == "e") {
                // Clang 21 writes Source events as asynchronous begin/end pairs.
                // Pairs can be emitted in postorder, so do not sort by timestamp.
                const auto id = event->getInteger("id");
                const auto pid = event->getInteger("pid");
                const auto tid = event->getInteger("tid");
                const auto timestamp = event->getNumber("ts");
                if (!id || !pid || !tid || !timestamp || !std::isfinite(*timestamp))
                    throw std::runtime_error("Malformed asynchronous Source event");
                const auto key = std::to_string(*pid) + "/" + std::to_string(*tid) + "/" + std::to_string(*id);
                if (phase == "b") {
                    if (!detail || !pending.emplace(key, std::pair{*timestamp, detail->str()}).second)
                        throw std::runtime_error("Duplicate or unnamed Source begin event");
                } else {
                    const auto begin = pending.find(key);
                    if (begin == pending.end()) throw std::runtime_error("Unmatched Source end event");
                    accumulate(begin->second.second, *timestamp - begin->second.first);
                    pending.erase(begin);
                }
            } else {
                throw std::runtime_error("Unsupported Source event phase");
            }
        }
        if (!pending.empty()) throw std::runtime_error("Unfinished Source events in Clang time trace");
    }
    if (count == 0) throw std::runtime_error("No compiler time traces captured");
    json::Array by_target;
    for (const auto& [name, costs] : targets)
        by_target.push_back(json::Object{{"target", name}, {"headers", ranked(costs)}});
    return json::Object{{"schema_version", 1},
                        {"compilations", static_cast<std::int64_t>(count)},
                        {"metric", "inclusive Source event duration; nested headers overlap; not wall time or predicted savings"},
                        {"granularity_microseconds", 0},
                        {"headers", ranked(totals)},
                        {"targets", std::move(by_target)}};
}

std::string profile_markdown(const json::Object& profile) {
    std::ostringstream out;
    out << "# Header profiling\n\n" << profile.getString("metric")->str()
        << ". One instrumented build; profiling overhead is excluded from performance claims.\n\n"
        << "| Header | Inclusive seconds | Compilations |\n|---|---:|---:|\n";
    const auto* headers = profile.getArray("headers");
    for (std::size_t i = 0; i < std::min<std::size_t>(30, headers->size()); ++i) {
        const auto& row = *(*headers)[i].getAsObject();
        std::string label = row.getString("header")->str();
        for (auto& c : label) if (c == '|' || c == '\n' || c == '\r' || c == '`') c = ' ';
        out << "| `" << label << "` | " << std::fixed << std::setprecision(6)
            << *row.getNumber("inclusive_seconds") << " | " << *row.getInteger("compilations") << " |\n";
    }
    out << "\nFull rankings by target are in headers.json. Choose stable, repeated headers for a PCH experiment.\n";
    return out.str();
}

json::Object select_pch(const json::Object& profile, const fs::path& source,
                        const std::string& selected_target) {
    if (!selected_target.empty() && !safe_name(selected_target, false))
        throw std::runtime_error("Unsupported PCH target name");
    const auto* targets = profile.getArray("targets");
    if (!targets) throw std::runtime_error("Missing per-target header profile");
    json::Object selected;
    double best_seconds = -1;
    std::pair<std::string, std::string> best_key;
    for (const auto& value : *targets) {
        if (interrupted()) throw std::runtime_error("Interrupted");
        const auto* target = value.getAsObject();
        const auto name = target ? target->getString("target") : std::nullopt;
        const auto* headers = target ? target->getArray("headers") : nullptr;
        if (!name || !headers) throw std::runtime_error("Invalid per-target header profile");
        if ((!selected_target.empty() && *name != selected_target) || !safe_name(name->str(), false)) continue;
        for (const auto& row : *headers) {
            const auto* header = row.getAsObject();
            const auto path = header ? header->getString("header") : std::nullopt;
            const auto seconds = header ? header->getNumber("inclusive_seconds") : std::nullopt;
            const auto compilations = header ? header->getInteger("compilations") : std::nullopt;
            if (!path || !seconds || !compilations || !std::isfinite(*seconds) || *seconds < 0 || *compilations < 0)
                throw std::runtime_error("Invalid PCH selection metric");
            if (!path->starts_with("<SOURCE>/") || *compilations < 2 || *seconds == 0) continue;
            const fs::path relative = path->drop_front(9).str();
            const auto extension = relative.extension();
            if (!safe_name(relative.generic_string(), true) || relative.is_absolute() ||
                (extension != ".h" && extension != ".hpp" && extension != ".hh" && extension != ".hxx") ||
                !within(absolute_path(source / relative), absolute_path(source)) ||
                !fs::is_regular_file(source / relative)) continue;
            const auto key = std::pair{name->str(), relative.lexically_normal().generic_string()};
            if (*seconds < best_seconds || (*seconds == best_seconds && key >= best_key)) continue;
            best_seconds = *seconds;
            best_key = key;
            selected = json::Object{{"target", key.first}, {"header", key.second},
                                     {"profile_header", path->str()},
                                     {"inclusive_seconds", *seconds}, {"compilations", *compilations},
                                     {"rule", "highest per-target inclusive parse cost among original project headers used in at least two compilations; ties by target and path"},
                                     {"scope", "one candidate; inclusive parse cost is not predicted build-time savings"}};
        }
    }
    if (selected.empty())
        throw std::runtime_error("No eligible project header used by at least two compilations of the selected targets; use an explicit --target and --header");
    return selected;
}

std::string pch_cmake(const Options& options, const fs::path& source) {
    if (!safe_name(options.target, false)) throw std::runtime_error("Unsupported PCH target name");
    std::string includes;
    std::set<std::string> unique;
    for (const auto& header : options.headers) {
        if (!unique.insert(header).second) throw std::runtime_error("Duplicate PCH header: " + header);
        std::string expression;
        if (header.starts_with('<') && header.ends_with('>')) {
            const auto inner = header.substr(1, header.size() - 2);
            if (!safe_name(inner, true) || inner.find(' ') != std::string::npos)
                throw std::runtime_error("Unsupported system PCH header: " + header);
            expression = "<" + inner + "$<ANGLE-R>";
        } else {
            const fs::path path(header);
            if (!safe_name(header, true) || path.is_absolute() ||
                !within(source / path, source) || !fs::is_regular_file(source / path))
                throw std::runtime_error("PCH header must be a file inside the source tree: " + header);
            expression = "${CMAKE_CURRENT_SOURCE_DIR}/" + path.lexically_normal().generic_string();
        }
        includes += "  \"$<$<COMPILE_LANGUAGE:CXX>:" + expression + ">\"\n";
    }
    const auto& target = options.target;
    const std::string experiment =
           "if(NOT TARGET " + target + ")\n  message(FATAL_ERROR \"Buildray PCH target does not exist\")\nendif()\n"
           "get_target_property(_buildray_pch_type " + target + " TYPE)\n"
           "get_target_property(_buildray_pch_imported " + target + " IMPORTED)\n"
           "get_target_property(_buildray_pch_alias " + target + " ALIASED_TARGET)\n"
           "get_property(_buildray_pch_existing TARGET " + target + " PROPERTY PRECOMPILE_HEADERS SET)\n"
           "get_property(_buildray_pch_reuse TARGET " + target + " PROPERTY PRECOMPILE_HEADERS_REUSE_FROM SET)\n"
           "if(_buildray_pch_imported OR _buildray_pch_alias OR _buildray_pch_existing OR _buildray_pch_reuse\n"
           "   OR NOT _buildray_pch_type MATCHES \"^(EXECUTABLE|STATIC_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|OBJECT_LIBRARY)$\")\n"
           "  message(FATAL_ERROR \"Buildray requires a local compiled target without an existing PCH\")\nendif()\n"
           "set_property(TARGET " + target + " PROPERTY DISABLE_PRECOMPILE_HEADERS OFF)\n"
           "target_precompile_headers(" + target + " PRIVATE\n" + includes + ")\n";
    std::string guarded =
        "\n# Buildray: optional PCH experiment.\n"
        "option(BUILDRAY_PCH \"Enable the measured Buildray PCH\" OFF)\n"
        "if(BUILDRAY_PCH)\n";
    std::istringstream lines(experiment);
    for (std::string line; std::getline(lines, line);) guarded += "  " + line + '\n';
    return guarded + "endif()\n";
}
}  // namespace buildray
