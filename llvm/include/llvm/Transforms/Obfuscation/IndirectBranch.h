#ifndef OBFUSCATION_INDIRECTBR_H
#define OBFUSCATION_INDIRECTBR_H

// Namespace
namespace llvm {
class FunctionPass;
class PassRegistry;
class ObfuscationOptions;

FunctionPass *createIndirectBranchPass(ObfuscationOptions *argsOptions);
void          initializeIndirectBranchPass(PassRegistry &Registry);

}

#endif