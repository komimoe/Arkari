#ifndef OBFUSCATION_CONSTANT_FP_ENCRYPTION_H
#define OBFUSCATION_CONSTANT_FP_ENCRYPTION_H

namespace llvm {
class FunctionPass;
class PassRegistry;
class ObfuscationOptions;

FunctionPass *createConstantFPEncryptionPass(ObfuscationOptions *argsOptions);
void          initializeConstantFPEncryptionPass(PassRegistry &Registry);

}

#endif