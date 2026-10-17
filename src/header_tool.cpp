#include <clang/AST/ASTConsumer.h>
#include <clang/AST/DeclCXX.h>
#include <clang/AST/DeclTemplate.h>
#include <clang/Basic/Version.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/FrontendActions.h>
#include <clang/Lex/PPCallbacks.h>
#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/JSONCompilationDatabase.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/JSON.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace json = llvm::json;
static_assert(CLANG_VERSION_MAJOR == LLVM_VERSION_MAJOR,
              "Clang and LLVM development headers must match");
namespace {
bool inside(const fs::path& file, const fs::path& root) {
    auto it = file.begin();
    for (const auto& part : root)
        if (it == file.end() || *it++ != part) return false;
    return true;
}
bool header(const std::string& path) {
    const auto ext = fs::path(path).extension();
    return ext == ".h" || ext == ".hpp" || ext == ".hh" || ext == ".hxx";
}
bool identifier(const std::string& name) {
    return !name.empty() &&
           (std::isalpha(static_cast<unsigned char>(name.front())) || name.front() == '_') &&
           std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) || c == '_'; });
}
struct Include {
    std::string file, provider;
    std::size_t offset, length;
};
struct Facts {
    fs::path root;
    std::set<std::string> headers;
    std::vector<Include> includes;
    std::map<std::pair<std::string, std::string>, std::set<std::string>> declarations;
    std::map<std::string, std::vector<std::pair<unsigned, unsigned>>> scopes;

    std::string path(clang::FileEntryRef entry) const {
        auto name = entry.getFileEntry().tryGetRealPathName();
        if (name.empty()) name = entry.getName();
        const auto file = fs::weakly_canonical(fs::path(name.str()));
        return inside(file, root) ? file.string() : "";
    }
    std::string path(clang::SourceLocation location, const clang::SourceManager& sm) const {
        if (location.isInvalid()) return "";
        const auto entry = sm.getFileEntryRefForID(sm.getFileID(sm.getExpansionLoc(location)));
        return entry ? path(*entry) : "";
    }
};

class Includes : public clang::PPCallbacks {
    Facts& facts;
    clang::SourceManager& sm;

   public:
    Includes(Facts& facts, clang::SourceManager& sm) : facts(facts), sm(sm) {}
    void InclusionDirective(clang::SourceLocation hash, const clang::Token&, llvm::StringRef, bool,
                            clang::CharSourceRange, clang::OptionalFileEntryRef file,
                            llvm::StringRef, llvm::StringRef, const clang::Module*, bool,
                            clang::SrcMgr::CharacteristicKind) override {
        if (!file || hash.isMacroID()) return;
        const auto provider = facts.path(*file), consumer = facts.path(hash, sm);
        if (header(provider)) facts.headers.insert(provider);
        if (!header(consumer)) return;
        facts.headers.insert(consumer);
        const auto buffer = sm.getBufferData(sm.getFileID(hash));
        const auto pos = sm.getFileOffset(hash);
        const auto previous = buffer.rfind('\n', pos);
        const auto start = previous == llvm::StringRef::npos ? 0 : previous + 1;
        const auto newline = buffer.find('\n', pos);
        const auto end = newline == llvm::StringRef::npos ? buffer.size() : newline + 1;
        facts.includes.push_back({consumer, provider, start, end - start});
    }
};

class Consumer : public clang::ASTConsumer {
    Facts& facts;
    clang::SourceManager* sm = nullptr;
    std::set<const clang::DeclContext*> visited;

    void reference(clang::QualType type, clang::SourceLocation location) {
        if (location.isMacroID() || type.isNull() ||
            (!type->isPointerType() && !type->isReferenceType()))
            return;
        const auto file = facts.path(location, *sm);
        if (!header(file)) return;
        const auto pointee = type->getPointeeType();
        // Typedefs and templates may require declarations other than a record tag.
        if (pointee->getAs<clang::TypedefType>()) return;
        const auto* record = pointee->getAsCXXRecordDecl();
        const auto* definition = record ? record->getDefinition() : nullptr;
        if (!definition || definition->hasAttrs() || definition->getDescribedClassTemplate() ||
            llvm::isa<clang::ClassTemplateSpecializationDecl>(definition) ||
            definition->isDependentContext())
            return;
        const auto provider = facts.path(definition->getLocation(), *sm);
        if (!header(provider) || provider == file) return;
        const auto name = definition->getNameAsString();
        if (!identifier(name) || (!definition->isClass() && !definition->isStruct())) return;
        std::vector<std::string> namespaces;
        const auto* context = definition->getDeclContext();
        while (const auto* ns = llvm::dyn_cast<clang::NamespaceDecl>(context)) {
            const auto part = ns->getNameAsString();
            if (!identifier(part) || part == "std" || ns->isInline() || ns->isAnonymousNamespace())
                return;
            namespaces.push_back(part);
            context = ns->getParent();
        }
        if (!context->isTranslationUnit()) return;  // Nested and local records stay untouched.
        std::string declaration;
        for (auto it = namespaces.rbegin(); it != namespaces.rend(); ++it)
            declaration += "namespace " + *it + " { ";
        declaration += std::string(definition->isClass() ? "class " : "struct ") + name + ";";
        for (std::size_t i = 0; i < namespaces.size(); ++i) declaration += " }";
        facts.declarations[{file, provider}].insert(std::move(declaration));
    }
    void walk(clang::DeclContext* context) {
        if (!visited.insert(context).second) return;
        for (auto* decl : context->decls()) {
            const auto file = facts.path(decl->getLocation(), *sm);
            if (file.empty() || decl->isImplicit()) continue;
            if (const auto* value = llvm::dyn_cast<clang::ValueDecl>(decl))
                reference(value->getType(), decl->getLocation());
            if (const auto* function = llvm::dyn_cast<clang::FunctionDecl>(decl)) {
                reference(function->getReturnType(), function->getLocation());
                for (const auto* parameter : function->parameters())
                    reference(parameter->getType(), parameter->getLocation());
            }
            if (llvm::isa<clang::NamespaceDecl, clang::RecordDecl, clang::LinkageSpecDecl>(decl)) {
                const auto begin = sm->getExpansionLoc(decl->getBeginLoc());
                const auto end = sm->getExpansionLoc(decl->getEndLoc());
                if (header(file) && facts.path(begin, *sm) == file && facts.path(end, *sm) == file)
                    facts.scopes[file].emplace_back(sm->getFileOffset(begin),
                                                    sm->getFileOffset(end));
                walk(clang::Decl::castToDeclContext(decl));
            }
        }
    }

   public:
    explicit Consumer(Facts& facts) : facts(facts) {}
    void HandleTranslationUnit(clang::ASTContext& context) override {
        sm = &context.getSourceManager();
        walk(context.getTranslationUnitDecl());
    }
};

class Action : public clang::ASTFrontendAction {
    Facts& facts;

   public:
    explicit Action(Facts& facts) : facts(facts) {}
    std::unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& compiler,
                                                          llvm::StringRef) override {
        compiler.getPreprocessor().addPPCallbacks(
            std::make_unique<Includes>(facts, compiler.getSourceManager()));
        return std::make_unique<Consumer>(facts);
    }
};
class Factory : public clang::tooling::FrontendActionFactory {
    Facts& facts;

   public:
    explicit Factory(Facts& facts) : facts(facts) {}
    std::unique_ptr<clang::FrontendAction> create() override {
        return std::make_unique<Action>(facts);
    }
};
}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--version") {
            llvm::outs() << json::Value(json::Object{{"schema_version", 1},
                                                     {"clang_major", CLANG_VERSION_MAJOR}})
                         << '\n';
            return 0;
        }
        if (argc != 6)
            throw std::runtime_error(
                "Usage: buildray-header-tool DATABASE SOURCE ROOT OUTPUT RESOURCE_DIR");
        std::string error;
        auto database = clang::tooling::JSONCompilationDatabase::loadFromFile(
            argv[1], error, clang::tooling::JSONCommandLineSyntax::AutoDetect);
        if (!database) throw std::runtime_error(error);
        Facts facts;
        facts.root = fs::canonical(argv[3]);
        clang::tooling::ClangTool tool(*database, {argv[2]});
        tool.appendArgumentsAdjuster(clang::tooling::getClangStripDependencyFileAdjuster());
        tool.appendArgumentsAdjuster(clang::tooling::getInsertArgumentAdjuster(
            clang::tooling::CommandLineArguments{"-resource-dir=" + std::string(argv[5])},
            clang::tooling::ArgumentInsertPosition::BEGIN));
        Factory factory(facts);
        if (tool.run(&factory) != 0) return 1;
        json::Array headers, includes, forwards;
        for (const auto& file : facts.headers) headers.push_back(file);
        for (const auto& include : facts.includes) {
            json::Object item{{"header", include.file},
                              {"provider", include.provider},
                              {"offset", static_cast<std::int64_t>(include.offset)},
                              {"length", static_cast<std::int64_t>(include.length)}};
            includes.push_back(json::Object(item));
            const bool scoped =
                std::ranges::any_of(facts.scopes[include.file], [&](const auto& range) {
                    return range.first <= include.offset && include.offset <= range.second;
                });
            const auto found = facts.declarations.find({include.file, include.provider});
            if (scoped || found == facts.declarations.end()) continue;
            json::Array declarations;
            for (const auto& declaration : found->second) declarations.push_back(declaration);
            item["declarations"] = std::move(declarations);
            forwards.push_back(std::move(item));
        }
        std::ofstream out(argv[4], std::ios::binary);
        if (!out) throw std::runtime_error("Cannot write header facts");
        std::string data;
        llvm::raw_string_ostream stream(data);
        stream << json::Value(json::Object{{"schema_version", 1},
                                           {"clang_major", CLANG_VERSION_MAJOR},
                                           {"headers", std::move(headers)},
                                           {"includes", std::move(includes)},
                                           {"forwards", std::move(forwards)}});
        out << data << '\n';
        if (!out) throw std::runtime_error("Cannot finish header facts");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "buildray-header-tool: " << error.what() << '\n';
        return 1;
    }
}
