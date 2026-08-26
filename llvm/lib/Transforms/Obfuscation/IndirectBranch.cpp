#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Obfuscation/IndirectBranch.h"
#include "llvm/Transforms/Obfuscation/ObfuscationOptions.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/IR/Module.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Support/RandomNumberGenerator.h"
#include "llvm/TargetParser/Triple.h"

#include <random>

#define DEBUG_TYPE "indbr"

using namespace llvm;

namespace {
struct IndirectBranch : public FunctionPass {
  static char         ID;
  ObfuscationOptions *ArgsOptions;

  DenseMap<Function *, SmallPtrSet<Constant *, 8>>   FunctionBBs;
  DenseMap<Function *, SmallPtrSet<BranchInst *, 8>> FunctionBrs;

  std::vector<Constant *>          BBAddrTargets;
  DenseMap<Constant *, unsigned>   BBIndex;
  DenseMap<Constant *, uint64_t>   BBKeys;
  SmallVector<GlobalVariable *, 8> BBPageTable;
  std::mt19937_64                  RNG;
  uint64_t                         PtrEncKey = 0;

  bool RunOnFuncChanged = false;

  IndirectBranch(ObfuscationOptions *argsOptions) : FunctionPass(ID) {
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
    return {"IndirectBranch"};
  }

  void NumberBasicBlock(Module &M) {
    for (auto &F : M) {
      if (F.empty() || F.isWeakForLinker() ||
          F.getSection() == ".text.startup" ||
          F.isIntrinsic()) {
        continue;
      }
      const auto opt = ArgsOptions->toObfuscate(ArgsOptions->indBrOpt(), &F);
      if (!opt.isEnabled()) {
        continue;
      }
      SplitAllCriticalEdges(F, CriticalEdgeSplittingOptions(nullptr, nullptr));

      const uint64_t BBKey = RNG();

      for (auto &BB : F) {
        if (auto *BI = dyn_cast<BranchInst>(BB.getTerminator())) {
          if (BI->isConditional()) {
            FunctionBrs[&F].insert(BI);
            unsigned N = BI->getNumSuccessors();
            for (unsigned I = 0; I < N; I++) {
              BasicBlock *Successor = BI->getSuccessor(I);
              auto        BBAddr = BlockAddress::get(Successor);
              FunctionBBs[&F].insert(BBAddr);
              if (BBKeys.count(BBAddr) == 0) {
                BBAddrTargets.push_back(BBAddr);
                BBKeys[BBAddr] = BBKey;
              }
            }
          }
        }
      }
    }
  }

  bool doInitialization(Module &M) override {
    BBIndex.clear();
    BBPageTable.clear();
    FunctionBBs.clear();
    FunctionBrs.clear();
    BBAddrTargets.clear();
    BBKeys.clear();

    NumberBasicBlock(M);
    if (BBAddrTargets.empty()) {
      return false;
    }

    PtrEncKey = RNG();

    CreatePageTableArgs createPageTableArgs;
    createPageTableArgs.CountLoop = 1;
    createPageTableArgs.GVNamePrefix = M.getName().str() + "_IndirectBr";
    createPageTableArgs.RNG = &RNG;
    createPageTableArgs.M = &M;
    createPageTableArgs.Objects = &BBAddrTargets;
    createPageTableArgs.IndexMap = &BBIndex;
    createPageTableArgs.ObjectKeys = &BBKeys;
    createPageTableArgs.OutPageTable = &BBPageTable;
    createPageTableArgs.PtrEncKey = PtrEncKey;

    createPageTable(createPageTableArgs);
    return false;
  }


  bool runOnFunction(Function &Fn) override {
    const auto opt = ArgsOptions->toObfuscate(ArgsOptions->indBrOpt(), &Fn);
    if (!opt.isEnabled()) {
      return false;
    }

    LLVMContext &Ctx = Fn.getContext();
    auto &       M = *Fn.getParent();

    if (BBAddrTargets.empty()) {
      return false;
    }

    auto &FuncBBsSet = FunctionBBs[&Fn];
    auto &FuncBrs = FunctionBrs[&Fn];
    if (FuncBBsSet.empty() || FuncBrs.empty()) {
      return false;
    }

    std::vector<Constant *>        FuncBBs;
    DenseMap<Constant *, uint64_t> FuncKeys;
    auto                           FuncKey = RNG();

    for (auto bb : FuncBBsSet) {
      FuncBBs.push_back(bb);
      FuncKeys[bb] = FuncKey;
    }

    SmallVector<GlobalVariable *, 8> FuncBBPageTable;
    DenseMap<Constant *, unsigned>   FuncBBIndex;

    if (opt.level()) {
      CreatePageTableArgs createPageTableArgs;
      createPageTableArgs.CountLoop = opt.level();
      createPageTableArgs.GVNamePrefix =
          M.getName().str() + Fn.getName().str() + "_IndirectBr";
      createPageTableArgs.M = &M;
      createPageTableArgs.RNG = &RNG;
      createPageTableArgs.Objects = &FuncBBs;
      createPageTableArgs.IndexMap = &BBIndex;
      createPageTableArgs.ObjectKeys = &FuncKeys;
      createPageTableArgs.OutPageTable = &FuncBBPageTable;

      enhancedPageTable(createPageTableArgs, &FuncBBIndex);
    }

    auto *IntTy = getPageTableIntTy(M);
    for (auto BI : FuncBrs) {
      if (BI && BI->isConditional()) {
        IRBuilder<> IRB(BI);

        auto Cond = BI->getCondition();

        auto TBB = BI->getSuccessor(0);
        auto FBB = BI->getSuccessor(1);
        auto AddrTBB = BlockAddress::get(TBB);
        auto AddrFBB = BlockAddress::get(FBB);

        auto TIndex = opt.level()
                        ? ConstantInt::get(IntTy, FuncBBIndex[AddrTBB])
                        : ConstantInt::get(IntTy, BBIndex[AddrTBB]);

        auto FIndex = opt.level()
                        ? ConstantInt::get(IntTy, FuncBBIndex[AddrFBB])
                        : ConstantInt::get(IntTy, BBIndex[AddrFBB]);

        auto NextIndex = IRB.CreateSelect(Cond, TIndex, FIndex);

        BuildDecryptArgs buildDecrypt;
        buildDecrypt.FuncLoopCount = opt.level();
        buildDecrypt.NextIndex = 0;
        buildDecrypt.NextIndexValue = NextIndex;
        buildDecrypt.Fn = &Fn;
        buildDecrypt.InsertBefore = BI;
        buildDecrypt.LoadTy = PointerType::getUnqual(Ctx);
        buildDecrypt.ModulePageTable = &BBPageTable;
        buildDecrypt.FuncPageTable = &FuncBBPageTable;
        buildDecrypt.ModuleKey = BBKeys[AddrTBB];
        buildDecrypt.FuncKey = FuncKeys[AddrTBB];
        buildDecrypt.PtrEncKey = PtrEncKey;
        Triple T(M.getTargetTriple());
        buildDecrypt.PtrAuthKey = T.isAArch64() ? 0 : -1;
        buildDecrypt.PtrAuthDisc = 0;

        auto            TargetPtr = buildPageTableDecryptIR(buildDecrypt);
        IndirectBrInst *IBI = IndirectBrInst::Create(TargetPtr, 2);
        ReplaceInstWithInst(BI, IBI);
        IBI->addDestination(TBB);
        IBI->addDestination(FBB);

        RunOnFuncChanged = true;
      }
    }

    return true;
  }

  bool doFinalization(Module &M) override {
    if (!RunOnFuncChanged || BBPageTable.empty()) {
      return false;
    }
    for (auto bbPage : BBPageTable) {
      appendToCompilerUsed(M, {bbPage});
    }
    return true;
  }

};
} // anonymous namespace

char IndirectBranch::ID = 0;

FunctionPass *llvm::createIndirectBranchPass(ObfuscationOptions *argsOptions) {
  return new IndirectBranch(argsOptions);
}

INITIALIZE_PASS(IndirectBranch, "indbr",
                "Enable IR Indirect Branch Obfuscation", false, false)