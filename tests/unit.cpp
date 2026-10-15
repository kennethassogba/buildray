#include <unistd.h>

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
        require(fixes.size() == 1 && fixes[0].length == 18, "structured fixes filtered by check");
        write_file(temporary / "fixes.yaml", "Diagnostics: [unterminated");
        rejects([&] { read_fixes(temporary / "fixes.yaml"); }, "malformed YAML rejected");
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
