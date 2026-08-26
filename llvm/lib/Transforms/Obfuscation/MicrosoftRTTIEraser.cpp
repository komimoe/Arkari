#include "llvm/Transforms/Obfuscation/ObfuscationOptions.h"
#include "llvm/Transforms/Obfuscation/MicrosoftRTTIEraser.h"
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/Transforms/Utils/GlobalStatus.h"
#include "llvm/Transforms/IPO/Attributor.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/BLAKE3.h"

#define DEBUG_TYPE "ms_rtti_eraser"


using namespace llvm;

namespace {
class MsRttiEraser : public ModulePass {
protected:
  ObfuscationOptions *ArgsOptions;
  llvm::BLAKE3        Blake3;

public:
  static char ID;

  MsRttiEraser(ObfuscationOptions *argsOptions) : ModulePass(ID) {
    this->ArgsOptions = argsOptions;
    initializeMsRttiEraserPass(*PassRegistry::getPassRegistry());
  }

  StringRef getPassName() const override {
    return "MsRttiEraser";
  }

  bool runOnModule(Module &M) override {
    auto &seed = ArgsOptions->randomSeed();
    if (seed.empty()) {
      report_fatal_error(
          "No random seed found in config file, but rtti eraser enabled.");
    }
    bool         changed = false;
    LLVMContext &ctx = M.getContext();
    for (GlobalVariable &gv : M.globals()) {
      if (gv.isConstant() || !gv.hasInitializer()) {
        continue;
      }
      if (!gv.hasName() || !gv.getName().starts_with("??_R0")) {
        continue;
      }
      Type *tyGv = gv.getValueType();
      if (!tyGv->isStructTy() ||
          !tyGv->getStructName().starts_with("rtti.TypeDescriptor")) {
        continue;
      }

      ConstantStruct *   initStruct = cast<ConstantStruct>(gv.getInitializer());
      ConstantDataArray *rttiDA = cast<ConstantDataArray>(
          initStruct->getOperand(2));
      if (!rttiDA->isString()) {
        report_fatal_error("RTTI[2] is not a string.");
      }
      StringRef tyInfoString = rttiDA->getAsString();
      if (!tyInfoString.starts_with(".?AV") &&
          !tyInfoString.starts_with(".?AU")
      ) {
        continue;
      }
      auto      newRttiName = randomRttiName(tyInfoString);
      Constant *newRttiNameConstant = ConstantDataArray::getString(
          ctx, newRttiName, newRttiName[newRttiName.size() - 1] != '\0');

      SmallVector<Constant *, 4> Ops;
      for (unsigned k = 0; k < initStruct->getNumOperands(); ++k)
        Ops.push_back(initStruct->getOperand(k));
      Ops[2] = newRttiNameConstant;
      gv.setInitializer(ConstantStruct::get(initStruct->getType(), Ops));

      changed = true;
    }
    return changed;
  }

  SmallString<256> randomRttiName(StringRef rtti) {
    constexpr char pwTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    SmallString<512> passwd;
    passwd.append(ArgsOptions->randomSeed());
    passwd.append(rtti);
    Blake3.init();
    Blake3.update(passwd);
    const auto hash = Blake3.final();

    SmallString<256> result = rtti;

    for (size_t i = 4; i < result.size(); ++i) {
      const char currentChar = result[i];
      if (currentChar == '\0') {
        break;
      }
      if (currentChar == '@' || currentChar == '.' || currentChar == '?' ||
          currentChar == '$') {
        continue;
      }
      result[i] = pwTable[(currentChar ^ hash[i % sizeof(hash)]) % (
                            sizeof(pwTable) - 1)];
    }
    return result;
  }

  bool doFinalization(Module &) override {
    return false;
  }
};
}

char MsRttiEraser::ID = 0;

ModulePass *llvm::createMsRttiEraserPass(ObfuscationOptions *argsOptions) {
  return new MsRttiEraser(argsOptions);
}

INITIALIZE_PASS(MsRttiEraser, "ms_rtti_eraser", "Enable Microsoft RTTI Eraser",
                false, false)