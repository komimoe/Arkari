#ifndef OBFUSCATION_OBFUSCATIONPASSMANAGER_H
#define OBFUSCATION_OBFUSCATIONPASSMANAGER_H

#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/Transforms/Obfuscation/IndirectBranch.h"
#include "llvm/Transforms/Obfuscation/IndirectCall.h"
#include "llvm/Transforms/Obfuscation/IndirectGlobalVariable.h"
#include "llvm/Transforms/Obfuscation/StringEncryption.h"
#include "llvm/Transforms/Obfuscation/ConstantIntEncryption.h"
#include "llvm/Transforms/Obfuscation/ConstantFPEncryption.h"
#include "llvm/Transforms/Obfuscation/MicrosoftRTTIEraser.h"
#include "llvm/Passes/PassBuilder.h"

// Namespace
namespace llvm {
class ModulePass;
class PassRegistry;

ModulePass *createObfuscationPassManager();
void        initializeObfuscationPassManagerPass(PassRegistry &Registry);

class ObfuscationPassManagerPass
    : public PassInfoMixin<ObfuscationPassManagerPass> {
public:
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
    outs() << M.getName() << "\n";
    ModulePass *OPM = createObfuscationPassManager();
    bool        Changed = OPM->runOnModule(M);
    OPM->doFinalization(M);
    delete OPM;
    if (Changed) {
      return PreservedAnalyses::none();
    }
    return PreservedAnalyses::all();
  }

  static llvm::StringRef name() {
    return "ObfuscationPassManagerPass";
  }
};

} // namespace llvm

#endif