#ifndef OBFUSCATION_STRING_ENCRYPTION_H
#define OBFUSCATION_STRING_ENCRYPTION_H

namespace llvm {
class ModulePass;
class PassRegistry;
class ObfuscationOptions;

ModulePass *createStringEncryptionPass(ObfuscationOptions *argsOptions);
void        initializeStringEncryptionPass(PassRegistry &Registry);

}

#endif