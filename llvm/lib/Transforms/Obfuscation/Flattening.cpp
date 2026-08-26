//===- Flattening.cpp - Flattening Obfuscation pass------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the flattening pass
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Constants.h"
#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/Transforms/Obfuscation/LegacyLowerSwitch.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Transforms/Obfuscation/ObfuscationOptions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/RandomNumberGenerator.h"

#include <memory>
#include <random>
#include "llvm/ADT/DenseSet.h"

#define DEBUG_TYPE "flattening"

using namespace std;
using namespace llvm;

// Stats
STATISTIC(Flattened, "Functions flattened");

namespace {
struct Flattening : public FunctionPass {
  unsigned    pointerSize;
  static char ID; // Pass identification, replacement for typeid

  ObfuscationOptions *ArgsOptions;
  std::mt19937_64     RNG;

  Flattening(unsigned            pointerSize,
             ObfuscationOptions *argsOptions) : FunctionPass(ID) {
    this->pointerSize = pointerSize;
    this->ArgsOptions = argsOptions;
    uint64_t seed = 0;
    if (auto errorCode = llvm::getRandomBytes(&seed, sizeof(seed))) {
      llvm::report_fatal_error(
          StringRef("Failed to get random bytes for page table generation") +
          errorCode.message());
    }

    RNG = std::mt19937_64(seed);
  }

  bool runOnFunction(Function &F) override;
  bool flatten(Function *f);
};
}

bool Flattening::runOnFunction(Function &F) {
  if (F.isIntrinsic()) {
    return false;
  }
  Function *tmp = &F;
  bool      result = false;
  // Do we obfuscate
  const auto opt = ArgsOptions->toObfuscate(ArgsOptions->flaOpt(), &F);
  if (!opt.isEnabled()) {
    return result;
  }
  if (flatten(tmp)) {
    ++Flattened;
    result = true;
  }

  return result;
}

bool Flattening::flatten(Function *f) {
  SmallVector<BasicBlock *, 32> origBB;

  auto &Ctx = f->getContext();
  Type *intType = Type::getInt32Ty(Ctx);
  if (pointerSize == 8) {
    intType = Type::getInt64Ty(Ctx);
  }
  auto *IntTy = cast<IntegerType>(intType);

  auto randWord = [&]() ->uint64_t {
    uint64_t v = RNG();
    if (pointerSize != 8)
      v &= 0xffffffffull;
    return v;
  };

  auto randConst = [&]() ->ConstantInt * {
    return ConstantInt::get(IntTy, randWord());
  };

  // Lower switch
  auto lower = std::unique_ptr<FunctionPass>(createLegacyLowerSwitchPass());
  lower->runOnFunction(*f);

  // Save all original BB
  for (auto i = f->begin(); i != f->end(); ++i) {
    auto bb = &*i;
    origBB.push_back(bb);

    if (isa<InvokeInst>(bb->getTerminator()) || bb->isEHPad()) {
      return false;
    }
  }

  // Nothing to flatten
  if (origBB.size() <= 1) {
    return false;
  }

  // Remove first BB
  origBB.erase(origBB.begin());

  // Get a pointer on the first BB
  auto insertBlock = &*(f->begin());
  auto splitPos = insertBlock->getFirstNonPHIOrDbgOrAlloca();

  std::shuffle(origBB.begin(), origBB.end(), RNG);

  auto bbEndOfEntry = insertBlock->splitBasicBlock(splitPos, "first");
  origBB.insert(origBB.begin(), bbEndOfEntry);

  DenseSet<uint64_t>                    UsedCases;
  DenseMap<BasicBlock *, ConstantInt *> CaseVal;

  for (BasicBlock *BB : origBB) {
    uint64_t v;
    do {
      v = randWord();
    } while (v == 0 || UsedCases.count(v));
    UsedCases.insert(v);
    CaseVal[BB] = ConstantInt::get(IntTy, v);
  }

  ConstantInt *EntryCase = CaseVal[bbEndOfEntry];

  // Create switch variable and set as it
  IRBuilder<> IRB{insertBlock};
  const auto  switchVar = IRB.CreateAlloca(IntTy, nullptr, "switchVar");
  const auto  switchXorVar = IRB.CreateAlloca(IntTy, nullptr, "switchXor");

  // init：Encoded = EntryCase ^ XorKey
  ConstantInt *entryXor = randConst();
  Value *      entryEnc = IRB.CreateXor(EntryCase, entryXor);
  IRB.CreateStore(entryEnc, switchVar, true);
  IRB.CreateStore(entryXor, switchXorVar, true);

  // Create main loop
  auto bbLoopEntry =
      BasicBlock::Create(f->getContext(), "loopEntry", f, insertBlock);
  auto bbLoopEnd =
      BasicBlock::Create(f->getContext(), "loopEnd", f, insertBlock);

  // loopEntry
  IRB.SetInsertPoint(bbLoopEntry);
  Value *enc0 = IRB.CreateLoad(IntTy, switchVar, "switchVar.enc0");
  Value *xor0 = IRB.CreateLoad(IntTy, switchXorVar, "switchXor.xor0");

  // rolling delta
  ConstantInt *delta = randConst();
  Value *      enc1 = IRB.CreateXor(enc0, delta, "switchVar.enc1");
  Value *      xor1 = IRB.CreateXor(xor0, delta, "switchXor.xor1");
  IRB.CreateStore(enc1, switchVar, true);
  IRB.CreateStore(xor1, switchXorVar, true);

  Value *switchCondition = IRB.CreateXor(enc1, xor1, "switchCond");

  // Move first BB on top
  insertBlock->moveBefore(bbLoopEntry);
  BranchInst::Create(bbLoopEntry, insertBlock);

  // loopEnd jump to loopEntry
  BranchInst::Create(bbLoopEntry, bbLoopEnd);

  auto swDefault =
      BasicBlock::Create(f->getContext(), "switchDefault", f, bbLoopEnd);
  BranchInst::Create(bbLoopEnd, swDefault);

  // Create switch instruction itself and set condition
  auto switchI = SwitchInst::Create(switchCondition, swDefault, 0, bbLoopEntry);

  // Remove branch jump from 1st BB and make a jump to the while
  ReplaceInstWithInst(f->begin()->getTerminator(),
                      BranchInst::Create(bbLoopEntry));

  // Put all BB in the switch (case 值使用纯随机表)
  for (auto bi = origBB.begin(); bi != origBB.end(); ++bi) {
    auto bb = *bi;

    // Move the BB inside the switch (only visual, no code logic)
    bb->moveBefore(bbLoopEnd);

    switchI->addCase(CaseVal[bb], bb);
  }

  // Recalculate switchVar
  for (auto bi = origBB.begin(); bi != origBB.end(); ++bi) {
    const auto bb = *bi;

    // Ret BB
    if (bb->getTerminator()->getNumSuccessors() == 0) {
      continue;
    }

    IRB.SetInsertPoint(bb->getTerminator());

    auto writeNextEncoded = [&](Value *NextCaseVal) {
      // NextEnc = NextCase ^ NewXor
      ConstantInt *newXor = randConst();
      Value *      nextEnc = IRB.CreateXor(NextCaseVal, newXor);

      IRB.CreateStore(nextEnc, switchVar, true);
      IRB.CreateStore(newXor, switchXorVar, true);

      IRB.CreateBr(bbLoopEnd);
      bb->getTerminator()->eraseFromParent();
    };

    // If it's a non-conditional jump
    if (bb->getTerminator()->getNumSuccessors() == 1) {
      auto tbb = bb->getTerminator()->getSuccessor(0);

      Value *nextCase = nullptr;
      if (CaseVal.count(tbb)) {
        nextCase = CaseVal[tbb];
      } else {
        nextCase = EntryCase;
      }

      writeNextEncoded(nextCase);
      continue;
    }

    // If it's a conditional jump
    if (bb->getTerminator()->getNumSuccessors() == 2 &&
        isa<BranchInst>(bb->getTerminator())) {
      auto *br = cast<BranchInst>(bb->getTerminator());

      auto *succT = br->getSuccessor(0);
      auto *succF = br->getSuccessor(1);

      Value *caseT =
          CaseVal.count(succT) ? CaseVal[succT] : EntryCase;
      Value *caseF =
          CaseVal.count(succF) ? CaseVal[succF] : EntryCase;

      Value *selectedCase =
          IRB.CreateSelect(br->getCondition(), caseT, caseF, "nextCase");

      writeNextEncoded(selectedCase);
      continue;
    }
  }

  fixStack(f);

  return true;
}


char                            Flattening::ID = 0;
static RegisterPass<Flattening> X("flattening", "Call graph flattening");

FunctionPass *llvm::createFlatteningPass(unsigned            pointerSize,
                                         ObfuscationOptions *argsOptions) {
  return new Flattening(pointerSize, argsOptions);
}