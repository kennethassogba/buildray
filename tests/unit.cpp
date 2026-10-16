#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>

#include "buildray/core.hpp"

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template <class Function>
void rejects(Function action, const char* message) {
    try {
        action();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(message);
}
buildray::json::Value measurement(std::vector<double> times) {
    using namespace buildray;
    json::Array samples;
    for (const auto value : times) samples.push_back(json::Object{{"wall_seconds", value}});
    return json::Object{
        {"schema_version", 1},
        {"command", "measure"},
        {"status", "passed"},
        {"runs", static_cast<std::int64_t>(times.size())},
        {"baseline_tests_passed", 1},
        {"compiler_version", "test compiler"},
        {"cmake_version", "test cmake"},
        {"ninja_version", "test ninja"},
        {"baseline", json::Object{{"samples", std::move(samples)}, {"median_seconds", 99999}}},
        {"measurement_context", json::Object{{"method", "clean-build-warm-filesystem-v1"},
                                             {"buildray_version", BUILDRAY_VERSION},
                                             {"sampling", "paired_alternating"},
                                             {"session", "unit"},
                                             {"jobs", 2},
                                             {"cpu_affinity_count", 2},
                                             {"cpu_model", "unit CPU"},
                                             {"architecture", "test"},
                                             {"kernel", "test kernel"},
                                             {"cpu_quota", "max"},
                                             {"tests_required", true},
                                             {"cmake_args", json::Array{}},
                                             {"cache_flags", json::Object{}},
                                             {"environment_flags", json::Object{}}}}};
}
}  // namespace

int main() {
    using namespace buildray;
    const auto temporary =
        fs::temp_directory_path() / ("buildray-unit-" + std::to_string(::getpid()));
    try {
        require(median({8, 1, 3}) == 3, "odd median");
        require(median({8, 1, 3, 4}) == 3.5, "even median");
        require(mad({1, 2, 3}) == 1, "MAD");
        rejects([] { median({}); }, "empty median must fail");
        const auto before = measurement({10, 10.01, 9.99});
        const auto slow = measurement({12, 12.01, 11.99});
        const auto regression = compare_reports(before, slow, 5);
        require(regression.exit_code == 3 && !*regression.report.getBoolean("gate_passed"),
                "regression gate");
        require(std::abs(*regression.report.getNumber("wall_change_percent") - 20) < 0.0001,
                "recompute statistics from raw samples, not reported medians");
        require(compare_reports(before, slow, 25).report.getString("performance_result") ==
                    "within_budget",
                "budget respected");
        require(compare_reports(before, measurement({9, 9.01, 8.99}), 5)
                        .report.getString("performance_result") == "improved",
                "improvement");
        require(compare_reports(measurement({5, 10, 15}), measurement({6, 11, 16}), 0)
                        .report.getString("performance_result") == "inconclusive",
                "noise must not fail the build");
        rejects([&] { compare_reports(before, measurement({1, 2}), 5); }, "short sample set");
        rejects([&] { compare_reports(before, measurement({0, 1, 2}), 5); }, "invalid sample");
        rejects([&] { compare_reports(before, slow, -1); }, "negative budget");
        rejects([&] { compare_reports(before, measurement({1e308, 1e308, 1e308}), 5); },
                "statistics overflow must fail");
        auto malformed = measurement({10, 10, 10});
        (*malformed.getAsObject())["compiler_version"] = nullptr;
        rejects([&] { compare_reports(malformed, malformed, 5); }, "tool version type");
        auto incompatible = measurement({10, 10, 10});
        (*incompatible.getAsObject()->getObject("measurement_context"))["jobs"] = 8;
        rejects([&] { compare_reports(before, incompatible, 5); }, "different runner context");
        incompatible = measurement({10, 10, 10});
        (*incompatible.getAsObject())["compiler_version"] = "other compiler";
        rejects([&] { compare_reports(before, incompatible, 5); }, "different toolchain");
        incompatible = measurement({10, 10, 10});
        incompatible.getAsObject()->erase("measurement_context");
        rejects([&] { compare_reports(before, incompatible, 5); },
                "legacy reports must be refused");
        incompatible = measurement({10, 10, 10});
        (*incompatible.getAsObject())["status"] = "failed";
        rejects([&] { compare_reports(before, incompatible, 5); }, "failed measurements");
        incompatible = measurement({10, 10, 10});
        (*incompatible.getAsObject())["baseline_tests_passed"] = 0;
        rejects([&] { compare_reports(before, incompatible, 5); }, "missing test validation");
        require(within("/a/b/c", "/a/b"), "containment");
        require(!within("/a/bad", "/a/b"), "containment must compare components");
        const std::string source =
            "#include <vector>\r\n#include \"local.hpp\"\r\nint main() {}\r\n";
        Replacement first{"a.cpp", 0, 19, ""};
        Replacement second{"a.cpp", 19, 22, ""};
        require(removable_include(source, first), "CRLF include removal");
        require(removable_include(source, second), "quoted include removal");
        require(apply_removals(source, {second, first, first}) == "int main() {}\r\n",
                "descending replacements and deduplication");
        require(!removable_include(source, {"a.cpp", 1, 18, ""}), "partial line");
        require(!removable_include(source, {"a.cpp", 0, source.size() + 1, ""}), "bounds");
        require(!removable_include(source, {"a.cpp", 0, 19, "injected"}), "no insertions");
        const std::string kept = "#include <vector> // IWYU pragma: keep\n";
        require(!removable_include(kept, {"a", 0, kept.size(), ""}), "keep pragma");
        const std::string continuation = "#include \\\n<vector>\n";
        require(!removable_include(continuation, {"a", 0, continuation.size(), ""}),
                "continued include");
        const std::string macro = "#include SOME_HEADER\n";
        require(!removable_include(macro, {"a", 0, macro.size(), ""}), "macro include");
        for (const auto* header : {"body.inc", "body.inl", "body.ipp", "body.tpp", "body_inl.h",
                                   "body-inl.h", "body.inl.h"}) {
            const std::string implementation = std::string("#include \"") + header + "\"\n";
            require(!removable_include(implementation, {"a", 0, implementation.size(), ""}),
                    "keep implementation headers that may provide template definitions");
        }
        rejects([&] { apply_removals(source, {first, {"a.cpp", 0, 41, ""}}); },
                "overlap must fail");
        fs::create_directory(temporary);
        write_file(temporary / "fixes.yaml", R"(---
MainSourceFile: '/a.cpp'
Diagnostics:
  - DiagnosticName: misc-include-cleaner
    DiagnosticMessage:
      Message: 'unused'
      FilePath: '/a.cpp'
      FileOffset: 0
      Replacements:
        - FilePath: '/a.cpp'
          Offset: 0
          Length: 18
          ReplacementText: ''
        - FilePath: '/a.cpp'
          Offset: 0
          Length: 0
          ReplacementText: '#include <cstdint>'
    Notes: []
    Level: Warning
    BuildDirectory: '/tmp'
  - DiagnosticName: other-check
    DiagnosticMessage:
      Replacements:
        - FilePath: '/a.cpp'
          Offset: 0
          Length: 18
          ReplacementText: ''
...
)");
        const auto fixes = read_fixes(temporary / "fixes.yaml");
        require(fixes.size() == 2 && fixes[0].length == 18 && !fixes[1].text.empty(),
                "read provider insertions too; only removable_include authorizes deletions");
        write_file(temporary / "fixes.yaml", "Diagnostics: [unterminated");
        rejects([&] { read_fixes(temporary / "fixes.yaml"); }, "malformed YAML rejected");
        const auto records = temporary / "traces";
        fs::create_directories(records);
        write_json(records / "one.json", json::Object{
            {"exit_code", 0}, {"directory", temporary.string()},
            {"trace_file", (records / "one.trace").string()},
            {"command", json::Array{"clang++", "-o", "sub/CMakeFiles/core.dir/a.cpp.o"}}});
        write_file(records / "one.trace", R"({"traceEvents":[
          {"name":"Source","ph":"X","dur":200,"args":{"detail":"outer.hpp"}},
          {"name":"Source","ph":"b","pid":1,"tid":1,"id":0,"ts":20,"args":{"detail":"inner.hpp"}},
          {"name":"Source","ph":"e","pid":1,"tid":1,"id":0,"ts":120},
          {"name":"Total Source","ph":"X","dur":999999},
          {"name":"Source","ph":"X","dur":50,"args":{"detail":"inner.hpp"}}
        ]})");
        const auto profile = header_profile(records, temporary, temporary / "build");
        const auto& rows = *profile.getArray("headers");
        require(rows.size() == 2 && rows[0].getAsObject()->getString("header") == "<SOURCE>/outer.hpp",
                "rank inclusive events without adding Total Source summaries");
        const auto& inner = *rows[1].getAsObject();
        require(*inner.getInteger("events") == 2 && *inner.getInteger("compilations") == 1 &&
                    std::abs(*inner.getNumber("inclusive_seconds") - 0.00015) < 1e-10,
                "Clang async and complete events, repeated headers count once per compilation");
        write_file(records / "one.trace", R"({"traceEvents":[
          {"name":"Source","ph":"e","pid":1,"tid":1,"id":0,"ts":120}
        ]})");
        rejects([&] { header_profile(records, temporary, temporary / "build"); },
                "unpaired trace events cannot produce partial rankings");
        const auto success = run({"/bin/sh", "-c", "exit 0"}, temporary);
        require(success.code == 0 && success.seconds > 0, "process success and timing");
        require(run({"/bin/sh", "-c", "exit 7"}, temporary).code == 7, "exit status preserved");
        const auto literal = temporary / "literal.txt";
        require(
            run({"/usr/bin/printf", "%s", "$(touch injected); spaced argument"}, temporary, literal)
                    .code == 0,
            "literal arguments");
        require(read_file(literal) == "$(touch injected); spaced argument" &&
                    !fs::exists(temporary / "injected"),
                "arguments must never be interpreted by a shell");
        require(
            run({"/no/such/buildray-command"}, temporary, temporary / "missing.log").code == 127,
            "missing executable");
        fs::remove_all(temporary);
        std::cout << "Unit checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        fs::remove_all(temporary);
        std::cerr << error.what() << '\n';
        return 1;
    }
}
