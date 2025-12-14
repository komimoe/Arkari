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
#include "llvm/CryptoUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Transforms/Obfuscation/ObfuscationOptions.h"
#include "llvm/IR/IRBuilder.h"

#include <memory>
#include <random>

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
  CryptoUtils         RandomEngine;

  Flattening(unsigned            pointerSize,
             ObfuscationOptions *argsOptions) : FunctionPass(ID) {
    this->pointerSize = pointerSize;
    this->ArgsOptions = argsOptions;
  }

  bool runOnFunction(Function &F) override;
  bool flatten(Function *f, const ObfOpt &opt);
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
  if (flatten(tmp, opt)) {
    ++Flattened;
    result = true;
  }

  return result;
}

bool Flattening::flatten(Function *f, const ObfOpt &opt) {
  vector<BasicBlock *> origBB;

  auto &Ctx = f->getContext();
  auto  intType = Type::getInt32Ty(Ctx);

  if (pointerSize == 8) {
    intType = Type::getInt64Ty(Ctx);
  }

  // SCRAMBLER
  char scrambling_key[16];
  cryptoutils->get_bytes(scrambling_key, 16);
  // END OF SCRAMBLER

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

  auto splitPos = --(insertBlock->end());

  if (insertBlock->size() > 1) {
    --splitPos;
  }

  std::mt19937_64 re(RandomEngine.get_uint64_t());
  std::shuffle(origBB.begin(), origBB.end(), re);

  auto bbEndOfEntry = insertBlock->splitBasicBlock(splitPos, "first");
  origBB.insert(origBB.begin(), bbEndOfEntry);

  // Remove jump
  insertBlock->getTerminator()->eraseFromParent();

  // Create switch variable and set as it
  IRBuilder<> IRB{insertBlock};
  const auto  switchVar = IRB.CreateAlloca(intType, nullptr, "switchVar");
  const auto  switchXorVar = IRB.CreateAlloca(intType, nullptr, "switchXor");


  ConstantInt *entryRandomXor = cast<ConstantInt>(
      ConstantInt::get(intType, RandomEngine.get_uint64_t()));
  if (pointerSize == 8) {
    auto xorKey = ConstantExpr::getXor(
        entryRandomXor,
        ConstantInt::get(intType, cryptoutils->scramble64(0, scrambling_key)));

    IRB.CreateStore(xorKey, switchVar, true);
  } else {
    auto xorKey = ConstantExpr::getXor(
        entryRandomXor,
        ConstantInt::get(intType, cryptoutils->scramble32(0, scrambling_key)));

    IRB.CreateStore(xorKey, switchVar, true);
  }
  IRB.CreateStore(entryRandomXor, switchXorVar, true);

  // Create main loop

  auto bbLoopEntry = BasicBlock::Create(f->getContext(), "loopEntry", f,
                                        insertBlock);
  auto bbLoopEnd = BasicBlock::Create(f->getContext(), "loopEnd", f,
                                      insertBlock);
  IRB.SetInsertPoint(bbLoopEntry);
  auto switchVarLoad = IRB.CreateLoad(intType, switchVar, "switchVar");
  auto switchXorLoad = IRB.CreateLoad(intType, switchXorVar, "switchXor");
  auto switchCondition = IRB.CreateXor(switchVarLoad, switchXorLoad);
  // Move first BB on top
  insertBlock->moveBefore(bbLoopEntry);
  BranchInst::Create(bbLoopEntry, insertBlock);

  // loopEnd jump to loopEntry
  BranchInst::Create(bbLoopEntry, bbLoopEnd);

  auto swDefault = BasicBlock::Create(f->getContext(), "switchDefault", f,
                                      bbLoopEnd);
  BranchInst::Create(bbLoopEnd, swDefault);

  // Create switch instruction itself and set condition
  auto switchI = SwitchInst::Create(&*f->begin(), swDefault, 0, bbLoopEntry);
  switchI->setCondition(switchCondition);

  // Remove branch jump from 1st BB and make a jump to the while
  f->begin()->getTerminator()->eraseFromParent();

  BranchInst::Create(bbLoopEntry, &*f->begin());

  // Put all BB in the switch
  for (auto bi = origBB.begin(); bi != origBB.end(); ++bi) {
    const auto   bb = *bi;
    ConstantInt *numToCase;
    // Add case to switch
    if (pointerSize == 8) {
      numToCase = cast<ConstantInt>(ConstantInt::get(
          intType,
          cryptoutils->scramble64(switchI->getNumCases(), scrambling_key)));
    } else {
      numToCase = cast<ConstantInt>(ConstantInt::get(
          intType,
          cryptoutils->scramble32(switchI->getNumCases(), scrambling_key)));
    }

    // Move the BB inside the switch (only visual, no code logic)
    bb->moveBefore(bbLoopEnd);

    switchI->addCase(numToCase, bb);
  }

  // Recalculate switchVar
  for (auto bi = origBB.begin(); bi != origBB.end(); ++bi) {
    const auto bb = *bi;

    // Ret BB
    if (bb->getTerminator()->getNumSuccessors() == 0) {
      continue;
    }

    IRB.SetInsertPoint(bb->getTerminator());
    // If it's a non-conditional jump
    if (bb->getTerminator()->getNumSuccessors() == 1) {
      // Get successor and delete terminator
      auto tbb = bb->getTerminator()->getSuccessor(0);

      // Get next case
      auto numToCase = switchI->findCaseDest(tbb);

      // If target not in switch (e.g., back-edge to entry block), use case 0
      // Case 0 is bbEndOfEntry, the entry block continuation
      if (numToCase == nullptr) {
        if (pointerSize == 8) {
          numToCase = cast<ConstantInt>(
              ConstantInt::get(
                  intType,
                  cryptoutils->scramble64(0, scrambling_key)));
        } else {
          numToCase = cast<ConstantInt>(
              ConstantInt::get(
                  intType,
                  llvm::cryptoutils->scramble32(0, scrambling_key)));
        }
      }

      ConstantInt *randomXor = cast<ConstantInt>(
          ConstantInt::get(intType, RandomEngine.get_uint64_t()));

      auto xorKey = ConstantExpr::getXor(randomXor, numToCase);

      // Update switchVar and jump to the end of loop
      IRB.CreateStore(xorKey, switchVar, true);
      IRB.CreateStore(randomXor, switchXorVar, true);
      IRB.CreateBr(bbLoopEnd);
      bb->getTerminator()->eraseFromParent();
      continue;
    }

    // If it's a conditional jump
    if (bb->getTerminator()->getNumSuccessors() == 2) {
      // Get next cases
      auto numToCaseTrue =
          switchI->findCaseDest(bb->getTerminator()->getSuccessor(0));
      auto numToCaseFalse =
          switchI->findCaseDest(bb->getTerminator()->getSuccessor(1));

      // If target not in switch (e.g., back-edge to entry block), use case 0
      if (numToCaseTrue == nullptr) {
        if (pointerSize == 8) {
          numToCaseTrue = cast<ConstantInt>(
              ConstantInt::get(
                  intType,
                  llvm::cryptoutils->scramble64(0, scrambling_key)));
        } else {
          numToCaseTrue = cast<ConstantInt>(
              ConstantInt::get(
                  intType,
                  llvm::cryptoutils->scramble32(0, scrambling_key)));
        }
      }

      if (numToCaseFalse == nullptr) {
        if (pointerSize == 8) {
          numToCaseFalse = cast<ConstantInt>(
              ConstantInt::get(
                  intType,
                  llvm::cryptoutils->scramble64(0, scrambling_key)));
        } else {
          numToCaseFalse = cast<ConstantInt>(
              ConstantInt::get(
                  intType,
                  llvm::cryptoutils->scramble32(0, scrambling_key)));
        }
      }

      ConstantInt *randomXor = cast<ConstantInt>(
          ConstantInt::get(intType, RandomEngine.get_uint64_t()));

      auto xorKeyT = ConstantExpr::getXor(numToCaseTrue, randomXor);
      auto xorKeyF = ConstantExpr::getXor(numToCaseFalse, randomXor);
      IRB.CreateStore(randomXor, switchXorVar, true);

      // Create a SelectInst
      auto br = cast<BranchInst>(bb->getTerminator());
      auto sel = IRB.CreateSelect(br->getCondition(), xorKeyT, xorKeyF);

      // Update switchVar and jump to the end of loop
      IRB.CreateStore(sel, switchVar, true);
      IRB.CreateBr(bbLoopEnd);

      // Erase terminator
      bb->getTerminator()->eraseFromParent();
      continue;
    }
  }

  fixStack(f);

  lower->runOnFunction(*f);

  return true;
}

char                            Flattening::ID = 0;
static RegisterPass<Flattening> X("flattening", "Call graph flattening");

FunctionPass *llvm::createFlatteningPass(unsigned            pointerSize,
                                         ObfuscationOptions *argsOptions) {
  return new Flattening(pointerSize, argsOptions);
}