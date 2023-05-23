//===--- CastCheck.cpp - clang-tidy -------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CastCheck.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Lex/Lexer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FormatVariadic.h"
#include <optional>
#include <string>

using namespace clang::ast_matchers;
using llvm::StringRef;

namespace {
std::string maybePrefixed(StringRef Func) {
  std::string Prefix = "llvm::";
  return (Prefix + Func).str();
}

std::string transformObj(StringRef Obj, StringRef Function, bool IsArrow) {
  if (IsArrow) {
    llvm::errs() << "Func: " << Function << " and Obj: " << Obj;
    if (Function.trim().empty())
      return (Obj + "*this").str();
    return ("*" + Obj).str();
  }
  return Obj.str();
}
std::string transformFunction(StringRef Function, bool IsPointerUnion) {
  llvm::errs() << "Transform func(" << Function << ")";
  Function.consume_front("template ");
  Function = Function.trim();
  if (IsPointerUnion && Function.contains("dyn_cast") &&
      !Function.contains("dyn_cast_if_present")) {
    Function.consume_front("dyn_cast");
    return maybePrefixed(("dyn_cast_if_present" + Function).str());
  }
  if (Function.contains("dyn_cast_or_null")) {
    Function.consume_front("dyn_cast_or_null");
    return maybePrefixed(("dyn_cast_if_present" + Function).str());
  }
  // Inside a class declaraction, so we have to prefix. Due to bad parsing
  // logic elsewhere, Obj contains the function call string in these cases, so
  // just return the prefix
  if (Function.trim().empty())
    return "llvm::";
  return maybePrefixed(Function.str());
}
std::pair<std::string, std::string> splitCall(StringRef Call, bool IsArrow) {
  if (Call.contains("...") && Call.contains("isa")) {
    auto [Obj, Function] = IsArrow ? Call.rsplit("->isa") : Call.rsplit(".isa");
    return std::make_pair(Obj.str(), ("isa" + Function).str());
  }
  auto [Obj, Function] = IsArrow ? Call.rsplit("->") : Call.rsplit(".");
  return std::make_pair(Obj.str(), Function.str());
}
} // namespace

namespace clang::tidy::misc {

CastCheck::CastCheck(StringRef Name, ClangTidyContext *Context)
    : ClangTidyCheck(Name, Context) {}

void CastCheck::registerMatchers(MatchFinder *Finder) {
  // Define the list of methods/functions being refactored
  // (cast/dyn_cast/dyn_cast_or_null/isa).
  //
  // This uses matchers to find situations matching the following information:
  // 1. A method call
  // 2. The object calling a method is of one of the types that we know supports
  //    these functions by being of or deriving from the later list of types.
  // 3. The binding can be mostly ignored. I don't know what I'm doing, some
  //    binds only matched the beginning of the last token, so I did hacky
  //    string replacement instead of further understanding the code base.
  auto AddMatchers = [&](std::string BaseType, std::string CalleeBind) {
    Finder->addMatcher(
        callExpr(
            callee(memberExpr(hasDeclaration(namedDecl(allOf(
                                  hasUnderlyingDecl(matchesName(
                                      llvm::formatv("^{0}", BaseType).str())),
                                  hasName("cast")))))
                       .bind(CalleeBind)))
            .bind("Call"),
        this);
    Finder->addMatcher(
        callExpr(
            callee(memberExpr(hasDeclaration(namedDecl(allOf(
                                  hasUnderlyingDecl(matchesName(
                                      llvm::formatv("^{0}", BaseType).str())),
                                  hasName("dyn_cast")))))
                       .bind(CalleeBind)))
            .bind("Call"),
        this);
    Finder->addMatcher(
        callExpr(
            callee(memberExpr(hasDeclaration(namedDecl(allOf(
                                  hasUnderlyingDecl(matchesName(
                                      llvm::formatv("^{0}", BaseType).str())),
                                  hasName("isa")))))
                       .bind(CalleeBind)))
            .bind("Call"),
        this);
    Finder->addMatcher(
        callExpr(
            callee(memberExpr(hasDeclaration(namedDecl(allOf(
                                  hasUnderlyingDecl(matchesName(
                                      llvm::formatv("^{0}", BaseType).str())),
                                  hasName("dyn_cast_or_null")))))
                       .bind(CalleeBind)))
            .bind("Call"),
        this);
  };
  // Add matchers for the following list of all classes that support functional
  // casting (as far as I know).
  AddMatchers("::mlir::Attribute", "Callee");
  // AddMatchers("::mlir::Location");
  AddMatchers("::mlir::Op", "Callee");
  AddMatchers("::mlir::Type", "Callee");
  AddMatchers("::mlir::Value", "Callee");
  AddMatchers("::mlir::OpFoldResult", "Callee");
  AddMatchers("::llvm::PointerUnion", "PUCallee");
}

void CastCheck::printFixIt(CallExpr const *Call, StringRef ReplacementString) {
  SourceRange CallRange = Call->getSourceRange();
  diag(CallRange.getBegin(),
       "Casting call is using methods instead of functions "
       "https://mlir.llvm.org/deprecation/")
      << FixItHint::CreateReplacement(CallRange, ReplacementString);
}

void CastCheck::check(const MatchFinder::MatchResult &Result) {
  // Get some matched tokens
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("Call");
  const auto *Callee = Result.Nodes.getNodeAs<MemberExpr>("Callee");
  bool IsPointerUnion = false;
  if (!Callee) {
    IsPointerUnion = true;
    Callee = Result.Nodes.getNodeAs<MemberExpr>("PUCallee");
    assert(Callee && "The binding on the MemberExpr is using an unknown name");
  }

  // Get the string matching the entire matched object + method call
  SourceRange Range = Call->getSourceRange();
  std::string Name = Callee->getMemberNameInfo().getName().getAsString();
  llvm::StringRef Src = Lexer::getSourceText(
      CharSourceRange::getCharRange(Range), *Result.SourceManager,
      Result.Context->getLangOpts());
  bool IsArrow = Callee->isArrow();
  auto [Obj, Function] = splitCall(Src, IsArrow);
  Obj = transformObj(Obj, Function, IsArrow);
  Function = transformFunction(Function, IsPointerUnion);

  // XXX: Everything below is ugly and copy pasted instead of using helper
  // functions or more carefully ordering the code.
  //
  // Below, in every case, the following logic occurs:
  // 1. Remove uses of the `template` keyword which might occur
  // 2. Split the object from the method while crossing one's fingers that the
  //    last . or -> represents the casting method
  // 3. Suggest a FixItHint with a new string of the form $method(obj).
  // 4. Each case below will note the additional considerations they handle.

  // An arrow needs to be replaced with a dereferenced object, so split on arrow
  // instead and add a *.

  // isa has a variadic form and my hack to use the last period then fails.
  // Instead match on `.isa`
  // This handles all the other cases that call the method on the object.
  std::string dst = Function + Obj + ")";
  printFixIt(Call, dst);
}

} // namespace clang::tidy::misc
