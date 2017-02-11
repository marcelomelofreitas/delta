#include <unordered_set>
#include <llvm/Support/Path.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/Preprocessor.h>
#include <clang/Lex/HeaderSearch.h>
#include <clang/Parse/ParseAST.h>
#include <clang/AST/Decl.h>
#include <clang/AST/DeclGroup.h>
#include <clang/AST/Type.h>
#include <clang/AST/PrettyPrinter.h>
#include "c-import.h"
#include "typecheck.h"
#include "../ast/type.h"
#include "../ast/decl.h"

namespace {

clang::PrintingPolicy printingPolicy{clang::LangOptions()};

/** Prints the message to stderr if it hasn't been printed yet. */
void warnOnce(const llvm::Twine& message) {
    static std::unordered_set<std::string> printedMessages;
    llvm::SmallVector<char, 64> buffer;
    if (printedMessages.count(message.toStringRef(buffer)) != 0) return;
    llvm::errs() << "WARNING: " << *printedMessages.emplace(message.str()).first << '\n';
}

Type toDelta(clang::QualType qualtype) {
    const bool isMutable = !qualtype.isConstQualified();
    auto& type = *qualtype.getTypePtr();
    switch (type.getTypeClass()) {
        case clang::Type::Pointer: {
            auto pointeeType = llvm::cast<clang::PointerType>(type).getPointeeType();
            return Type(PtrType(llvm::make_unique<Type>(toDelta(pointeeType))), isMutable);
        }
        case clang::Type::Builtin:
            switch (llvm::cast<clang::BuiltinType>(type).getKind()) {
                case clang::BuiltinType::Void:  return Type(BasicType{"void"}, isMutable);
                case clang::BuiltinType::Bool:  return Type(BasicType{"bool"}, isMutable);
                case clang::BuiltinType::Char_S:
                case clang::BuiltinType::Char_U:return Type(BasicType{"char"}, isMutable);
                case clang::BuiltinType::Int:   return Type(BasicType{"int"},  isMutable);
                case clang::BuiltinType::UInt:  return Type(BasicType{"uint"}, isMutable);
                default:
                    auto name = llvm::cast<clang::BuiltinType>(type).getName(printingPolicy);
                    warnOnce("Builtin type '" + name + "' not handled");
                    return Type(BasicType{"int"}, isMutable);
            }
            return Type(BasicType{llvm::cast<clang::BuiltinType>(type).getName(printingPolicy)}, isMutable);
        case clang::Type::Typedef:
            return toDelta(llvm::cast<clang::TypedefType>(type).desugar());
        case clang::Type::Elaborated:
            return toDelta(llvm::cast<clang::ElaboratedType>(type).getNamedType());
        case clang::Type::Record:
            return Type(BasicType{llvm::cast<clang::RecordType>(type).getDecl()->getName()}, isMutable);
        default:
            warnOnce(llvm::Twine(type.getTypeClassName()) + " not handled");
            return Type(BasicType{"int"}, isMutable);
    }
}

FuncDecl toDelta(const clang::FunctionDecl& decl) {
    std::vector<ParamDecl> params;
    for (auto* param : decl.parameters()) {
        params.emplace_back(ParamDecl{"", toDelta(param->getType()),
                            param->getNameAsString(), SrcLoc::invalid()});
    }
    return FuncDecl{decl.getNameAsString(), std::move(params),
                    toDelta(decl.getReturnType()), "", nullptr, SrcLoc::invalid()};
}

llvm::Optional<FieldDecl> toDelta(const clang::FieldDecl& decl) {
    if (decl.getName().empty()) return llvm::None;
    return FieldDecl{toDelta(decl.getType()), decl.getNameAsString(), SrcLoc::invalid()};
}

llvm::Optional<TypeDecl> toDelta(const clang::RecordDecl& decl) {
    if (decl.getName().empty()) return llvm::None;
    TypeDecl typeDecl{TypeTag::Struct, decl.getNameAsString(), {}, SrcLoc::invalid()};
    typeDecl.fields.reserve(16); // TODO: Reserve based on the field count of `decl`.
    for (auto* field : decl.fields()) {
        if (auto fieldDecl = toDelta(*field)) {
            typeDecl.fields.emplace_back(std::move(*fieldDecl));
        } else {
            return llvm::None;
        }
    }
    return typeDecl;
}

class CToDeltaConverter : public clang::ASTConsumer {
public:
    bool HandleTopLevelDecl(clang::DeclGroupRef declGroup) final override {
        for (clang::Decl* decl : declGroup) {
            switch (decl->getKind()) {
                case clang::Decl::Function:
                    addToSymbolTable(toDelta(llvm::cast<clang::FunctionDecl>(*decl)));
                    break;
                case clang::Decl::Record: {
                    if (!decl->isFirstDecl()) break;
                    auto typeDecl = toDelta(llvm::cast<clang::RecordDecl>(*decl));
                    if (typeDecl) addToSymbolTable(std::move(*typeDecl));
                    break;
                }
                default:
                    break;
            }
        }
        return true; // continue parsing
    }
};

// FIXME: Temporary hack for finding Clang builtin includes.
// See http://clang.llvm.org/docs/FAQ.html#i-get-errors-about-some-headers-being-missing-stddef-h-stdarg-h
std::string getClangBuiltinIncludePath() {
    char path[64];
    std::shared_ptr<FILE> f(popen("dirname $(dirname $(which clang))", "r"), pclose);
    if (!f || fscanf(f.get(), "%63s", path) != 1) return "";

    char version[6];
    f.reset(popen("clang --version", "r"), pclose);
    if (!f || fscanf(f.get(), "clang version %5s", version) != 1) return "";

    return std::string(path) + "/lib/clang/" + version + "/include";
}

} // anonymous namespace

void importCHeader(llvm::StringRef headerName) {
    clang::CompilerInstance ci;
    clang::DiagnosticOptions diagnosticOptions;
    ci.createDiagnostics();

    std::shared_ptr<clang::TargetOptions> pto = std::make_shared<clang::TargetOptions>();
    pto->Triple = llvm::sys::getDefaultTargetTriple();
    clang::TargetInfo* pti = clang::TargetInfo::CreateTargetInfo(ci.getDiagnostics(), pto);
    ci.setTarget(pti);

    ci.createFileManager();
    ci.createSourceManager(ci.getFileManager());

    extern const char* currentFileName;
    llvm::StringRef importerDir = llvm::sys::path::parent_path(currentFileName);
    ci.getHeaderSearchOpts().AddPath(importerDir,          clang::frontend::Quoted, false, false);
    ci.getHeaderSearchOpts().AddPath("/usr/include",       clang::frontend::System, false, false);
    ci.getHeaderSearchOpts().AddPath("/usr/local/include", clang::frontend::System, false, false);

    std::string clangBuiltinIncludePath = getClangBuiltinIncludePath();
    if (!clangBuiltinIncludePath.empty()) {
        ci.getHeaderSearchOpts().AddPath(clangBuiltinIncludePath, clang::frontend::System, false, false);
    } else {
        llvm::errs() << "warning: clang not found, importing certain headers might not work\n";
    }

    ci.createPreprocessor(clang::TU_Complete);
    auto& pp = ci.getPreprocessor();
    pp.getBuiltinInfo().initializeBuiltins(pp.getIdentifierTable(), pp.getLangOpts());

    ci.setASTConsumer(llvm::make_unique<CToDeltaConverter>());
    ci.createASTContext();

    const clang::DirectoryLookup* curDir = nullptr;
    const clang::FileEntry* pFile = ci.getPreprocessor().getHeaderSearchInfo().LookupFile(
        headerName, {}, false, nullptr, curDir, {}, nullptr, nullptr, nullptr, nullptr);
    if (!pFile) {
        llvm::errs() << "error: couldn't find header '" << headerName << "'\n";
        abort();
    }

    ci.getSourceManager().setMainFileID(ci.getSourceManager().createFileID(
        pFile, clang::SourceLocation(), clang::SrcMgr::C_System));
    ci.getDiagnosticClient().BeginSourceFile(ci.getLangOpts(), &ci.getPreprocessor());
    clang::ParseAST(ci.getPreprocessor(), &ci.getASTConsumer(), ci.getASTContext());
    ci.getDiagnosticClient().EndSourceFile();
}