//===--- CastCheck.h - clang-tidy -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_CHECKCHECK_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_CHECKCHECK_H

#include "../ClangTidyCheck.h"
#include "llvm/ADT/StringRef.h"
#include <string>

namespace clang::tidy::misc {

/// Suggest replacements for `obj.cast<type>()` with `cast<type>(obj)` along
/// with dyn_cast/dyn_cast_or_null/isa for all classes that support the
/// functional calls in MLIR. This is not a perfect check and is only a
/// temporary piece of code to ease migration.
class CastCheck : public ClangTidyCheck {
private:
  void printFixIt(CallExpr const *Call, StringRef ReplacementString);

public:
  CastCheck(StringRef Name, ClangTidyContext *Context);
  bool isLanguageVersionSupported(const LangOptions &LangOpts) const override {
    // XXX: I don't know what to put here, so always return true.
    return true;
  }
  void registerMatchers(ast_matchers::MatchFinder *Finder) override;
  void check(const ast_matchers::MatchFinder::MatchResult &Result) override;
};

} // namespace clang::tidy::misc

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_MISC_CHECKCHECK_H
