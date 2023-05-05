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
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/FormatVariadic.h"
#include <optional>
#include <string>

using namespace clang::ast_matchers;

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
  //
  // XXX: This should be a function instead of a macro.
#define ADD_MATCHERS(BASE_TYPE)                                                \
  Finder->addMatcher(                                                          \
      callExpr(                                                                \
          callee(memberExpr(hasObjectExpression(                               \
                                expr(hasType(cxxRecordDecl(                    \
                                         isSameOrDerivedFrom(BASE_TYPE))))     \
                                    .bind("Obj")),                             \
                            hasDeclaration(namedDecl(hasName("cast"))))        \
                     .bind("Callee")))                                         \
          .bind("Call"),                                                       \
      this);                                                                   \
  Finder->addMatcher(                                                          \
      callExpr(                                                                \
          callee(memberExpr(hasObjectExpression(                               \
                                expr(hasType(cxxRecordDecl(                    \
                                         isSameOrDerivedFrom(BASE_TYPE))))     \
                                    .bind("Obj")),                             \
                            hasDeclaration(namedDecl(hasName("dyn_cast"))))    \
                     .bind("Callee")))                                         \
          .bind("Call"),                                                       \
      this);                                                                   \
  Finder->addMatcher(                                                          \
      callExpr(                                                                \
          callee(memberExpr(hasObjectExpression(                               \
                                expr(hasType(cxxRecordDecl(                    \
                                         isSameOrDerivedFrom(BASE_TYPE))))     \
                                    .bind("Obj")),                             \
                            hasDeclaration(namedDecl(hasName("isa"))))         \
                     .bind("Callee")))                                         \
          .bind("Call"),                                                       \
      this);                                                                   \
  Finder->addMatcher(                                                          \
      callExpr(                                                                \
          callee(memberExpr(                                                   \
                     hasObjectExpression(                                      \
                         expr(hasType(cxxRecordDecl(                           \
                                  isSameOrDerivedFrom(BASE_TYPE))))            \
                             .bind("Obj")),                                    \
                     hasDeclaration(namedDecl(hasName("dyn_cast_or_null"))))   \
                     .bind("Callee")))                                         \
          .bind("Call"),                                                       \
      this);

  // Add matchers for the following list of all classes that support functional
  // casting (as far as I know).
  ADD_MATCHERS("::mlir::Attribute");
  ADD_MATCHERS("::mlir::Location");
  ADD_MATCHERS("::mlir::Op");
  ADD_MATCHERS("::mlir::Type");
  ADD_MATCHERS("::mlir::Value");

#undef ADD_MATCHERS
}

void CastCheck::check(const MatchFinder::MatchResult &Result) {
  // Get some matched tokens
  const auto *Call = Result.Nodes.getNodeAs<CallExpr>("Call");
  const auto *Callee = Result.Nodes.getNodeAs<MemberExpr>("Callee");

  // Get the string matching the entire matched object + method call
  SourceRange Range = Call->getSourceRange();
  std::string Name = Callee->getMemberNameInfo().getName().getAsString();
  llvm::StringRef Src = Lexer::getSourceText(
      CharSourceRange::getCharRange(Range), *Result.SourceManager,
      Result.Context->getLangOpts());

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
  if (Callee->isArrow()) {
    auto [Obj, Function] = Src.rsplit("->");
    Function.consume_front("template");
    std::string dst = (Function + "*" + Obj + ")").str();
    diag(Range.getBegin(), "Call '%0' is using methods instead of functions "
                           "https://mlir.llvm.org/deprecation/")
        << Name << FixItHint::CreateReplacement(Call->getSourceRange(), dst);

    return;
  }

  // isa has a variadic form and my hack to use the last period then fails.
  // Instead match on `.isa`
  if (Src.contains("...") && Src.contains("isa")) {
    auto [Obj, Function] = Src.split(".isa");
    Function.consume_front("template");
    std::string dst = ("isa" + Function + "" + Obj + ")").str();
    diag(Range.getBegin(), "Call '%0' is using methods instead of functions "
                           "https://mlir.llvm.org/deprecation/")
        << Name << FixItHint::CreateReplacement(Call->getSourceRange(), dst);
    return;
  }

  // This handles all the other cases that call the method on the object.
  auto [Obj, Function] = Src.rsplit('.');
  Function.consume_front("template");
  std::string dst = (Function + "" + Obj + ")").str();
  diag(Range.getBegin(), "Call '%0' is using methods instead of functions "
                         "https://mlir.llvm.org/deprecation/")
      << Name << FixItHint::CreateReplacement(Call->getSourceRange(), dst);
}

} // namespace clang::tidy::misc
