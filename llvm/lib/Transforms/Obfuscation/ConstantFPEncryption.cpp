#include "llvm/Transforms/Obfuscation/ObfuscationOptions.h"
#include "llvm/Transforms/Obfuscation/ConstantFPEncryption.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"
#include "llvm/Transforms/IPO/Attributor.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <algorithm>

#define DEBUG_TYPE "constant-fp-encryption"

using namespace llvm;

namespace {

struct ConstantFPEncryption : public FunctionPass {
  static char         ID;
  ObfuscationOptions *ArgsOptions;

  DenseMap<Function *, SmallPtrSet<Instruction *, 16>> FunctionModifyIRs;
  std::mt19937_64                                      RNG;

  ConstantFPEncryption(ObfuscationOptions *argsOptions) : FunctionPass(ID) {
    this->ArgsOptions = argsOptions;
    uint64_t seed = 0;
    if (auto errorCode = llvm::getRandomBytes(&seed, sizeof(seed))) {
      llvm::report_fatal_error(
          StringRef("Failed to get random bytes for page table generation") +
          errorCode.message());
    }

    RNG = std::mt19937_64(seed);
  }

  StringRef getPassName() const override {
    return {"ConstantFPEncryption"};
  }

  bool doInitialization(Module &M) override {
    bool Changed = false;
    for (auto &F : M) {
      const auto opt = ArgsOptions->toObfuscate(ArgsOptions->cfeOpt(), &F);
      if (!opt.isEnabled()) {
        continue;
      }
      Changed |= expandConstantExpr(F);
      for (auto &BB : F) {
        for (auto &I : BB) {
          if (I.isEHPad() || isa<AllocaInst>(&I) ||
              isa<IntrinsicInst>(&I) || isa<SwitchInst>(I) ||
              I.isAtomic()) {
            continue;
          }
          auto CI = dyn_cast<CallInst>(&I);
          auto GEP = dyn_cast<GetElementPtrInst>(&I);
          auto PHI = dyn_cast<PHINode>(&I);

          for (unsigned i = 0; i < (PHI
                                      ? PHI->getNumIncomingValues()
                                      : I.getNumOperands()); ++i) {
            if (CI && CI->isBundleOperand(i)) {
              continue;
            }
            if (GEP && (i < 2 || GEP->getSourceElementType()->isStructTy())) {
              continue;
            }
            if (PHI && isa<SwitchInst>(
                    PHI->getIncomingBlock(i)->getTerminator())) {
              continue;
            }
            Value *Opr = PHI ? PHI->getIncomingValue(i) : I.getOperand(i);
            if (isa<ConstantFP>(Opr)) {
              FunctionModifyIRs[&F].insert(&I);
              break;
            }
          }
        }
      }
    }
    return Changed;
  }

  bool runOnFunction(Function &F) override {
    const auto opt = ArgsOptions->toObfuscate(ArgsOptions->cfeOpt(), &F);
    if (!opt.isEnabled()) {
      return false;
    }
    auto &FuncModifyIRs = FunctionModifyIRs[&F];
    if (FuncModifyIRs.empty()) {
      return false;
    }

    // Count constant occurrences for deduplication
    DenseMap<ConstantFP *, unsigned> ConstUseCount;
    for (auto I : FuncModifyIRs) {
      auto CI = dyn_cast<CallInst>(I);
      auto GEP = dyn_cast<GetElementPtrInst>(I);
      auto PHI = dyn_cast<PHINode>(I);
      for (unsigned i = 0; i < I->getNumOperands(); ++i) {
        if (CI && CI->isBundleOperand(i))
          continue;
        if (GEP && i < 2)
          continue;
        if (auto CFP = dyn_cast<ConstantFP>(I->getOperand(i))) {
          if (PHI &&
              isa<SwitchInst>(PHI->getIncomingBlock(i)->getTerminator()))
            continue;
          ConstUseCount[CFP]++;
        }
      }
    }

    // Pre-encrypt duplicate constants at function entry to avoid
    // creating redundant GlobalVariables and decrypt sequences
    DenseMap<ConstantFP *, AllocaInst *> DedupCache;
    auto &EntryBB = F.getEntryBlock();
    Instruction *AllocaInsertPt = &*EntryBB.begin();
    for (auto &KV : ConstUseCount) {
      if (KV.second <= 1)
        continue;
      auto *CFP = KV.first;
      auto *Ty = CFP->getType();
      auto BitWidth = Ty->getPrimitiveSizeInBits().getFixedValue();
      if (BitWidth < 8)
        continue;
      IRBuilder<NoFolder> AIB(AllocaInsertPt);
      DedupCache[CFP] = AIB.CreateAlloca(Ty, nullptr);
    }

    if (!DedupCache.empty()) {
      Instruction *DecryptPt = nullptr;
      for (auto &I : EntryBB) {
        if (!isa<AllocaInst>(&I)) {
          DecryptPt = &I;
          break;
        }
      }
      if (!DecryptPt)
        DecryptPt = EntryBB.getTerminator();
      for (auto &KV : DedupCache) {
        Value *Dec = encryptConstant(KV.first, DecryptPt, RNG, opt.level());
        IRBuilder<NoFolder> SIB(DecryptPt);
        SIB.CreateAlignedStore(Dec, KV.second, Align{1}, true);
      }
    }

    for (auto I : FuncModifyIRs) {
      auto CI = dyn_cast<CallInst>(I);
      auto GEP = dyn_cast<GetElementPtrInst>(I);
      auto PHI = dyn_cast<PHINode>(I);

      for (unsigned i = 0; i < I->getNumOperands(); ++i) {
        if (CI && CI->isBundleOperand(i)) {
          continue;
        }
        if (GEP && i < 2) {
          continue;
        }
        Value *Opr = I->getOperand(i);
        if (auto CFP = dyn_cast<ConstantFP>(Opr)) {
          if (PHI && isa<
                SwitchInst>(PHI->getIncomingBlock(i)->getTerminator())) {
            continue;
          }

          auto InsertPoint =
              PHI ? PHI->getIncomingBlock(i)->getTerminator() : I;
          Value *CipherConstant;
          auto CacheIt = DedupCache.find(CFP);
          if (CacheIt != DedupCache.end()) {
            IRBuilder<NoFolder> IRB(InsertPoint);
            CipherConstant = IRB.CreateAlignedLoad(
                CFP->getType(), CacheIt->second, Align{1}, true);
          } else {
            CipherConstant = encryptConstant(CFP, InsertPoint, RNG,
                                             opt.level());
          }
          if (PHI)
            PHI->setIncomingValue(i, CipherConstant);
          else
            I->setOperand(i, CipherConstant);
        }
      }
    }
    return true;
  }
};
} // anonymous namespace

char ConstantFPEncryption::ID = 0;

FunctionPass *llvm::createConstantFPEncryptionPass(
    ObfuscationOptions *argsOptions) {
  return new ConstantFPEncryption(argsOptions);
}

INITIALIZE_PASS(ConstantFPEncryption, "cfe",
                "Enable IR Constant FP Encryption", false, false)