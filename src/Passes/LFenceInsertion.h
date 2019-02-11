//===--- Passes/LFenceInsertion.h ---------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_BOLT_LFENCE_INSERTION_H
#define LLVM_TOOLS_LLVM_BOLT_LFENCE_INSERTION_H

#include "BinaryPasses.h"
#include "BinarySection.h"
#include <string>
#include <unordered_map>

namespace llvm {
namespace bolt {

class LFenceInsertion : public BinaryFunctionPass {
private:

public:
  explicit LFenceInsertion() : BinaryFunctionPass(false) {}

  const char *getName() const override { return "lfence-insertion"; }

  void runOnFunctions(BinaryContext &BC,
                      std::map<uint64_t, BinaryFunction> &BFs,
                      std::set<uint64_t> &LargeFunctions) override;
};
} // namespace bolt
} // namespace llvm

#endif
