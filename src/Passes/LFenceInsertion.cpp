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
// conditional branch to protect against Spectre Variant 1, as well as the
// various LVI mitigations.
//
// The runtime performance impact of this is significant!
//
// NOTE: This pass is incompatible with RetpolineInsertion.
//===----------------------------------------------------------------------===//
#include "LFenceInsertion.h"
#include "RewriteInstance.h"
#include "RetpolineInsertion.h" //IndirectBranchInfo
#include "ParallelUtilities.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "bolt-lfence"

using namespace llvm;
using namespace bolt;
namespace opts {

extern cl::OptionCategory BoltCategory;

llvm::cl::opt<bool>
InsertLFences("insert-lfences",
  cl::desc("run lfence insertion pass"),
  cl::init(false),
  cl::ZeroOrMore,
  cl::cat(BoltCategory));

llvm::cl::opt<bool>
LFenceConditionalBranches("lfence-conditional-branches",
  cl::desc("determine if all conditional branches should be lfence mitigated"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));
llvm::cl::opt<bool>
LFenceLoads("lfence-loads",
  cl::desc("determine if all loads should be lfence mitigated"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));
llvm::cl::opt<bool>
LFenceReturns("lfence-returns",
  cl::desc("determine if all returns should be lfence mitigated"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));
llvm::cl::opt<bool>
LFenceIndirectCalls("lfence-indirect-calls",
  cl::desc("determine if all indirect calls should be lfence mitigated"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));
llvm::cl::opt<bool>
LFenceIndirectJumps("lfence-indirect-jumps",
  cl::desc("determine if all indirect jumps should be lfence mitigated"),
  cl::init(true),
  cl::ZeroOrMore,
  cl::Hidden,
  cl::cat(BoltCategory));

} // namespace opts

namespace llvm {
namespace bolt {

void LFenceInsertion::runOnFunctions(BinaryContext &BC) {

  if (!opts::InsertLFences)
    return;

  assert(BC.isX86() &&
         "lfence insertion not supported for target architecture");

  assert(BC.HasRelocations && "lfence mode not supported in non-reloc");

  auto &MIB = *BC.MIB;
  uint32_t LFencedBranches = 0;
  uint32_t LFencedLoads = 0;
  uint32_t LFencedRets = 0;
  uint32_t LFencedIndirectCalls = 0;
  uint32_t LFencedIndirectJmps = 0;
  for (auto &It : BC.getBinaryFunctions()) {
    auto &Function = It.second;

    // For performance reasons, we may want to skip some functions and
    // manually add lfences to them only where absolutely needed.
    if (Function.isIgnored())
      continue;

    for (auto &BB : Function) {
      bool LastWasLFence = false;
      for (auto It = BB.begin(); It != BB.end(); ++It) {
        auto &Inst = *It;

        if (opts::LFenceConditionalBranches &&
            MIB.isConditionalBranch(Inst)) {
          // Inserts a lfence before every conditional branch.
          // For example:
          //   cmp %reg1, %reg2
          //   je <jump_dest>
          // gets rewritten to:
          //   cmp %reg1, %reg2
          //   lfence
          //   je <jump_dest>
          if (!LastWasLFence) {
            MCInst LFence;
            MIB.createLfence(LFence);
            It = BB.insertInstruction(It, std::move(LFence));
            ++It;
          }
          LFencedBranches++;
          LastWasLFence = false;
        } else if (opts::LFenceLoads &&
                   MIB.isLoad(Inst) && !MIB.isIndirectBranch(Inst) && !MIB.isIndirectCall(Inst)) {
          // Inserts an lfence after every load from memory.
          // For example:
          //   mov    0x8(%rbx), %rdi
          // Gets rewritten to:
          //   mov    0x8(%rbx), %rdi
          //   lfence
          ++It;
          MCInst LFence;
          MIB.createLfence(LFence);
          It = BB.insertInstruction(It, std::move(LFence));
          LFencedLoads++;
          LastWasLFence = true;
        } else if (opts::LFenceReturns &&
                   MIB.isReturn(Inst) && !MIB.isIndirectBranch(Inst)) {
          // Inserts a dummy write + lfence before every ret.
          // For example:
          //   retq
          // gets rewritten to:
          //   notq (%rsp)
          //   notq (%rsp)
          //   lfence
          //   retq
          for (int i = 0; i < 2; i++) {
            MCInst Notq;
            MIB.createNot(Notq, MIB.getStackPointer(), 1, MIB.getNoRegister(), 0, nullptr,
                          MIB.getNoRegister(), 8);
            It = BB.insertInstruction(It, std::move(Notq));
            ++It;
          }
          MCInst LFence;
          MIB.createLfence(LFence);
          It = BB.insertInstruction(It, std::move(LFence));
          ++It;
          LFencedRets++;
          LastWasLFence = false;
        } else if (opts::LFenceIndirectCalls &&
                   MIB.isIndirectCall(Inst) && MIB.isLoad(Inst) && !MIB.isIndirectBranch(Inst)) {
          // Translates indirect calls into lea/mov/jmp then applies the jmp mitigation.
          // For example:
          //   callq *(%rsi)
          // gets rewritten to:
          //   pushq %rdi //Dummy to overwrite later
          //   pushq %rdi
          //   leaq 0x18(%rip), %rdi //After the retq
          //   mov %rax, 8(%rsp) //Overwrite the dummy
          //   popq %rdi
          //   lfence
          //   pushq (%rsi)
          //   notq (%rsp)
          //   notq (%rsp)
          //   lfence
          //   retq

          IndirectBranchInfo BrInfo(Inst, MIB);
          const auto &MemRef = BrInfo.Memory;
          auto *Ctx = BC.Ctx.get();
          assert(BrInfo.isMem());

          // Create a separate MCCodeEmitter to allow lock-free execution
          BinaryContext::IndependentCodeEmitter Emitter;
          if (!opts::NoThreads) {
            Emitter = BC.createIndependentMCCodeEmitter();
          }

          int offset = 0x15 + BC.computeCodeSize(It, std::next(It), Emitter.MCE.get());

          MCPhysReg ScratchReg = MIB.getIntArgRegister(0);
          MCInst Pushq1; //Dummy, to overwrite later.
          MIB.createPushRegister(Pushq1, ScratchReg, 8);
          It = BB.insertInstruction(It, std::move(Pushq1));
          ++It;
          MCInst Pushq2;
          MIB.createPushRegister(Pushq2, ScratchReg, 8);
          It = BB.insertInstruction(It, std::move(Pushq2));
          ++It;
          MCInst Leaq;
          MIB.createLea(Leaq, MIB.getInstructionPointer(), 1, MIB.getNoRegister(),
                        offset, nullptr, MIB.getNoRegister(), ScratchReg, 8);
          It = BB.insertInstruction(It, std::move(Leaq));
          ++It;
          MCInst Movq;
          MIB.createSaveToStack(Movq, MIB.getStackPointer(), 8, ScratchReg, 8);
          It = BB.insertInstruction(It, std::move(Movq));
          ++It;
          MCInst Popq;
          MIB.createPopRegister(Popq, ScratchReg, 8);
          It = BB.insertInstruction(It, std::move(Popq));
          ++It;
          MCInst LFence1;
          MIB.createLfence(LFence1);
          It = BB.insertInstruction(It, std::move(LFence1));
          ++It;
          MCInst Pushq3;
          MIB.createPushRegisterIndirect(Pushq3, MemRef.BaseRegNum, MemRef.ScaleValue,
                                         MemRef.IndexRegNum, MemRef.DispValue, MemRef.DispExpr,
                                         MemRef.SegRegNum, 8);
          It = BB.insertInstruction(It, std::move(Pushq3));
          ++It;
          for (int i = 0; i < 2; i++) {
            MCInst Notq;
            MIB.createNot(Notq, MIB.getStackPointer(), 1, MIB.getNoRegister(), 0, nullptr,
                          MIB.getNoRegister(), 8);
            It = BB.insertInstruction(It, std::move(Notq));
            ++It;
          }
          MCInst LFence2;
          MIB.createLfence(LFence2);
          It = BB.insertInstruction(It, std::move(LFence2));
          ++It;
          MCInst Retq;
          MIB.createReturn(Retq);
          BB.replaceInstruction(It, std::vector<MCInst>({Retq}));
          LFencedIndirectCalls++;
          LastWasLFence = false;
        } else if (opts::LFenceIndirectJumps &&
                   MIB.isIndirectBranch(Inst) && MIB.isLoad(Inst)) {
          // Maps indirect jumps to "push; ret", then applies ret mitigation.
          // For example:
          //   jmpq *(%rsi)
          // gets rewritten to:
          //   pushq (%rsi)
          //   notq (%rsp)
          //   notq (%rsp)
          //   lfence
          //   retq
          IndirectBranchInfo BrInfo(Inst, MIB);
          const auto &MemRef = BrInfo.Memory;

          MCInst Push;
          MIB.createPushRegisterIndirect(Push, MemRef.BaseRegNum, MemRef.ScaleValue,
                                         MemRef.IndexRegNum, MemRef.DispValue, MemRef.DispExpr,
                                         MemRef.SegRegNum, 8);
          It = BB.insertInstruction(It, std::move(Push));
          ++It;
          for (int i = 0; i < 2; i++) {
            MCInst Notq;
            MIB.createNot(Notq, MIB.getStackPointer(), 1, MIB.getNoRegister(), 0, nullptr,
                          MIB.getNoRegister(), 8);
            It = BB.insertInstruction(It, std::move(Notq));
            ++It;
          }
          MCInst LFence;
          MIB.createLfence(LFence);
          It = BB.insertInstruction(It, std::move(LFence));
          ++It;
          MCInst Retq;
          MIB.createReturn(Retq);
          BB.replaceInstruction(It, std::vector<MCInst>({Retq}));
          LFencedIndirectJmps++;
          LastWasLFence = false;
        } else if (MIB.isLfence(Inst)) {
          LastWasLFence = true;
        } else {
          LastWasLFence = false;
        }
      }
    }
  }

  outs() << "\nBOLT-INFO: The number of lfenced branches is : " << LFencedBranches;
  outs() << "\nBOLT-INFO: The number of lfenced loads is : " << LFencedLoads;
  outs() << "\nBOLT-INFO: The number of lfenced rets is : " << LFencedRets;
  outs() << "\nBOLT-INFO: The number of lfenced indirect calls is : " << LFencedIndirectCalls;
  outs() << "\nBOLT-INFO: The number of lfenced indirect jmps is : " << LFencedIndirectJmps
         << "\n\n";
}

} // namespace bolt
} // namespace llvm
