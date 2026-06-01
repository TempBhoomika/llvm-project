//===- RuntimeInterfaceValidatorTest.cpp - Runtime Validator Tests ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains unit tests for the RuntimeInterfaceValidator class.
//
//===----------------------------------------------------------------------===//

#include "RuntimeInterfaceValidator.h"
#include "ErrorHandler.h"
#include "RuntimeInterface.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

class RuntimeInterfaceValidatorTest : public ::testing::Test {
protected:
  void SetUp() override {
    Context = std::make_unique<LLVMContext>();
    ErrHandler = std::make_unique<ErrorHandler>(*Context);
    Validator = std::make_unique<RuntimeInterfaceValidator>(*Context, *ErrHandler);
    
    // Create a test module
    Module = std::make_unique<llvm::Module>("test_module", *Context);
    
    // Initialize validator
    RuntimeInterface Interface;
    Validator->initialize(Interface);
  }
  
  void TearDown() override {
    Validator.reset();
    ErrHandler.reset();
    Module.reset();
    Context.reset();
  }
  
  Function* createTestFunction(StringRef Name, Type* RetType, ArrayRef<Type*> ParamTypes,
                               CallingConv::ID CC = CallingConv::C, bool IsVarArg = false) {
    FunctionType* FT = FunctionType::get(RetType, ParamTypes, IsVarArg);
    Function* F = Function::Create(FT, Function::ExternalLinkage, Name, Module.get());
    F->setCallingConv(CC);
    
    // Create a basic block with a return instruction
    BasicBlock* BB = BasicBlock::Create(*Context, "entry", F);
    IRBuilder<> Builder(BB);
    if (RetType->isVoidTy()) {
      Builder.CreateRetVoid();
    } else {
      Builder.CreateRet(Constant::getNullValue(RetType));
    }
    
    return F;
  }
  
  void addVersionMetadata(StringRef Version) {
    LLVMContext& Ctx = Module->getContext();
    MDString* VersionStr = MDString::get(Ctx, Version);
    MDNode* VersionNode = MDNode::get(Ctx, VersionStr);
    NamedMDNode* VersionMD = Module->getOrInsertNamedMetadata("mpi.sanitizer.version");
    VersionMD->addOperand(VersionNode);
  }
  
  void addABIMetadata(uint32_t ABI) {
    LLVMContext& Ctx = Module->getContext();
    ConstantInt* ABIInt = ConstantInt::get(Type::getInt32Ty(Ctx), ABI);
    MDNode* ABINode = MDNode::get(Ctx, ConstantAsMetadata::get(ABIInt));
    NamedMDNode* ABIMD = Module->getOrInsertNamedMetadata("mpi.sanitizer.abi");
    ABIMD->addOperand(ABINode);
  }
  
  std::unique_ptr<LLVMContext> Context;
  std::unique_ptr<ErrorHandler> ErrHandler;
  std::unique_ptr<RuntimeInterfaceValidator> Validator;
  std::unique_ptr<llvm::Module> Module;
};

TEST_F(RuntimeInterfaceValidatorTest, RuntimeVersionParsing) {
  // Test version parsing
  RuntimeVersion V1 = RuntimeVersion::parseFromString("1.2.3");
  EXPECT_EQ(V1.Major, 1u);
  EXPECT_EQ(V1.Minor, 2u);
  EXPECT_EQ(V1.Patch, 3u);
  EXPECT_EQ(V1.ABIVersion, 0u);
  
  RuntimeVersion V2 = RuntimeVersion::parseFromString("2.0.1-5");
  EXPECT_EQ(V2.Major, 2u);
  EXPECT_EQ(V2.Minor, 0u);
  EXPECT_EQ(V2.Patch, 1u);
  EXPECT_EQ(V2.ABIVersion, 5u);
}

TEST_F(RuntimeInterfaceValidatorTest, VersionCompatibility) {
  RuntimeVersion V1_0_0(1, 0, 0);
  RuntimeVersion V1_1_0(1, 1, 0);
  RuntimeVersion V1_2_0(1, 2, 0);
  RuntimeVersion V2_0_0(2, 0, 0);
  
  // Same major version, newer minor version should be compatible
  EXPECT_TRUE(V1_1_0.isCompatibleWith(V1_0_0));
  EXPECT_TRUE(V1_2_0.isCompatibleWith(V1_1_0));
  
  // Older minor version should not be compatible
  EXPECT_FALSE(V1_0_0.isCompatibleWith(V1_1_0));
  
  // Different major version should not be compatible
  EXPECT_FALSE(V2_0_0.isCompatibleWith(V1_2_0));
  EXPECT_FALSE(V1_2_0.isCompatibleWith(V2_0_0));
}

TEST_F(RuntimeInterfaceValidatorTest, HookSignatureValidation) {
  // Create expected signature
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  SmallVector<Type*, 2> ParamTypes = {PtrTy, PtrTy};
  
  HookSignature Expected("test_hook", VoidTy, ParamTypes);
  
  // Create matching function
  Function* MatchingFunc = createTestFunction("test_hook", VoidTy, ParamTypes);
  HookValidationResult Result1 = Validator->validateHookFunction(MatchingFunc, Expected);
  EXPECT_TRUE(Result1.IsValid);
  EXPECT_TRUE(Result1.Found);
  EXPECT_TRUE(Result1.Errors.empty());
  
  // Create function with wrong return type
  Type* IntTy = Type::getInt32Ty(*Context);
  Function* WrongReturnFunc = createTestFunction("test_hook2", IntTy, ParamTypes);
  HookValidationResult Result2 = Validator->validateHookFunction(WrongReturnFunc, Expected);
  EXPECT_FALSE(Result2.IsValid);
  EXPECT_FALSE(Result2.Errors.empty());
  
  // Create function with wrong parameter count
  SmallVector<Type*, 1> WrongParams = {PtrTy};
  Function* WrongParamFunc = createTestFunction("test_hook3", VoidTy, WrongParams);
  HookValidationResult Result3 = Validator->validateHookFunction(WrongParamFunc, Expected);
  EXPECT_FALSE(Result3.IsValid);
  EXPECT_FALSE(Result3.Errors.empty());
}

TEST_F(RuntimeInterfaceValidatorTest, ModuleValidationWithVersion) {
  // Add version metadata
  addVersionMetadata("1.2.0");
  addABIMetadata(1);
  
  // Create some expected hook functions
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* IntTy = Type::getInt32Ty(*Context);
  
  // Create required hooks
  createTestFunction("mpi_sanitizer_pre_call", VoidTy, {PtrTy, PtrTy});
  createTestFunction("mpi_sanitizer_post_call", VoidTy, {PtrTy, IntTy, PtrTy});
  createTestFunction("mpi_sanitizer_init", IntTy, {});
  createTestFunction("mpi_sanitizer_finalize", VoidTy, {});
  createTestFunction("mpi_sanitizer_report_error", VoidTy, {PtrTy, PtrTy});
  
  // Validate module
  InterfaceValidationResult Result = Validator->validateModule(*Module);
  
  EXPECT_TRUE(Result.RuntimeFound);
  EXPECT_EQ(Result.DetectedVersion.Major, 1u);
  EXPECT_EQ(Result.DetectedVersion.Minor, 2u);
  EXPECT_EQ(Result.DetectedVersion.Patch, 0u);
  EXPECT_TRUE(Result.passed());
}

TEST_F(RuntimeInterfaceValidatorTest, ModuleValidationMissingHooks) {
  // Add version metadata
  addVersionMetadata("1.0.0");
  
  // Create only some required hooks (missing others)
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  createTestFunction("mpi_sanitizer_pre_call", VoidTy, {PtrTy, PtrTy});
  // Missing other required hooks
  
  // Validate module
  InterfaceValidationResult Result = Validator->validateModule(*Module);
  
  EXPECT_TRUE(Result.RuntimeFound);
  EXPECT_FALSE(Result.passed()); // Should fail due to missing required hooks
  EXPECT_LT(Result.ValidRequiredHooks, Result.RequiredHooks);
}

TEST_F(RuntimeInterfaceValidatorTest, VersionCompatibilityChecking) {
  // Set minimum version requirement
  Validator->setMinimumRuntimeVersion(RuntimeVersion(1, 1, 0));
  
  // Test with compatible version
  addVersionMetadata("1.2.0");
  InterfaceValidationResult Result1 = Validator->validateModule(*Module);
  EXPECT_TRUE(Result1.RuntimeFound);
  EXPECT_TRUE(Result1.GlobalErrors.empty());
  
  // Test with incompatible version
  Module = std::make_unique<llvm::Module>("test_module2", *Context);
  addVersionMetadata("1.0.0"); // Below minimum
  InterfaceValidationResult Result2 = Validator->validateModule(*Module);
  EXPECT_TRUE(Result2.RuntimeFound);
  EXPECT_FALSE(Result2.GlobalErrors.empty());
}

TEST_F(RuntimeInterfaceValidatorTest, ABIValidation) {
  // Enable ABI validation
  Validator->setABIValidation(true);
  
  // Add compatible ABI version
  addVersionMetadata("1.0.0-2");
  addABIMetadata(2);
  
  InterfaceValidationResult Result1 = Validator->validateModule(*Module);
  EXPECT_TRUE(Result1.RuntimeFound);
  // Should pass ABI validation
  
  // Test with mismatched ABI (if we had a way to set expected ABI)
  // This would require extending the test setup
}

TEST_F(RuntimeInterfaceValidatorTest, OptionalHookHandling) {
  // Add version metadata
  addVersionMetadata("1.0.0");
  
  // Create only required hooks, skip optional ones
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* IntTy = Type::getInt32Ty(*Context);
  
  // Create all required hooks
  createTestFunction("mpi_sanitizer_pre_call", VoidTy, {PtrTy, PtrTy});
  createTestFunction("mpi_sanitizer_post_call", VoidTy, {PtrTy, IntTy, PtrTy});
  createTestFunction("mpi_sanitizer_init", IntTy, {});
  createTestFunction("mpi_sanitizer_finalize", VoidTy, {});
  createTestFunction("mpi_sanitizer_report_error", VoidTy, {PtrTy, PtrTy});
  
  // Don't create optional hooks (timing, comm_volume)
  
  InterfaceValidationResult Result = Validator->validateModule(*Module);
  
  EXPECT_TRUE(Result.RuntimeFound);
  EXPECT_TRUE(Result.passed()); // Should pass even without optional hooks
  EXPECT_EQ(Result.ValidRequiredHooks, Result.RequiredHooks);
}

TEST_F(RuntimeInterfaceValidatorTest, StrictValidationMode) {
  // Test strict vs non-strict validation
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* Int32Ty = Type::getInt32Ty(*Context);
  Type* Int64Ty = Type::getInt64Ty(*Context);
  
  HookSignature Expected("test_hook", VoidTy, {PtrTy, Int32Ty});
  
  // Create function with different integer size
  Function* DifferentIntFunc = createTestFunction("test_hook", VoidTy, {PtrTy, Int64Ty});
  
  // Test in strict mode
  Validator->setStrictValidation(true);
  HookValidationResult StrictResult = Validator->validateHookFunction(DifferentIntFunc, Expected);
  EXPECT_FALSE(StrictResult.IsValid);
  
  // Test in non-strict mode
  Validator->setStrictValidation(false);
  HookValidationResult NonStrictResult = Validator->validateHookFunction(DifferentIntFunc, Expected);
  EXPECT_TRUE(NonStrictResult.IsValid); // Should allow integer size differences
}

TEST_F(RuntimeInterfaceValidatorTest, CallingConventionValidation) {
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  HookSignature Expected("test_hook", VoidTy, {PtrTy});
  Expected.CallingConv = CallingConv::C;
  
  // Create function with correct calling convention
  Function* CorrectFunc = createTestFunction("test_hook1", VoidTy, {PtrTy}, CallingConv::C);
  HookValidationResult Result1 = Validator->validateHookFunction(CorrectFunc, Expected);
  EXPECT_TRUE(Result1.IsValid);
  
  // Create function with wrong calling convention
  Function* WrongFunc = createTestFunction("test_hook2", VoidTy, {PtrTy}, CallingConv::Fast);
  HookValidationResult Result2 = Validator->validateHookFunction(WrongFunc, Expected);
  EXPECT_FALSE(Result2.IsValid);
  EXPECT_FALSE(Result2.Errors.empty());
}

TEST_F(RuntimeInterfaceValidatorTest, VariadicFunctionValidation) {
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  // Test variadic signature
  HookSignature VariadicExpected("test_variadic", VoidTy, {PtrTy});
  VariadicExpected.IsVariadic = true;
  
  // Create matching variadic function
  Function* VariadicFunc = createTestFunction("test_variadic1", VoidTy, {PtrTy}, CallingConv::C, true);
  HookValidationResult Result1 = Validator->validateHookFunction(VariadicFunc, VariadicExpected);
  EXPECT_TRUE(Result1.IsValid);
  
  // Create non-variadic function (should fail)
  Function* NonVariadicFunc = createTestFunction("test_variadic2", VoidTy, {PtrTy}, CallingConv::C, false);
  HookValidationResult Result2 = Validator->validateHookFunction(NonVariadicFunc, VariadicExpected);
  EXPECT_FALSE(Result2.IsValid);
}

TEST_F(RuntimeInterfaceValidatorTest, ValidationReportGeneration) {
  // Add version and create some hooks with issues
  addVersionMetadata("1.0.0");
  
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* IntTy = Type::getInt32Ty(*Context);
  
  // Create some valid hooks
  createTestFunction("mpi_sanitizer_pre_call", VoidTy, {PtrTy, PtrTy});
  createTestFunction("mpi_sanitizer_init", IntTy, {});
  
  // Create hook with wrong signature
  createTestFunction("mpi_sanitizer_post_call", VoidTy, {PtrTy}); // Missing parameters
  
  // Missing some required hooks
  
  InterfaceValidationResult Result = Validator->validateModule(*Module);
  
  // Generate validation report
  std::string Report;
  raw_string_ostream OS(Report);
  Validator->generateValidationReport(Result, OS);
  
  std::string ReportStr = OS.str();
  
  // Check that report contains expected sections
  EXPECT_NE(ReportStr.find("Runtime Interface Validation Report"), std::string::npos);
  EXPECT_NE(ReportStr.find("Hook Validation Results"), std::string::npos);
  EXPECT_NE(ReportStr.find("PASS"), std::string::npos);
  EXPECT_NE(ReportStr.find("FAIL"), std::string::npos);
}

TEST_F(RuntimeInterfaceValidatorTest, CustomHookSignatures) {
  // Test adding custom hook signatures
  Type* VoidTy = Type::getVoidTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  HookSignature CustomHook("custom_hook", VoidTy, {PtrTy}, false, "custom");
  Validator->addExpectedHook(CustomHook);
  
  // Verify hook was added
  const HookSignature* Retrieved = Validator->getExpectedHook("custom_hook");
  EXPECT_NE(Retrieved, nullptr);
  EXPECT_EQ(Retrieved->Name, "custom_hook");
  EXPECT_FALSE(Retrieved->IsRequired);
  
  // Test removing hook
  Validator->removeExpectedHook("custom_hook");
  const HookSignature* Removed = Validator->getExpectedHook("custom_hook");
  EXPECT_EQ(Removed, nullptr);
}

TEST_F(RuntimeInterfaceValidatorTest, TypeCompatibilityChecking) {
  Type* Int32Ty = Type::getInt32Ty(*Context);
  Type* Int64Ty = Type::getInt64Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* IntPtrTy = Type::getInt32PtrTy(*Context);
  
  // Test exact type matching
  EXPECT_TRUE(Validator->areTypesCompatible(Int32Ty, Int32Ty));
  
  // Test pointer compatibility (should allow i8* as generic)
  EXPECT_TRUE(Validator->areTypesCompatible(IntPtrTy, PtrTy));
  EXPECT_TRUE(Validator->areTypesCompatible(PtrTy, IntPtrTy));
  
  // Test integer compatibility in non-strict mode
  Validator->setStrictValidation(false);
  EXPECT_TRUE(Validator->areTypesCompatible(Int32Ty, Int64Ty));
  
  // Test integer incompatibility in strict mode
  Validator->setStrictValidation(true);
  EXPECT_FALSE(Validator->areTypesCompatible(Int32Ty, Int64Ty));
}

} // anonymous namespace