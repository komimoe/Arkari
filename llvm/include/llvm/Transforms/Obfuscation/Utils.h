#ifndef __UTILS_OBF__
#define __UTILS_OBF__

#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <random>
#include <vector>

using namespace llvm;

struct CreatePageTableArgs {
  unsigned                           CountLoop;
  std::string                        GVNamePrefix;
  std::mt19937_64 *                  RNG;
  Module *                           M;
  std::vector<Constant *> *          Objects;
  DenseMap<Constant *, unsigned> *   IndexMap;
  DenseMap<Constant *, uint64_t> *   ObjectKeys;
  SmallVectorImpl<GlobalVariable *> *OutPageTable;
  uint64_t                           PtrEncKey;
};


struct BuildDecryptArgs {
  unsigned FuncLoopCount;
  unsigned NextIndex;
  Value *NextIndexValue;
  Function *Fn;
  Instruction *InsertBefore;
  Type *LoadTy;
  SmallVectorImpl<GlobalVariable *> *ModulePageTable;
  SmallVectorImpl<GlobalVariable *> *FuncPageTable;
  uint64_t ModuleKey;
  uint64_t FuncKey;
  uint64_t PtrEncKey;
  int PtrAuthKey; // -1 = no PAC, 0 = IA (code), 2 = DA (data)
  uint64_t PtrAuthDisc; // ptrauth discriminator
};

IntegerType *getPageTableIntTy(Module &M);
bool valueEscapes(Instruction *Inst);
void fixStack(Function *f);
CallBase *fixEH(CallBase *CB);
void LowerConstantExpr(Function &F);
bool expandConstantExpr(Function &F);
void createPageTable(const CreatePageTableArgs &args);
void enhancedPageTable(const CreatePageTableArgs &args,
                       DenseMap<Constant *, unsigned> *FuncIndexMap);
Value *buildPageTableDecryptIR(const BuildDecryptArgs &args);
Value *encryptConstant(Constant *plainConstant, Instruction *insertBefore,
                       std::mt19937_64 &rng, unsigned level);
#endif