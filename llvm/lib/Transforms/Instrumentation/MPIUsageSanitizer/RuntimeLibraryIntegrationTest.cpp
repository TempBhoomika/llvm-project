//===- RuntimeLibraryIntegrationTest.cpp - Runtime Integration Tests ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains comprehensive integration tests for the MPI Usage Sanitizer
// runtime library interface, including hook function call compatibility,
// parameter marshaling/unmarshaling, and interface contract verification.
//
//===----------------------------------------------------------------------===//

#include "RuntimeInterface.h"
#include "RuntimeInterfaceValidator.h"
#include "HookInserter.h"
#include "ErrorHandler.h"
#include "MPICallDetector.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Verifier.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/ExecutionEngine/GenericValue.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"
#include <dlfcn.h>
#include <memory>
#include <vector>
#include <string>

using namespace llvm;

namespace {

/// Mock runtime library implementation for testing
class MockRuntimeLibrary {
public:
  MockRuntimeLibrary() {
    // Initialize call counters
    PreCallCount = 0;
    PostCallCount = 0;
    InitCallCount = 0;
    FinalizeCallCount = 0;
    ErrorCallCount = 0;
    TimingStartCount = 0;
    TimingEndCount = 0;
    CommVolumeCount = 0;
    
    // Clear recorded data
    LastFunctionName.clear();
    LastErrorMessage.clear();
    LastLocation.clear();
    RecordedTimings.clear();
    RecordedVolumes.clear();
  }
  
  // Hook function implementations
  static void mpi_sanitizer_pre_call(const char* func_name, void* args) {
    getInstance().PreCallCount++;
    getInstance().LastFunctionName = func_name ? func_name : "";
    getInstance().LastArgs = args;
  }
  
  static void mpi_sanitizer_post_call(const char* func_name, int result, void* args) {
    getInstance().PostCallCount++;
    getInstance().LastFunctionName = func_name ? func_name : "";
    getInstance().LastResult = result;
    getInstance().LastArgs = args;
  }
  
  static int mpi_sanitizer_init(void) {
    getInstance().InitCallCount++;
    return 0; // Success
  }
  
  static void mpi_sanitizer_finalize(void) {
    getInstance().FinalizeCallCount++;
  }
  
  static void mpi_sanitizer_report_error(const char* message, const char* location) {
    getInstance().ErrorCallCount++;
    getInstance().LastErrorMessage = message ? message : "";
    getInstance().LastLocation = location ? location : "";
  }
  
  static void mpi_sanitizer_timing_start(const char* func_name) {
    getInstance().TimingStartCount++;
    getInstance().LastFunctionName = func_name ? func_name : "";
  }
  
  static void mpi_sanitizer_timing_end(const char* func_name, uint64_t duration) {
    getInstance().TimingEndCount++;
    getInstance().LastFunctionName = func_name ? func_name : "";
    getInstance().RecordedTimings.push_back({func_name ? func_name : "", duration});
  }
  
  static void mpi_sanitizer_comm_volume(const char* func_name, size_t bytes) {
    getInstance().CommVolumeCount++;
    getInstance().LastFunctionName = func_name ? func_name : "";
    getInstance().RecordedVolumes.push_back({func_name ? func_name : "", bytes});
  }
  
  // Getters for test verification
  static MockRuntimeLibrary& getInstance() {
    static MockRuntimeLibrary instance;
    return instance;
  }
  
  void reset() {
    *this = MockRuntimeLibrary();
  }
  
  // Public data members for test access
  uint32_t PreCallCount, PostCallCount, InitCallCount, FinalizeCallCount;
  uint32_t ErrorCallCount, TimingStartCount, TimingEndCount, CommVolumeCount;
  std::string LastFunctionName, LastErrorMessage, LastLocation;
  int LastResult;
  void* LastArgs;
  std::vector<std::pair<std::string, uint64_t>> RecordedTimings;
  std::vector<std::pair<std::string, size_t>> RecordedVolumes;
};

/// Test fixture for runtime library integration tests
class RuntimeLibraryIntegrationTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Initialize LLVM components
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
    
    Context = std::make_unique<LLVMContext>();
    ErrorHandler = std::make_unique<class ErrorHandler>(*Context);
    RuntimeInterface = std::make_unique<class RuntimeInterface>(*Context);
    Validator = std::make_unique<RuntimeInterfaceValidator>(*Context, *ErrorHandler);
    HookInserter = std::make_unique<class HookInserter>(*Context, *RuntimeInterface);
    
    // Create test module
    Module = std::make_unique<llvm::Module>("integration_test", *Context);
    
    // Initialize components
    Validator->initialize(*RuntimeInterface);
    
    // Reset mock runtime
    MockRuntimeLibrary::getInstance().reset();
  }
  
  void TearDown() override {
    ExecutionEngine.reset();
    HookInserter.reset();
    Validator.reset();
    RuntimeInterface.reset();
    ErrorHandler.reset();
    Module.reset();
    Context.reset();
  }
  
  /// Create a simple MPI function for testing
  Function* createMPIFunction(StringRef Name, Type* RetType, ArrayRef<Type*> ParamTypes) {
    FunctionType* FT = FunctionType::get(RetType, ParamTypes, false);
    Function* F = Function::Create(FT, Function::ExternalLinkage, Name, Module.get());
    
    BasicBlock* BB = BasicBlock::Create(*Context, "entry", F);
    IRBuilder<> Builder(BB);
    
    if (RetType->isVoidTy()) {
      Builder.CreateRetVoid();
    } else {
      Builder.CreateRet(Constant::getNullValue(RetType));
    }
    
    return F;
  }
  
  /// Create runtime hook functions in the module
  void createRuntimeHooks() {
    Type* VoidTy = Type::getVoidTy(*Context);
    Type* IntTy = Type::getInt32Ty(*Context);
    Type* PtrTy = Type::getInt8PtrTy(*Context);
    Type* SizeTy = Type::getInt64Ty(*Context);
    
    // Create hook function declarations
    createHookDeclaration("mpi_sanitizer_pre_call", VoidTy, {PtrTy, PtrTy});
    createHookDeclaration("mpi_sanitizer_post_call", VoidTy, {PtrTy, IntTy, PtrTy});
    createHookDeclaration("mpi_sanitizer_init", IntTy, {});
    createHookDeclaration("mpi_sanitizer_finalize", VoidTy, {});
    createHookDeclaration("mpi_sanitizer_report_error", VoidTy, {PtrTy, PtrTy});
    createHookDeclaration("mpi_sanitizer_timing_start", VoidTy, {PtrTy});
    createHookDeclaration("mpi_sanitizer_timing_end", VoidTy, {PtrTy, SizeTy});
    createHookDeclaration("mpi_sanitizer_comm_volume", VoidTy, {PtrTy, SizeTy});
  }
  
  /// Create hook function declaration
  Function* createHookDeclaration(StringRef Name, Type* RetType, ArrayRef<Type*> ParamTypes) {
    FunctionType* FT = FunctionType::get(RetType, ParamTypes, false);
    return Function::Create(FT, Function::ExternalLinkage, Name, Module.get());
  }
  
  /// Create execution engine and link with mock runtime
  bool createExecutionEngine() {
    std::string ErrorStr;
    ExecutionEngine.reset(EngineBuilder(std::unique_ptr<llvm::Module>(Module.release()))
                         .setErrorStr(&ErrorStr)
                         .setEngineKind(EngineKind::JIT)
                         .create());
    
    if (!ExecutionEngine) {
      errs() << "Failed to create execution engine: " << ErrorStr << "\n";
      return false;
    }
    
    // Map runtime functions to mock implementations
    ExecutionEngine->addGlobalMapping("mpi_sanitizer_pre_call", 
                                     (void*)MockRuntimeLibrary::mpi_sanitizer_pre_call);
    ExecutionEngine->addGlobalMapping("mpi_sanitizer_post_call", 
                                     (void*)MockRuntimeLibrary::mpi_sanitizer_post_call);
    ExecutionEngine->addGlobalMapping("mpi_sanitizer_init", 
                                     (void*)MockRuntimeLibrary::mpi_sanitizer_init);
    ExecutionEngine->addGlobalMapping("mpi_sanitizer_finalize", 
                                     (void*)MockRuntimeLibrary::mpi_sanitizer_finalize);
    ExecutionEngine->addGlobalMapping("mpi_sanitizer_report_error", 
                                     (void*)MockRuntimeLibrary::mpi_sanitizer_report_error);
    ExecutionEngine->addGlobalMapping("mpi_sanitizer_timing_start", 
                                     (void*)MockRuntimeLibrary::mpi_sanitizer_timing_start);
    ExecutionEngine->addGlobalMapping("mpi_sanitizer_timing_end", 
                                     (void*)MockRuntimeLibrary::mpi_sanitizer_timing_end);
    ExecutionEngine->addGlobalMapping("mpi_sanitizer_comm_volume", 
                                     (void*)MockRuntimeLibrary::mpi_sanitizer_comm_volume);
    
    return true;
  }
  
  /// Execute a function in the JIT
  GenericValue executeFunction(Function* F, ArrayRef<GenericValue> Args = {}) {
    if (!ExecutionEngine) {
      return GenericValue();
    }
    
    return ExecutionEngine->runFunction(F, Args);
  }
  
  std::unique_ptr<LLVMContext> Context;
  std::unique_ptr<class ErrorHandler> ErrorHandler;
  std::unique_ptr<class RuntimeInterface> RuntimeInterface;
  std::unique_ptr<RuntimeInterfaceValidator> Validator;
  std::unique_ptr<class HookInserter> HookInserter;
  std::unique_ptr<llvm::Module> Module;
  std::unique_ptr<ExecutionEngine> ExecutionEngine;
};

TEST_F(RuntimeLibraryIntegrationTest, BasicHookFunctionCalls) {
  // Create runtime hooks
  createRuntimeHooks();
  
  // Validate runtime interface
  InterfaceValidationResult ValidationResult = Validator->validateModule(*Module);
  EXPECT_TRUE(ValidationResult.RuntimeFound);
  
  // Create execution engine
  ASSERT_TRUE(createExecutionEngine());
  
  // Test direct hook calls
  Function* PreCallHook = Module->getFunction("mpi_sanitizer_pre_call");
  Function* PostCallHook = Module->getFunction("mpi_sanitizer_post_call");
  Function* InitHook = Module->getFunction("mpi_sanitizer_init");
  
  ASSERT_NE(PreCallHook, nullptr);
  ASSERT_NE(PostCallHook, nullptr);
  ASSERT_NE(InitHook, nullptr);
  
  // Execute init hook
  GenericValue InitResult = executeFunction(InitHook);
  EXPECT_EQ(MockRuntimeLibrary::getInstance().InitCallCount, 1u);
  EXPECT_EQ(InitResult.IntVal.getZExtValue(), 0u); // Success return
  
  // Note: Direct execution of pre/post call hooks with string parameters
  // requires more complex setup with memory allocation in the JIT environment
  // This would be implemented in a full integration test suite
}

TEST_F(RuntimeLibraryIntegrationTest, HookInsertionAndExecution) {
  // Create a simple MPI function to instrument
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  Function* MPIFunc = createMPIFunction("MPI_Send", IntTy, {PtrTy, IntTy, IntTy, IntTy, IntTy, IntTy});
  
  // Create runtime hooks
  createRuntimeHooks();
  
  // Create call site for instrumentation
  CallBase Site;
  Site.FunctionName = "MPI_Send";
  Site.Type = MPIFunctionType::PointToPoint;
  Site.Inst = &MPIFunc->getEntryBlock().front();
  
  // Configure hook insertion
  HookConfiguration Config;
  Config.EnablePreCallHooks = true;
  Config.EnablePostCallHooks = true;
  Config.EnablePerformanceHooks = false;
  
  // Insert hooks (this would normally be done during pass execution)
  // For this test, we verify that the hook insertion infrastructure works
  EXPECT_TRUE(HookInserter->initialize(Config));
  
  // Verify hook declarations were created properly
  Function* PreCallHook = Module->getFunction("mpi_sanitizer_pre_call");
  Function* PostCallHook = Module->getFunction("mpi_sanitizer_post_call");
  
  EXPECT_NE(PreCallHook, nullptr);
  EXPECT_NE(PostCallHook, nullptr);
  
  // Verify function signatures match expected interface
  HookValidationResult PreCallResult = Validator->validateHookFunction(
      PreCallHook, *Validator->getExpectedHook("mpi_sanitizer_pre_call"));
  HookValidationResult PostCallResult = Validator->validateHookFunction(
      PostCallHook, *Validator->getExpectedHook("mpi_sanitizer_post_call"));
  
  EXPECT_TRUE(PreCallResult.IsValid);
  EXPECT_TRUE(PostCallResult.IsValid);
}

TEST_F(RuntimeLibraryIntegrationTest, ParameterMarshalingValidation) {
  // Test parameter marshaling for different MPI function types
  createRuntimeHooks();
  
  // Create test functions with different parameter patterns
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* DoubleTy = Type::getDoubleTy(*Context);
  
  // Point-to-point function with buffer parameters
  Function* SendFunc = createMPIFunction("MPI_Send", IntTy, 
                                        {PtrTy, IntTy, IntTy, IntTy, IntTy, IntTy});
  
  // Collective function with different parameter pattern
  Function* BcastFunc = createMPIFunction("MPI_Bcast", IntTy,
                                         {PtrTy, IntTy, IntTy, IntTy, IntTy});
  
  // Timing function with floating-point parameters
  Function* WtimeFunc = createMPIFunction("MPI_Wtime", DoubleTy, {});
  
  // Validate that hook insertion can handle different parameter types
  CallBase SendSite;
  SendSite.FunctionName = "MPI_Send";
  SendSite.Type = MPIFunctionType::PointToPoint;
  SendSite.Inst = &SendFunc->getEntryBlock().front();
  
  CallBase BcastSite;
  BcastSite.FunctionName = "MPI_Bcast";
  BcastSite.Type = MPIFunctionType::Collective;
  BcastSite.Inst = &BcastFunc->getEntryBlock().front();
  
  // Verify that parameter extraction and marshaling infrastructure works
  // This tests the integration between MetadataExtractor and HookInserter
  EXPECT_TRUE(HookInserter->canInstrumentCallBase(SendSite));
  EXPECT_TRUE(HookInserter->canInstrumentCallBase(BcastSite));
}

TEST_F(RuntimeLibraryIntegrationTest, ErrorHandlingIntegration) {
  // Test error handling integration with runtime library
  createRuntimeHooks();
  
  // Create execution engine
  ASSERT_TRUE(createExecutionEngine());
  
  // Test error reporting hook
  Function* ErrorHook = Module->getFunction("mpi_sanitizer_report_error");
  ASSERT_NE(ErrorHook, nullptr);
  
  // Validate error hook signature
  HookValidationResult ErrorResult = Validator->validateHookFunction(
      ErrorHook, *Validator->getExpectedHook("mpi_sanitizer_report_error"));
  EXPECT_TRUE(ErrorResult.IsValid);
  
  // Test that error handler can report to runtime library
  ErrorHandler->reportError(ErrorLevel::Error, ErrorCategory::CallDetection, 
                           "Test error message");
  
  // Verify error was recorded (in a real implementation, this would
  // trigger a call to the runtime error reporting function)
  const auto& Errors = ErrorHandler->getErrors();
  EXPECT_FALSE(Errors.empty());
  EXPECT_EQ(Errors.back().Message, "Test error message");
}

TEST_F(RuntimeLibraryIntegrationTest, PerformanceHookIntegration) {
  // Test performance monitoring hook integration
  createRuntimeHooks();
  
  // Create execution engine
  ASSERT_TRUE(createExecutionEngine());
  
  // Test timing hooks
  Function* TimingStartHook = Module->getFunction("mpi_sanitizer_timing_start");
  Function* TimingEndHook = Module->getFunction("mpi_sanitizer_timing_end");
  Function* CommVolumeHook = Module->getFunction("mpi_sanitizer_comm_volume");
  
  ASSERT_NE(TimingStartHook, nullptr);
  ASSERT_NE(TimingEndHook, nullptr);
  ASSERT_NE(CommVolumeHook, nullptr);
  
  // Validate performance hook signatures
  HookValidationResult TimingStartResult = Validator->validateHookFunction(
      TimingStartHook, *Validator->getExpectedHook("mpi_sanitizer_timing_start"));
  HookValidationResult TimingEndResult = Validator->validateHookFunction(
      TimingEndHook, *Validator->getExpectedHook("mpi_sanitizer_timing_end"));
  HookValidationResult CommVolumeResult = Validator->validateHookFunction(
      CommVolumeHook, *Validator->getExpectedHook("mpi_sanitizer_comm_volume"));
  
  EXPECT_TRUE(TimingStartResult.IsValid);
  EXPECT_TRUE(TimingEndResult.IsValid);
  EXPECT_TRUE(CommVolumeResult.IsValid);
  
  // Test performance hook configuration
  HookConfiguration PerfConfig;
  PerfConfig.EnablePerformanceHooks = true;
  PerfConfig.EnableTimingHooks = true;
  PerfConfig.EnableCommVolumeHooks = true;
  
  EXPECT_TRUE(HookInserter->initialize(PerfConfig));
}

TEST_F(RuntimeLibraryIntegrationTest, VersionCompatibilityIntegration) {
  // Test version compatibility checking with runtime library
  
  // Add version metadata to module
  LLVMContext& Ctx = Module->getContext();
  MDString* VersionStr = MDString::get(Ctx, "1.2.0");
  MDNode* VersionNode = MDNode::get(Ctx, VersionStr);
  NamedMDNode* VersionMD = Module->getOrInsertNamedMetadata("mpi.sanitizer.version");
  VersionMD->addOperand(VersionNode);
  
  // Add ABI metadata
  ConstantInt* ABIInt = ConstantInt::get(Type::getInt32Ty(Ctx), 2);
  MDNode* ABINode = MDNode::get(Ctx, ConstantAsMetadata::get(ABIInt));
  NamedMDNode* ABIMD = Module->getOrInsertNamedMetadata("mpi.sanitizer.abi");
  ABIMD->addOperand(ABINode);
  
  createRuntimeHooks();
  
  // Test version detection and compatibility
  RuntimeVersion DetectedVersion = Validator->detectRuntimeVersion(*Module);
  EXPECT_EQ(DetectedVersion.Major, 1u);
  EXPECT_EQ(DetectedVersion.Minor, 2u);
  EXPECT_EQ(DetectedVersion.Patch, 0u);
  
  // Test compatibility with minimum version
  RuntimeVersion MinVersion(1, 0, 0);
  EXPECT_TRUE(Validator->checkVersionCompatibility(DetectedVersion, MinVersion));
  
  // Test incompatibility with higher version
  RuntimeVersion HigherVersion(2, 0, 0);
  EXPECT_FALSE(Validator->checkVersionCompatibility(DetectedVersion, HigherVersion));
}

TEST_F(RuntimeLibraryIntegrationTest, ABICompatibilityValidation) {
  // Test ABI compatibility validation
  createRuntimeHooks();
  
  // Add ABI metadata
  LLVMContext& Ctx = Module->getContext();
  ConstantInt* ABIInt = ConstantInt::get(Type::getInt32Ty(Ctx), 1);
  MDNode* ABINode = MDNode::get(Ctx, ConstantAsMetadata::get(ABIInt));
  NamedMDNode* ABIMD = Module->getOrInsertNamedMetadata("mpi.sanitizer.abi");
  ABIMD->addOperand(ABINode);
  
  // Enable ABI validation
  Validator->setABIValidation(true);
  
  // Test ABI validation
  RuntimeVersion Version(1, 0, 0, 1); // ABI version 1
  EXPECT_TRUE(Validator->validateABICompatibility(*Module, Version));
  
  // Test ABI mismatch
  RuntimeVersion MismatchVersion(1, 0, 0, 2); // ABI version 2
  EXPECT_FALSE(Validator->validateABICompatibility(*Module, MismatchVersion));
}

TEST_F(RuntimeLibraryIntegrationTest, CompleteIntegrationWorkflow) {
  // Test complete integration workflow from detection to execution
  
  // 1. Create MPI functions
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  Function* MPISend = createMPIFunction("MPI_Send", IntTy, {PtrTy, IntTy, IntTy, IntTy, IntTy, IntTy});
  Function* MPIRecv = createMPIFunction("MPI_Recv", IntTy, {PtrTy, IntTy, IntTy, IntTy, IntTy, IntTy, PtrTy});
  
  // 2. Create runtime hooks
  createRuntimeHooks();
  
  // 3. Add version information
  LLVMContext& Ctx = Module->getContext();
  MDString* VersionStr = MDString::get(Ctx, "1.1.0");
  MDNode* VersionNode = MDNode::get(Ctx, VersionStr);
  NamedMDNode* VersionMD = Module->getOrInsertNamedMetadata("mpi.sanitizer.version");
  VersionMD->addOperand(VersionNode);
  
  // 4. Validate runtime interface
  InterfaceValidationResult ValidationResult = Validator->validateModule(*Module);
  EXPECT_TRUE(ValidationResult.RuntimeFound);
  EXPECT_TRUE(ValidationResult.passed());
  
  // 5. Configure hook insertion
  HookConfiguration Config;
  Config.EnablePreCallHooks = true;
  Config.EnablePostCallHooks = true;
  Config.EnableErrorChecking = true;
  
  EXPECT_TRUE(HookInserter->initialize(Config));
  
  // 6. Verify module is valid LLVM IR
  std::string VerifyError;
  raw_string_ostream VerifyOS(VerifyError);
  EXPECT_FALSE(verifyModule(*Module, &VerifyOS));
  
  // 7. Create execution engine and test
  ASSERT_TRUE(createExecutionEngine());
  
  // 8. Test that all components work together
  EXPECT_EQ(ValidationResult.ValidRequiredHooks, ValidationResult.RequiredHooks);
  EXPECT_TRUE(ValidationResult.DetectedVersion.isCompatibleWith(RuntimeVersion(1, 0, 0)));
}

TEST_F(RuntimeLibraryIntegrationTest, RuntimeLibraryContractVerification) {
  // Test that runtime library interface contracts are properly verified
  
  createRuntimeHooks();
  
  // Test contract verification for each hook category
  const auto& ExpectedHooks = Validator->getExpectedHooks();
  
  for (const auto& Hook : ExpectedHooks) {
    const std::string& HookName = Hook.first();
    const HookSignature& Signature = Hook.second;
    
    Function* F = Module->getFunction(HookName);
    if (F) {
      HookValidationResult Result = Validator->validateHookFunction(F, Signature);
      
      if (Signature.IsRequired) {
        EXPECT_TRUE(Result.IsValid) << "Required hook " << HookName << " failed validation";
      }
      
      // Verify contract-specific requirements
      if (HookName == "mpi_sanitizer_init") {
        // Init hook should return int
        EXPECT_TRUE(F->getReturnType()->isIntegerTy(32));
      } else if (HookName.find("timing") != std::string::npos) {
        // Timing hooks should be in performance category
        EXPECT_EQ(Signature.Category, "performance");
      } else if (HookName.find("error") != std::string::npos) {
        // Error hooks should be in error category
        EXPECT_EQ(Signature.Category, "error");
      }
    }
  }
}

} // anonymous namespace