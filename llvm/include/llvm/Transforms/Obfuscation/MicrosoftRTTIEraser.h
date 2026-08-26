#ifndef _MICROSOFT_RTTI_ERASER_INCLUDES_
#define _MICROSOFT_RTTI_ERASER_INCLUDES_

namespace llvm {
class ModulePass;
class PassRegistry;
class ObfuscationOptions;

ModulePass *createMsRttiEraserPass(ObfuscationOptions *argsOptions);
void        initializeMsRttiEraserPass(PassRegistry &Registry);

}

#endif