add_llvm_component_library(LLVMObfuscation
  Utils.cpp
  ObfuscationPassManager.cpp
  ObfuscationOptions.cpp
  IndirectBranch.cpp
  IndirectCall.cpp
  IndirectGlobalVariable.cpp
  Flattening.cpp
  StringEncryption.cpp
  LegacyLowerSwitch.cpp
  ConstantIntEncryption.cpp
  ConstantFPEncryption.cpp
  MicrosoftRTTIEraser.cpp
  
  COMPONENT_NAME
  Obfuscation
  
  ADDITIONAL_HEADER_DIRS
  ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/Obfuscation
  
  DEPENDS
  intrinsics_gen

  LINK_COMPONENTS
  Core
  Support
  TargetParser
  TransformUtils
  Linker
  )
