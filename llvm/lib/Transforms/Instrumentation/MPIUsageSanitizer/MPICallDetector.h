//===- MPICallDetector.h - MPI Call Detection Engine ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the MPICallDetector class which identifies MPI function
// calls within LLVM IR modules, supporting both direct and indirect calls
// across multiple language bindings.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPICALLDETECTOR_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPICALLDETECTOR_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include <vector>
#include <memory>

namespace llvm {

class Function;
class Instruction;
class Module;
class AAResults;
class Value;

/// Classification of MPI function types
enum class MPIFunctionType {
  PointToPoint,   // MPI_Send, MPI_Recv, etc.
  Collective,     // MPI_Bcast, MPI_Reduce, etc.
  Communicator,   // MPI_Comm_create, MPI_Comm_split, etc.
  Datatype,       // MPI_Type_create, MPI_Type_commit, etc.
  Request,        // MPI_Wait, MPI_Test, etc.
  Info,           // MPI_Info_create, MPI_Info_set, etc.
  Window,         // MPI_Win_create, MPI_Win_fence, etc.
  File,           // MPI_File_open, MPI_File_read, etc.
  Topology,       // MPI_Cart_create, MPI_Graph_create, etc.
  Environment,    // MPI_Init, MPI_Finalize, etc.
  Group,          // MPI_Group_incl, MPI_Group_excl, etc.
  Process,        // MPI_Comm_spawn, MPI_Comm_connect, etc.
  Attribute,      // MPI_Attr_get, MPI_Attr_put, etc.
  Error,          // MPI_Error_class, MPI_Error_string, etc.
  Profiling,      // MPI_Pcontrol, etc.
  Unknown
};

/// Represents a detected MPI call site
struct CallSite {
  Instruction* CallInst;
  StringRef FunctionName;
  MPIFunctionType Type;
  bool IsIndirect;
  DebugLoc Location;
  
  CallSite(Instruction* CI, StringRef Name, MPIFunctionType T, bool Indirect)
    : CallInst(CI), FunctionName(Name), Type(T), IsIndirect(Indirect) {
    if (CI) Location = CI->getDebugLoc();
  }
};

/// Forward declarations
class MPIFunctionDatabase;
class NameManglingHandler;

/// MPI Call Detection Engine
///
/// Identifies all MPI function calls within LLVM IR modules, supporting
/// both direct and indirect calls across multiple language bindings.
class MPICallDetector {
public:
  MPICallDetector();
  ~MPICallDetector();
  
  /// Detect all MPI calls in a function
  std::vector<CallSite> detectMPICalls(Function& F);
  
  /// Detect all MPI calls in a function with alias analysis support
  std::vector<CallSite> detectMPICalls(Function& F, AAResults* AA);
  
  /// Check if a function name corresponds to an MPI function
  bool isMPIFunction(StringRef FunctionName);
  
  /// Classify an MPI function by type
  MPIFunctionType classifyMPIFunction(StringRef FunctionName);
  
  /// Initialize the detector with a module (for context)
  void initialize(Module& M);

private:
  /// Detect direct MPI function calls
  std::vector<CallSite> detectDirectCalls(Function& F);
  
  /// Detect indirect MPI function calls through function pointers
  std::vector<CallSite> detectIndirectCalls(Function& F);
  
  /// Detect indirect MPI function calls with alias analysis support
  std::vector<CallSite> detectIndirectCallsWithAA(Function& F, AAResults* AA);
  
  /// Analyze an indirect call to determine if it might be an MPI function
  bool analyzeIndirectCall(CallInst* CI, Value* CalledValue);
  bool analyzeIndirectCall(InvokeInst* II, Value* CalledValue);
  
  /// Analyze indirect calls with alias analysis support
  bool analyzeIndirectCallWithAA(CallInst* CI, Value* CalledValue, AAResults* AA);
  bool analyzeIndirectCallWithAA(InvokeInst* II, Value* CalledValue, AAResults* AA);
  
  /// Common analysis logic for both CallInst and InvokeInst
  bool analyzeIndirectCallCommon(Instruction* CallInstruction, Value* CalledValue);
  
  /// Common analysis logic with alias analysis support
  bool analyzeIndirectCallCommonWithAA(Instruction* CallInstruction, Value* CalledValue, AAResults* AA);
  
  /// Try to infer the actual function name from an indirect call
  StringRef inferFunctionName(Value* CalledValue);
  
  /// Try to infer function name with alias analysis support
  StringRef inferFunctionNameWithAA(Value* CalledValue, AAResults* AA);
  
  /// Resolve function pointers using alias analysis
  std::vector<Function*> resolveFunctionPointer(Value* FunctionPtr, AAResults* AA);
  
  /// Analyze stores to function pointers to track MPI function assignments
  std::vector<Function*> analyzeStoresTo(Value* Pointer, AAResults* AA);
  
  /// Check if the calling context suggests MPI usage
  bool hasMPICallingContext(Instruction* CallInstruction);
  
  /// Check if a function type has MPI-like signature characteristics
  bool hasMPILikeSignature(FunctionType* FT);
  
  /// Classify MPI function by name heuristics
  MPIFunctionType classifyByName(StringRef FunctionName);
  
  /// Handle language-specific name variations
  StringRef normalizeFunctionName(StringRef Name);
  
  /// Demangle C++ MPI function names
  std::string demangledCXXMPIName(StringRef MangledName);
  
  /// Extract method name from C++ class method mangling
  std::string extractCXXMethodName(StringRef MangledName, StringRef ClassName);
  
  /// Convert C++ namespace syntax to C function names
  std::string convertCXXNameToC(StringRef CXXName);
  
  std::unique_ptr<MPIFunctionDatabase> FunctionDB;
  std::unique_ptr<NameManglingHandler> ManglingHandler;
  Module* CurrentModule = nullptr;
};

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPICALLDETECTOR_H