//===--- Passes/LFenceInsertion.cpp-------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class implements a pass that inserts LFENCE instructions before each
// conditional branch to protect against Spectre Variant 1.
// The performance impact of this is significant!
//===----------------------------------------------------------------------===//
#include "LFenceInsertion.h"
#include "RewriteInstance.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "bolt-lfence"

using namespace llvm;
using namespace bolt;
namespace opts {
extern bool shouldProcess(const bolt::BinaryFunction &Function);

extern cl::OptionCategory BoltCategory;

llvm::cl::opt<bool>
InsertLFences("insert-lfences",
  cl::desc("run lfence insertion pass"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

} // namespace opts

namespace llvm {
namespace bolt {

// Inserts a lfence before every conditional branch.
// For example:
//   cmp %reg1, %reg2
//   je <jump_dest>
// gets rewritten to:
//   cmp %reg1, %reg2
//   lfence
//   je <jump_dest>
void LFenceInsertion::runOnFunctions(BinaryContext &BC) {

  if (!opts::InsertLFences)
    return;

  assert(BC.isX86() &&
         "lfence insertion not supported for target architecture");

  assert(BC.HasRelocations && "lfence mode not supported in non-reloc");

  auto &MIB = *BC.MIB;
  uint32_t LFencedBranches = 0;
  for (auto &It : BC.getBinaryFunctions()) {
    auto &Function = It.second;

    // For performance reasons, we may want to skip some functions and
    // manually add lfences to them only where absolutely needed.
    if (!opts::shouldProcess(Function))
      continue;

    for (auto &BB : Function) {
      for (auto It = BB.begin(); It != BB.end(); ++It) {
        auto &Inst = *It;

        if (!MIB.isConditionalBranch(Inst))
          continue;

        MCInst LFence;
        MIB.createLfence(LFence);
        It = BB.insertInstruction(It, std::move(LFence));
        ++It;
        LFencedBranches++;
      }
    }
  }
  outs() << "\nBOLT-INFO: The number of lfenced branches is : " << LFencedBranches
         << "\n";
}

} // namespace bolt
} // namespace llvm
