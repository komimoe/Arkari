#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Transforms/Obfuscation/IndirectGlobalVariable.h"
#include "llvm/Transforms/Obfuscation/ObfuscationOptions.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/APInt.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/TargetParser/Triple.h"

#include <random>

#define DEBUG_TYPE "indgv"

using namespace llvm;

namespace {
struct IndirectGlobalVariable : public FunctionPass {
  static char         ID;
  ObfuscationOptions *ArgsOptions;

  DenseMap<Function *, SmallPtrSet<GlobalVariable *, 8>> FunctionGVs;

  std::vector<Constant *>          GlobalVariables;
  DenseMap<Constant *, unsigned>   GVIndex;
  DenseMap<Constant *, uint64_t>   GVKeys;
  SmallVector<GlobalVariable *, 8> GVPageTable;

  std::mt19937_64 RNG;
  uint64_t        PtrEncKey = 0;
  bool            RunOnFuncChanged = false;

  IndirectGlobalVariable(ObfuscationOptions *argsOptions) : FunctionPass(ID) {
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
    return {"IndirectGlobalVariable"};
  }

  void NumberGlobalVariable(Module &M) {
    for (auto &F : M) {
      if (F.isIntrinsic()) {
        continue;
      }
      LowerConstantExpr(F);
      for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        Instruction *Inst = &*I;

        if (Inst->isEHPad() || isa<CallInst>(Inst)) {
          continue;
        }

        for (auto op = Inst->op_begin(); op != Inst->op_end(); ++op) {
          Value *val = *op;
          if (auto GV = dyn_cast<GlobalVariable>(val)) {
            if (GV->isThreadLocal() || GV->isDLLImportDependent()) {
              continue;
            }
            if (GV->getMetadata("noobf")) {
              continue;
            }

            FunctionGVs[&F].insert(GV);
            if (GVKeys.count(GV) == 0) {
              GlobalVariables.push_back(GV);
              GVKeys[GV] = RNG();
            }
          }
        }
      }
    }
  }

  bool doInitialization(Module &M) override {
    GVIndex.clear();
    GVPageTable.clear();
    FunctionGVs.clear();
    GlobalVariables.clear();
    GVKeys.clear();

    NumberGlobalVariable(M);
    if (GlobalVariables.empty()) {
      return false;
    }

    PtrEncKey = RNG();

    CreatePageTableArgs createPageTableArgs;
    createPageTableArgs.CountLoop = 1;
    createPageTableArgs.GVNamePrefix = M.getName().str() + "_IndirectGVs";
    createPageTableArgs.RNG = &RNG;
    createPageTableArgs.M = &M;
    createPageTableArgs.Objects = &GlobalVariables;
    createPageTableArgs.IndexMap = &GVIndex;
    createPageTableArgs.ObjectKeys = &GVKeys;
    createPageTableArgs.OutPageTable = &GVPageTable;
    createPageTableArgs.PtrEncKey = PtrEncKey;

    createPageTable(createPageTableArgs);
    return false;
  }


  bool runOnFunction(Function &Fn) override {
    const auto opt = ArgsOptions->toObfuscate(ArgsOptions->indGvOpt(), &Fn);
    if (!opt.isEnabled()) {
      return false;
    }

    auto &M = *Fn.getParent();

    if (GlobalVariables.empty()) {
      return false;
    }

    auto &FuncGVSet = FunctionGVs[&Fn];
    if (FuncGVSet.empty()) {
      return false;
    }

    std::vector<Constant *>        FuncGVs;
    DenseMap<Constant *, uint64_t> FuncKeys;
    for (auto GV : FuncGVSet) {
      FuncGVs.push_back(GV);
      FuncKeys[GV] = RNG();
    }

    SmallVector<GlobalVariable *, 8> FuncGVPageTable;
    DenseMap<Constant *, unsigned>   FuncGVIndex;

    if (opt.level()) {
      CreatePageTableArgs createPageTableArgs;
      createPageTableArgs.CountLoop = opt.level();
      createPageTableArgs.GVNamePrefix =
          M.getName().str() + Fn.getName().str() + "_IndirectGVs";
      createPageTableArgs.RNG = &RNG;
      createPageTableArgs.M = &M;
      createPageTableArgs.Objects = &FuncGVs;
      createPageTableArgs.IndexMap = &GVIndex;
      createPageTableArgs.ObjectKeys = &FuncKeys;
      createPageTableArgs.OutPageTable = &FuncGVPageTable;

      enhancedPageTable(createPageTableArgs, &FuncGVIndex);
    }

    // Count GV references for deduplication
    DenseMap<GlobalVariable *, unsigned> GVUseCount;
    for (inst_iterator I = inst_begin(Fn), E = inst_end(Fn); I != E; ++I) {
      Instruction *Inst = &*I;
      if (isa<CallInst>(Inst) || isa<CatchReturnInst>(Inst) ||
          isa<ResumeInst>(Inst) || Inst->isEHPad())
        continue;
      for (unsigned i = 0; i < Inst->getNumOperands(); ++i) {
        if (auto *GV = dyn_cast<GlobalVariable>(Inst->getOperand(i))) {
          if (GVIndex.count(GV))
            GVUseCount[GV]++;
        }
      }
    }

    // Pre-decrypt duplicate GVs at function entry
    DenseMap<GlobalVariable *, AllocaInst *> GVDedupCache;
    auto &EntryBB = Fn.getEntryBlock();
    Instruction *AllocaInsertPt = &*EntryBB.begin();
    auto *PtrTy = PointerType::getUnqual(Fn.getContext());
    for (auto &KV : GVUseCount) {
      if (KV.second <= 1)
        continue;
      IRBuilder<> AIB(AllocaInsertPt);
      GVDedupCache[KV.first] = AIB.CreateAlloca(PtrTy, nullptr);
    }

    if (!GVDedupCache.empty()) {
      Instruction *DecryptPt = nullptr;
      for (auto &I : EntryBB) {
        if (!isa<AllocaInst>(&I)) {
          DecryptPt = &I;
          break;
        }
      }
      if (!DecryptPt)
        DecryptPt = EntryBB.getTerminator();
      Triple T(M.getTargetTriple());
      for (auto &KV : GVDedupCache) {
        auto *           GV = KV.first;
        BuildDecryptArgs buildDecrypt;
        buildDecrypt.FuncLoopCount = opt.level();
        buildDecrypt.NextIndex = opt.level() ? FuncGVIndex[GV] : GVIndex[GV];
        buildDecrypt.NextIndexValue = nullptr;
        buildDecrypt.Fn = &Fn;
        buildDecrypt.InsertBefore = DecryptPt;
        buildDecrypt.LoadTy = GV->getType();
        buildDecrypt.ModulePageTable = &GVPageTable;
        buildDecrypt.FuncPageTable = &FuncGVPageTable;
        buildDecrypt.ModuleKey = GVKeys[GV];
        buildDecrypt.FuncKey = FuncKeys[GV];
        buildDecrypt.PtrEncKey = PtrEncKey;
        buildDecrypt.PtrAuthKey = T.isAArch64() ? 2 : -1;
        buildDecrypt.PtrAuthDisc = 0;
        auto        GVPtr = buildPageTableDecryptIR(buildDecrypt);
        IRBuilder<> SIB(DecryptPt);
        SIB.CreateAlignedStore(GVPtr, KV.second, Align{1}, true);
      }
    }

    for (inst_iterator I = inst_begin(Fn), E = inst_end(Fn); I != E; ++I) {
      Instruction *Inst = &*I;
      if (isa<CallInst>(Inst) || isa<CatchReturnInst>(Inst) || isa<
            ResumeInst>(Inst) || Inst->isEHPad()) {
        continue;
      }

      for (unsigned i = 0; i < Inst->getNumOperands(); ++i) {
        if (GlobalVariable *GV = dyn_cast<
          GlobalVariable>(Inst->getOperand(i))) {
          if (!GVIndex.count(GV)) {
            continue;
          }

          auto PHI = dyn_cast<PHINode>(Inst);
          auto InsertPoint = PHI
                               ? PHI->getIncomingBlock(i)->getTerminator()
                               : Inst;

          Value *GVPtr;
          auto   CacheIt = GVDedupCache.find(GV);
          if (CacheIt != GVDedupCache.end()) {
            IRBuilder<> IRB(InsertPoint);
            GVPtr = IRB.CreateAlignedLoad(
                GV->getType(), CacheIt->second, Align{1}, true);
          } else {
            BuildDecryptArgs buildDecrypt;
            buildDecrypt.FuncLoopCount = opt.level();
            buildDecrypt.NextIndex =
                opt.level() ? FuncGVIndex[GV] : GVIndex[GV];
            buildDecrypt.NextIndexValue = nullptr;
            buildDecrypt.Fn = &Fn;
            buildDecrypt.InsertBefore = InsertPoint;
            buildDecrypt.LoadTy = GV->getType();
            buildDecrypt.ModulePageTable = &GVPageTable;
            buildDecrypt.FuncPageTable = &FuncGVPageTable;
            buildDecrypt.ModuleKey = GVKeys[GV];
            buildDecrypt.FuncKey = FuncKeys[GV];
            buildDecrypt.PtrEncKey = PtrEncKey;
            Triple T(M.getTargetTriple());
            buildDecrypt.PtrAuthKey = T.isAArch64() ? 2 : -1;
            buildDecrypt.PtrAuthDisc = 0;
            GVPtr = buildPageTableDecryptIR(buildDecrypt);
          }

          if (PHI)
            PHI->setIncomingValue(i, GVPtr);
          else
            Inst->replaceUsesOfWith(GV, GVPtr);
          RunOnFuncChanged = true;
        }
      }
    }

    return true;
  }

  bool doFinalization(Module &M) override {
    if (!RunOnFuncChanged || GVPageTable.empty()) {
      return false;
    }
    for (auto gvPage : GVPageTable) {
      appendToCompilerUsed(M, {gvPage});
    }
    return true;
  }

};
} // anonymous namespace

char IndirectGlobalVariable::ID = 0;

FunctionPass *llvm::createIndirectGlobalVariablePass(
    ObfuscationOptions *argsOptions) {
  return new IndirectGlobalVariable(argsOptions);
}

INITIALIZE_PASS(IndirectGlobalVariable, "indgv",
                "Enable IR Indirect Global Variable Obfuscation", false, false)