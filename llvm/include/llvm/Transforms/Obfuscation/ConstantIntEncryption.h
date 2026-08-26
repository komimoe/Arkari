#ifndef OBFUSCATION_CONSTANT_INT_ENCRYPTION_H
#define OBFUSCATION_CONSTANT_INT_ENCRYPTION_H

namespace llvm {
class FunctionPass;
class PassRegistry;
class ObfuscationOptions;

FunctionPass *createConstantIntEncryptionPass(ObfuscationOptions *argsOptions);
void          initializeConstantIntEncryptionPass(PassRegistry &Registry);

}

#endif