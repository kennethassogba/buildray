#include "buildray/core.hpp"

#include <llvm/Support/Error.h>
#include <llvm/Support/YAMLTraits.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <regex>
#include <stdexcept>
#include <utility>

namespace buildray {
struct FixMessage {
    std::vector<Replacement> replacements;
};
struct FixDiagnostic {
    std::string name;
    FixMessage message;
};
struct FixDocument {
    std::vector<FixDiagnostic> diagnostics;
};
}  // namespace buildray

LLVM_YAML_IS_SEQUENCE_VECTOR(buildray::Replacement)
LLVM_YAML_IS_SEQUENCE_VECTOR(buildray::FixDiagnostic)

namespace llvm::yaml {
template <>
struct MappingTraits<buildray::Replacement> {
    static void mapping(IO& io, buildray::Replacement& item) {
        io.mapRequired("FilePath", item.file);
        io.mapRequired("Offset", item.offset);
        io.mapRequired("Length", item.length);
        io.mapRequired("ReplacementText", item.text);
    }
};
template <>
struct MappingTraits<buildray::FixMessage> {
    static void mapping(IO& io, buildray::FixMessage& item) {
        io.mapOptional("Replacements", item.replacements);
    }
};
template <>
struct MappingTraits<buildray::FixDiagnostic> {
    static void mapping(IO& io, buildray::FixDiagnostic& item) {
        io.mapRequired("DiagnosticName", item.name);
        io.mapRequired("DiagnosticMessage", item.message);
    }
};
template <>
struct MappingTraits<buildray::FixDocument> {
    static void mapping(IO& io, buildray::FixDocument& item) {
        io.mapOptional("Diagnostics", item.diagnostics);
    }
};
}  // namespace llvm::yaml

namespace buildray {
std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("Cannot read " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void write_file(const fs::path& path, const std::string& data) {
    fs::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file || !(file << data)) throw std::runtime_error("Cannot write " + path.string());
}

json::Value read_json(const fs::path& path) {
    auto value = json::parse(read_file(path));
    if (!value)
        throw std::runtime_error("Invalid JSON in " + path.string() + ": " +
                                 llvm::toString(value.takeError()));
    return std::move(*value);
}

void write_json(const fs::path& path, json::Value value) {
    std::string text;
    llvm::raw_string_ostream stream(text);
    stream << llvm::formatv("{0:2}", value) << '\n';
    write_file(path, text);
}

fs::path absolute_path(const fs::path& path) { return fs::weakly_canonical(fs::absolute(path)); }

bool within(const fs::path& path, const fs::path& parent) {
    auto child = path.begin();
    for (const auto& component : parent) {
        if (child == path.end() || *child != component) return false;
        ++child;
    }
    return true;
}

std::vector<Replacement> read_fixes(const fs::path& path) {
    if (!fs::exists(path)) return {};
    const auto content = read_file(path);
    llvm::yaml::Input input(content, nullptr, [](const llvm::SMDiagnostic& diagnostic, void*) {
        // clang-tidy exports metadata beyond the replacements we consume.
        if (diagnostic.getKind() == llvm::SourceMgr::DK_Error)
            diagnostic.print("buildray", llvm::errs());
    });
    input.setAllowUnknownKeys(true);
    FixDocument document;
    input >> document;
    if (input.error()) throw std::runtime_error("Invalid clang-tidy fixes: " + path.string());
    std::vector<Replacement> result;
    for (auto& diagnostic : document.diagnostics) {
        if (diagnostic.name != "misc-include-cleaner") continue;
        for (auto& replacement : diagnostic.message.replacements) {
            result.push_back(std::move(replacement));
        }
    }
    return result;
}

bool removable_include(const std::string& source, const Replacement& item) {
    if (!item.text.empty() || item.length == 0 || item.offset > source.size() ||
        item.length > source.size() - item.offset)
        return false;
    if (item.offset > 0 && source[item.offset - 1] != '\n') return false;
    const auto end = item.offset + item.length;
    if (end < source.size() && source[end - 1] != '\n') return false;
    const auto line = source.substr(item.offset, item.length);
    // Restrict automatic fixes to complete, single-line include directives.
    // In particular, do not remove a macro include or a continued directive.
    static const std::regex directive(
        R"(^[ \t]*#[ \t]*include[ \t]*([<][^>\r\n]+[>]|["][^"\r\n]+["])[ \t]*(//[^\r\n]*)?\r?\n?$)");
    return line.find("IWYU pragma:") == std::string::npos && line.find('\\') == std::string::npos &&
           std::regex_match(line, directive);
}

std::string apply_removals(std::string source, std::vector<Replacement> items) {
    std::ranges::sort(items);
    items.erase(std::unique(items.begin(), items.end()), items.end());
    std::size_t last = 0;
    for (const auto& item : items) {
        if (item.offset < last || !removable_include(source, item))
            throw std::runtime_error("Unsafe or overlapping include replacement");
        last = item.offset + item.length;
    }
    for (auto i = items.rbegin(); i != items.rend(); ++i) source.erase(i->offset, i->length);
    return source;
}

double median(std::vector<double> values) {
    if (values.empty()) throw std::runtime_error("Cannot summarize zero measurements");
    std::ranges::sort(values);
    const auto middle = values.size() / 2;
    return values.size() % 2 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
}

double mad(const std::vector<double>& values) {
    const auto center = median(values);
    std::vector<double> deviations;
    for (double value : values) deviations.push_back(std::abs(value - center));
    return median(std::move(deviations));
}
}  // namespace buildray
