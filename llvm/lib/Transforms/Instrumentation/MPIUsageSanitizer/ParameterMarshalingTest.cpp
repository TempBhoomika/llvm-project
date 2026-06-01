//===- ParameterMarshalingTest.cpp - Parameter Marshaling Tests ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains comprehensive tests for parameter marshaling and
// unmarshaling between the LLVM pass and runtime library interface.
//
//===----------------------------------------------------------------------===//

#include "RuntimeInterface.h"
#include "MetadataExtractor.h"
#include "HookInserter.h"
#include "MPICallDetector.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DataLayout.h"
#include "gtest/gtest.h"
#include <memory>
#include <vector>

using namespace llvm;

namespace {

struct BufferInfo {
  Type* ElementType = nullptr;
  uint64_t Count = 0;
};

/// Test parameter marshaling data structure
struct MarshaledParameter {
  /// Parameter type
  Type* ParamType = nullptr;
  
  /// Parameter value (for constants)
  Constant* ConstValue = nullptr;
  
  /// Parameter instruction (for variables)
  Value* ParamValue = nullptr;
  
  /// Marshaled size in bytes
  uint64_t MarshaledSize = 0;
  
  /// Parameter role (buffer, count, type, etc.)
  ParameterRole Role = ParameterRole::Unknown;
  
  /// Whether parameter is input, output, or both
  bool IsInput = false;
  bool IsOutput = false;
  
  /// Buffer information (if applicable)
  BufferInfo Buffer;
};

/// Mock parameter marshaling interface for testing
class MockParameterMarshaler {
public:
  MockParameterMarshaler(LLVMContext& Context) : Context(Context) {}
  
  /// Marshal parameters for runtime library call
  std::vector<MarshaledParameter> marshalParameters(const CallSite& Site, 
                                                    const MPICallMetadata& Metadata) {
    std::vector<MarshaledParameter> Marshaled;
    
    // Marshal each parameter based on its role and type
    for (size_t i = 0; i < Metadata.ParameterInfos.size(); ++i) {
      const ParameterInfo& ParamInfo = Metadata.ParameterInfos[i];
      MarshaledParameter MP;
      
      MP.ParamType = ParamInfo.ParamType;
      MP.Role = ParamInfo.Role;
      MP.IsInput = ParamInfo.IsInput;
      MP.IsOutput = ParamInfo.IsOutput;
      
      // Calculate marshaled size
      MP.MarshaledSize = calculateMarshaledSize(ParamInfo.Type);
      
      // Handle different parameter roles
      switch (ParamInfo.Role) {
        case ParameterRole::Buffer:
          MP.Buffer.ElementType = ParamInfo.ParamType;
          MP.Buffer.Count = 1;
          break;
        case ParameterRole::Count:
        case ParameterRole::Datatype:
        case ParameterRole::Communicator:
        case ParameterRole::Request:
          // These are typically simple values
          break;
        default:
          break;
      }
      
      Marshaled.push_back(MP);
    }
    
    return Marshaled;
  }
  
  /// Unmarshal parameters from runtime library call
  bool unmarshalParameters(const std::vector<MarshaledParameter>& Marshaled,
                           CallSite& Site) {
    // Verify that output parameters can be properly unmarshaled
    for (const auto& MP : Marshaled) {
      if (MP.IsOutput) {
        if (!canUnmarshalParameter(MP)) {
          return false;
        }
      }
    }
    return true;
  }
  
  /// Calculate marshaled size for a type
  uint64_t calculateMarshaledSize(Type* Ty) {
    if (!Ty) return 0;
    
    if (Ty->isPointerTy()) {
      return 8; // Pointer size
    } else if (Ty->isIntegerTy()) {
      return (Ty->getIntegerBitWidth() + 7) / 8; // Round up to bytes
    } else if (Ty->isFloatingPointTy()) {
      if (Ty->isFloatTy()) return 4;
      if (Ty->isDoubleTy()) return 8;
      return 16; // Long double or other
    } else if (Ty->isStructTy()) {
      // Calculate struct size (simplified)
      uint64_t Size = 0;
      StructType* ST = cast<StructType>(Ty);
      for (unsigned i = 0; i < ST->getNumElements(); ++i) {
        Size += calculateMarshaledSize(ST->getElementType(i));
      }
      return Size;
    }
    
    return 8; // Default size
  }
  
  /// Check if parameter can be unmarshaled
  bool canUnmarshalParameter(const MarshaledParameter& MP) {
    // Check if the parameter type supports unmarshaling
    if (!MP.ParamType) return false;
    
    // Output parameters must be pointers or references
    if (MP.IsOutput && !MP.ParamType->isPointerTy()) {
      return false;
    }
    
    // Buffer parameters need valid buffer info
    if (MP.Role == ParameterRole::Buffer && MP.IsOutput) {
      return MP.Buffer.ElementType != nullptr && MP.Buffer.Count > 0;
    }
    
    return true;
  }

private:
  LLVMContext& Context;
};

/// Test fixture for parameter marshaling tests
class ParameterMarshalingTest : public ::testing::Test {
protected:
  void SetUp() override {
    Context = std::make_unique<LLVMContext>();
    Module = std::make_unique<llvm::Module>("marshaling_test", *Context);
    RuntimeIntf = std::make_unique<llvm::RuntimeInterface>();
    MetaExtractor = std::make_unique<llvm::MetadataExtractor>();
    Marshaler = std::make_unique<MockParameterMarshaler>(*Context);
  }
  
  void TearDown() override {
    Marshaler.reset();
    MetaExtractor.reset();
    RuntimeIntf.reset();
    Module.reset();
    Context.reset();
  }
  
  /// Create a test MPI function call
  CallInst* createMPICall(StringRef FuncName, FunctionType* FT, ArrayRef<Value*> Args) {
    Function* MPIFunc = Function::Create(FT, Function::ExternalLinkage, FuncName, Module.get());
    
    // Create a test function to contain the call
    FunctionType* TestFT = FunctionType::get(Type::getVoidTy(*Context), false);
    Function* TestFunc = Function::Create(TestFT, Function::ExternalLinkage, "test", Module.get());
    BasicBlock* BB = BasicBlock::Create(*Context, "entry", TestFunc);
    
    IRBuilder<> Builder(BB);
    CallInst* Call = Builder.CreateCall(MPIFunc, Args);
    Builder.CreateRetVoid();
    
    return Call;
  }
  
  /// Create test values for different types
  Value* createTestValue(Type* Ty, StringRef Name = "test_val") {
    if (Ty->isPointerTy()) {
      return Constant::getNullValue(Ty);
    } else if (Ty->isIntegerTy()) {
      return ConstantInt::get(Ty, 42);
    } else if (Ty->isFloatingPointTy()) {
      return ConstantFP::get(Ty, 3.14);
    }
    return Constant::getNullValue(Ty);
  }
  
  std::unique_ptr<LLVMContext> Context;
  std::unique_ptr<llvm::Module> Module;
  std::unique_ptr<llvm::RuntimeInterface> RuntimeIntf;
  std::unique_ptr<llvm::MetadataExtractor> MetaExtractor;
  std::unique_ptr<MockParameterMarshaler> Marshaler;
};

TEST_F(ParameterMarshalingTest, BasicParameterMarshaling) {
  // Test basic parameter marshaling for MPI_Send
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  // Create MPI_Send call: MPI_Send(buf, count, datatype, dest, tag, comm)
  SmallVector<Type*, 6> ParamTypes = {PtrTy, IntTy, IntTy, IntTy, IntTy, IntTy};
  FunctionType* SendFT = FunctionType::get(IntTy, ParamTypes, false);
  
  SmallVector<Value*, 6> Args;
  for (Type* Ty : ParamTypes) {
    Args.push_back(createTestValue(Ty));
  }
  
  CallInst* SendCall = createMPICall("MPI_Send", SendFT, Args);
  
  // Create call site
  CallSite Site;
  Site.FunctionName = "MPI_Send";
  Site.Type = MPIFunctionType::PointToPoint;
  Site.Inst = SendCall;
  
  // Extract metadata
  MPICallMetadata Metadata = MetaExtractor->extractMetadata(Site);
  
  // Marshal parameters
  std::vector<MarshaledParameter> Marshaled = Marshaler->marshalParameters(Site, Metadata);
  
  // Verify marshaling results
  EXPECT_EQ(Marshaled.size(), 6u);
  
  // Check buffer parameter (first parameter)
  EXPECT_EQ(Marshaled[0].Role, ParameterRole::Buffer);
  EXPECT_TRUE(Marshaled[0].IsInput);
  EXPECT_FALSE(Marshaled[0].IsOutput);
  EXPECT_EQ(Marshaled[0].MarshaledSize, 8u); // Pointer size
  
  // Check count parameter (second parameter)
  EXPECT_EQ(Marshaled[1].Role, ParameterRole::Count);
  EXPECT_TRUE(Marshaled[1].IsInput);
  EXPECT_EQ(Marshaled[1].MarshaledSize, 4u); // int32 size
  
  // Check communicator parameter (last parameter)
  EXPECT_EQ(Marshaled[5].Role, ParameterRole::Communicator);
  EXPECT_TRUE(Marshaled[5].IsInput);
}

TEST_F(ParameterMarshalingTest, OutputParameterMarshaling) {
  // Test marshaling for functions with output parameters (MPI_Recv)
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* StatusPtrTy = PtrTy; // MPI_Status*
  
  // Create MPI_Recv call: MPI_Recv(buf, count, datatype, source, tag, comm, status)
  SmallVector<Type*, 7> ParamTypes = {PtrTy, IntTy, IntTy, IntTy, IntTy, IntTy, StatusPtrTy};
  FunctionType* RecvFT = FunctionType::get(IntTy, ParamTypes, false);
  
  SmallVector<Value*, 7> Args;
  for (Type* Ty : ParamTypes) {
    Args.push_back(createTestValue(Ty));
  }
  
  CallInst* RecvCall = createMPICall("MPI_Recv", RecvFT, Args);
  
  // Create call site
  CallSite Site;
  Site.FunctionName = "MPI_Recv";
  Site.Type = MPIFunctionType::PointToPoint;
  Site.Inst = RecvCall;
  
  // Extract metadata
  MPICallMetadata Metadata = MetaExtractor->extractMetadata(Site);
  
  // Marshal parameters
  std::vector<MarshaledParameter> Marshaled = Marshaler->marshalParameters(Site, Metadata);
  
  // Verify marshaling results
  EXPECT_EQ(Marshaled.size(), 7u);
  
  // Check buffer parameter (output)
  EXPECT_EQ(Marshaled[0].Role, ParameterRole::Buffer);
  EXPECT_FALSE(Marshaled[0].IsInput);
  EXPECT_TRUE(Marshaled[0].IsOutput);
  
  // Check status parameter (output)
  EXPECT_EQ(Marshaled[6].Role, ParameterRole::Status);
  EXPECT_FALSE(Marshaled[6].IsInput);
  EXPECT_TRUE(Marshaled[6].IsOutput);
  
  // Test unmarshaling
  bool CanUnmarshal = Marshaler->unmarshalParameters(Marshaled, Site);
  EXPECT_TRUE(CanUnmarshal);
}

TEST_F(ParameterMarshalingTest, CollectiveParameterMarshaling) {
  // Test marshaling for collective operations (MPI_Bcast)
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  // Create MPI_Bcast call: MPI_Bcast(buffer, count, datatype, root, comm)
  SmallVector<Type*, 5> ParamTypes = {PtrTy, IntTy, IntTy, IntTy, IntTy};
  FunctionType* BcastFT = FunctionType::get(IntTy, ParamTypes, false);
  
  SmallVector<Value*, 5> Args;
  for (Type* Ty : ParamTypes) {
    Args.push_back(createTestValue(Ty));
  }
  
  CallInst* BcastCall = createMPICall("MPI_Bcast", BcastFT, Args);
  
  // Create call site
  CallSite Site;
  Site.FunctionName = "MPI_Bcast";
  Site.Type = MPIFunctionType::Collective;
  Site.Inst = BcastCall;
  
  // Extract metadata
  MPICallMetadata Metadata = MetaExtractor->extractMetadata(Site);
  
  // Marshal parameters
  std::vector<MarshaledParameter> Marshaled = Marshaler->marshalParameters(Site, Metadata);
  
  // Verify marshaling results
  EXPECT_EQ(Marshaled.size(), 5u);
  
  // Buffer parameter is both input and output for broadcast
  EXPECT_EQ(Marshaled[0].Role, ParameterRole::Buffer);
  EXPECT_TRUE(Marshaled[0].IsInput);
  EXPECT_TRUE(Marshaled[0].IsOutput);
  
  // Root parameter
  EXPECT_EQ(Marshaled[3].Role, ParameterRole::Root);
  EXPECT_TRUE(Marshaled[3].IsInput);
  EXPECT_FALSE(Marshaled[3].IsOutput);
}

TEST_F(ParameterMarshalingTest, NonBlockingParameterMarshaling) {
  // Test marshaling for non-blocking operations (MPI_Isend)
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* RequestPtrTy = PtrTy; // MPI_Request*
  
  // Create MPI_Isend call: MPI_Isend(buf, count, datatype, dest, tag, comm, request)
  SmallVector<Type*, 7> ParamTypes = {PtrTy, IntTy, IntTy, IntTy, IntTy, IntTy, RequestPtrTy};
  FunctionType* IsendFT = FunctionType::get(IntTy, ParamTypes, false);
  
  SmallVector<Value*, 7> Args;
  for (Type* Ty : ParamTypes) {
    Args.push_back(createTestValue(Ty));
  }
  
  CallInst* IsendCall = createMPICall("MPI_Isend", IsendFT, Args);
  
  // Create call site
  CallSite Site;
  Site.FunctionName = "MPI_Isend";
  Site.Type = MPIFunctionType::PointToPoint;
  Site.Inst = IsendCall;
  
  // Extract metadata
  MPICallMetadata Metadata = MetaExtractor->extractMetadata(Site);
  
  // Marshal parameters
  std::vector<MarshaledParameter> Marshaled = Marshaler->marshalParameters(Site, Metadata);
  
  // Verify marshaling results
  EXPECT_EQ(Marshaled.size(), 7u);
  
  // Request parameter is output
  EXPECT_EQ(Marshaled[6].Role, ParameterRole::Request);
  EXPECT_FALSE(Marshaled[6].IsInput);
  EXPECT_TRUE(Marshaled[6].IsOutput);
}

TEST_F(ParameterMarshalingTest, ComplexParameterTypes) {
  // Test marshaling for functions with complex parameter types
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  Type* DoubleTy = Type::getDoubleTy(*Context);
  
  // Create a custom struct type
  SmallVector<Type*, 3> StructElements = {IntTy, DoubleTy, PtrTy};
  StructType* CustomStruct = StructType::create(*Context, StructElements, "CustomStruct");
  Type* CustomStructPtrTy = CustomStruct->getPointerTo();
  
  // Create function with complex parameters
  SmallVector<Type*, 4> ParamTypes = {CustomStructPtrTy, IntTy, DoubleTy, PtrTy};
  FunctionType* ComplexFT = FunctionType::get(IntTy, ParamTypes, false);
  
  SmallVector<Value*, 4> Args;
  for (Type* Ty : ParamTypes) {
    Args.push_back(createTestValue(Ty));
  }
  
  CallInst* ComplexCall = createMPICall("MPI_Complex", ComplexFT, Args);
  
  // Create call site
  CallSite Site;
  Site.FunctionName = "MPI_Complex";
  Site.Type = MPIFunctionType::PointToPoint;
  Site.Inst = ComplexCall;
  
  // Extract metadata
  MPICallMetadata Metadata = MetaExtractor->extractMetadata(Site);
  
  // Marshal parameters
  std::vector<MarshaledParameter> Marshaled = Marshaler->marshalParameters(Site, Metadata);
  
  // Verify marshaling handles complex types
  EXPECT_EQ(Marshaled.size(), 4u);
  
  // Check struct parameter marshaling
  EXPECT_EQ(Marshaled[0].ParamType, CustomStructPtrTy);
  EXPECT_EQ(Marshaled[0].MarshaledSize, 8u); // Pointer size
  
  // Check double parameter marshaling
  EXPECT_EQ(Marshaled[2].ParamType, DoubleTy);
  EXPECT_EQ(Marshaled[2].MarshaledSize, 8u); // Double size
}

TEST_F(ParameterMarshalingTest, VariadicParameterMarshaling) {
  // Test marshaling for variadic functions (if any exist in MPI)
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  // Create variadic function type
  SmallVector<Type*, 2> FixedParamTypes = {PtrTy, IntTy};
  FunctionType* VariadicFT = FunctionType::get(IntTy, FixedParamTypes, true);
  
  // Create call with extra arguments
  SmallVector<Value*, 4> Args;
  Args.push_back(createTestValue(PtrTy));
  Args.push_back(createTestValue(IntTy));
  Args.push_back(createTestValue(IntTy)); // Extra arg 1
  Args.push_back(createTestValue(PtrTy)); // Extra arg 2
  
  CallInst* VariadicCall = createMPICall("MPI_Variadic", VariadicFT, Args);
  
  // Create call site
  CallSite Site;
  Site.FunctionName = "MPI_Variadic";
  Site.Type = MPIFunctionType::PointToPoint;
  Site.Inst = VariadicCall;
  
  // Extract metadata
  MPICallMetadata Metadata = MetaExtractor->extractMetadata(Site);
  
  // Marshal parameters
  std::vector<MarshaledParameter> Marshaled = Marshaler->marshalParameters(Site, Metadata);
  
  // Verify marshaling handles variadic parameters
  EXPECT_EQ(Marshaled.size(), 4u);
  
  // All parameters should be marshaled
  for (const auto& MP : Marshaled) {
    EXPECT_GT(MP.MarshaledSize, 0u);
    EXPECT_NE(MP.ParamType, nullptr);
  }
}

TEST_F(ParameterMarshalingTest, MarshalingSizeCalculation) {
  // Test marshaling size calculation for different types
  Type* Int8Ty = Type::getInt8Ty(*Context);
  Type* Int16Ty = Type::getInt16Ty(*Context);
  Type* Int32Ty = Type::getInt32Ty(*Context);
  Type* Int64Ty = Type::getInt64Ty(*Context);
  Type* FloatTy = Type::getFloatTy(*Context);
  Type* DoubleTy = Type::getDoubleTy(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  // Test basic type sizes
  EXPECT_EQ(Marshaler->calculateMarshaledSize(Int8Ty), 1u);
  EXPECT_EQ(Marshaler->calculateMarshaledSize(Int16Ty), 2u);
  EXPECT_EQ(Marshaler->calculateMarshaledSize(Int32Ty), 4u);
  EXPECT_EQ(Marshaler->calculateMarshaledSize(Int64Ty), 8u);
  EXPECT_EQ(Marshaler->calculateMarshaledSize(FloatTy), 4u);
  EXPECT_EQ(Marshaler->calculateMarshaledSize(DoubleTy), 8u);
  EXPECT_EQ(Marshaler->calculateMarshaledSize(PtrTy), 8u);
  
  // Test struct type size
  SmallVector<Type*, 3> StructElements = {Int32Ty, DoubleTy, PtrTy};
  StructType* TestStruct = StructType::create(*Context, StructElements, "TestStruct");
  EXPECT_EQ(Marshaler->calculateMarshaledSize(TestStruct), 20u); // 4 + 8 + 8
}

TEST_F(ParameterMarshalingTest, UnmarshalingValidation) {
  // Test unmarshaling validation for different parameter configurations
  
  Type* IntTy = Type::getInt32Ty(*Context);
  Type* PtrTy = Type::getInt8PtrTy(*Context);
  
  // Test valid output parameter (pointer type)
  MarshaledParameter ValidOutput;
  ValidOutput.ParamType = PtrTy;
  ValidOutput.IsOutput = true;
  ValidOutput.Role = ParameterRole::Buffer;
  ValidOutput.Buffer.ElementType = IntTy;
  ValidOutput.Buffer.Count = 10;
  
  EXPECT_TRUE(Marshaler->canUnmarshalParameter(ValidOutput));
  
  // Test invalid output parameter (non-pointer type)
  MarshaledParameter InvalidOutput;
  InvalidOutput.ParamType = IntTy;
  InvalidOutput.IsOutput = true;
  InvalidOutput.Role = ParameterRole::Count;
  
  EXPECT_FALSE(Marshaler->canUnmarshalParameter(InvalidOutput));
  
  // Test input-only parameter (should always be valid)
  MarshaledParameter InputOnly;
  InputOnly.ParamType = IntTy;
  InputOnly.IsInput = true;
  InputOnly.IsOutput = false;
  InputOnly.Role = ParameterRole::Count;
  
  EXPECT_TRUE(Marshaler->canUnmarshalParameter(InputOnly));
}

} // anonymous namespace