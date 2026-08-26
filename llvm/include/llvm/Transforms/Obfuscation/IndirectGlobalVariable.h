#ifndef OBFUSCATION_INDIRECT_GLOBAL_VARIABLE_H
#define OBFUSCATION_INDIRECT_GLOBAL_VARIABLE_H

// Namespace
namespace llvm {
class FunctionPass;
class PassRegistry;
class ObfuscationOptions;

FunctionPass *createIndirectGlobalVariablePass(ObfuscationOptions *argsOptions);
void          initializeIndirectGlobalVariablePass(PassRegistry &Registry);

}

#endif