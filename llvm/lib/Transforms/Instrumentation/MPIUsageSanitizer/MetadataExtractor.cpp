//===- MetadataExtractor.cpp - MPI Call Metadata Extraction -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the MetadataExtractor class which extracts parameter
// information from MPI function calls.
//
//===----------------------------------------------------------------------===//

#include "MetadataExtractor.h"
#include "MPIFunctionDatabase.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/ADT/StringSwitch.h"
#include <memory>

using namespace llvm;

#define DEBUG_TYPE "mpi-metadata-extractor"

/// Implementation of ParameterAnalyzer methods
std::vector<ParameterInfo> ParameterAnalyzer::analyzeCall(const CallBase& Site) {
  std::vector<ParameterInfo> Infos;
  
  if (!Site.CallInst) {
    return Infos;
  }
  
  // Get function signature from database
  const MPIFunctionSignature* Signature = nullptr;
  if (FunctionDB) {
    Signature = FunctionDB->getFunctionSignature(Site.FunctionName);
  }
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      Type* ArgType = Arg->getType();
      
      ParameterRole Role = ParameterRole::Unknown;
      bool IsInput = true;
      bool IsOutput = false;
      
      // Use signature information if available
      if (Signature && i < Signature->Parameters.size()) {
        const auto& ParamInfo = Signature->Parameters[i];
        Role = ParamInfo.Role;
        IsInput = ParamInfo.IsInput;
        IsOutput = ParamInfo.IsOutput;
      } else {
        // Fall back to heuristic analysis
        Role = analyzeParameterByPosition(Site, i, ArgType);
        IsOutput = ArgType->isPointerTy() && (Role == ParameterRole::Status || 
                                             Role == ParameterRole::Request ||
                                             Role == ParameterRole::Buffer);
      }
      
      Infos.emplace_back(Arg, ArgType, Role, IsInput, IsOutput, i);
    }
  }
  
  return Infos;
}

ParameterRole ParameterAnalyzer::analyzeParameterByPosition(const CallBase& Site, unsigned Index, Type* ParamType) {
  // Analyze based on function name patterns and parameter position
  StringRef FuncName = Site.FunctionName;
  
  // Point-to-point communication patterns
  if (FuncName.starts_with("MPI_Send") || FuncName.starts_with("MPI_Isend")) {
    switch (Index) {
      case 0: return ParameterRole::Buffer;
      case 1: return ParameterRole::Count;
      case 2: return ParameterRole::Datatype;
      case 3: return ParameterRole::Destination;
      case 4: return ParameterRole::Tag;
      case 5: return ParameterRole::Communicator;
      case 6: return ParameterRole::Request; // For non-blocking
      default: return ParameterRole::Unknown;
    }
  }
  
  if (FuncName.starts_with("MPI_Recv") || FuncName.starts_with("MPI_Irecv")) {
    switch (Index) {
      case 0: return ParameterRole::Buffer;
      case 1: return ParameterRole::Count;
      case 2: return ParameterRole::Datatype;
      case 3: return ParameterRole::Source;
      case 4: return ParameterRole::Tag;
      case 5: return ParameterRole::Communicator;
      case 6: return (FuncName.starts_with("MPI_Irecv")) ? ParameterRole::Request : ParameterRole::Status;
      default: return ParameterRole::Unknown;
    }
  }
  
  // Collective communication patterns
  if (FuncName.starts_with("MPI_Bcast")) {
    switch (Index) {
      case 0: return ParameterRole::Buffer;
      case 1: return ParameterRole::Count;
      case 2: return ParameterRole::Datatype;
      case 3: return ParameterRole::Root;
      case 4: return ParameterRole::Communicator;
      default: return ParameterRole::Unknown;
    }
  }
  
  if (FuncName.starts_with("MPI_Reduce")) {
    switch (Index) {
      case 0: return ParameterRole::Buffer;      // sendbuf
      case 1: return ParameterRole::Buffer;      // recvbuf
      case 2: return ParameterRole::Count;
      case 3: return ParameterRole::Datatype;
      case 4: return ParameterRole::Operation;
      case 5: return ParameterRole::Root;
      case 6: return ParameterRole::Communicator;
      default: return ParameterRole::Unknown;
    }
  }
  
  // Request management
  if (FuncName.starts_with("MPI_Wait")) {
    switch (Index) {
      case 0: return ParameterRole::Request;
      case 1: return ParameterRole::Status;
      default: return ParameterRole::Unknown;
    }
  }
  
  // Communicator management
  if (FuncName.starts_with("MPI_Comm_")) {
    if (FuncName.contains("create") || FuncName.contains("split")) {
      // Last parameter is usually the new communicator
      if (ParamType->isPointerTy()) {
        return ParameterRole::Communicator;
      }
    }
  }
  
  // Generic heuristics based on type and position
  if (ParamType->isPointerTy()) {
    if (Index == 0) return ParameterRole::Buffer;
    return ParameterRole::Unknown;
  }
  
  if (ParamType->isIntegerTy()) {
    if (Index == 1) return ParameterRole::Count;
    return ParameterRole::Rank;
  }
  
  return ParameterRole::Unknown;
}

/// Implementation of TypeAnalyzer methods
ParameterRole TypeAnalyzer::analyzeType(Type* T, unsigned Index, StringRef FunctionName) {
  if (!T) return ParameterRole::Unknown;
  
  // Pointer type analysis
  if (auto* PtrTy = dyn_cast<PointerType>(T)) {
    return analyzePointerType(PtrTy, Index, FunctionName);
  }
  
  // Integer type analysis
  if (T->isIntegerTy()) {
    return analyzeIntegerType(T, Index, FunctionName);
  }
  
  // Floating point types (rare in MPI)
  if (T->isFloatingPointTy()) {
    return ParameterRole::Unknown;
  }
  
  return ParameterRole::Unknown;
}

bool TypeAnalyzer::isBufferType(Type* T) {
  if (!T) return false;
  
  // Check if it's a pointer to data
  if (T->isPointerTy()) {
    // In opaque pointer mode, we can't inspect the element type
    // So we assume all pointers could be buffers
    return true;
  }
  
  return false;
}

bool TypeAnalyzer::isCommunicatorType(Type* T) {
  if (!T) return false;
  
  // MPI_Comm is typically an integer or pointer type
  return T->isIntegerTy() || T->isPointerTy();
}

bool TypeAnalyzer::isRequestType(Type* T) {
  if (!T) return false;
  
  // MPI_Request is typically a pointer type
  return T->isPointerTy();
}

bool TypeAnalyzer::isStatusType(Type* T) {
  if (!T) return false;
  
  // MPI_Status is typically a pointer type
  // In opaque pointer mode, we can't inspect the element type
  // So we use heuristics based on context
  return T->isPointerTy();
}

ParameterRole TypeAnalyzer::analyzePointerType(PointerType* PtrTy, unsigned Index, StringRef FunctionName) {
  // In opaque pointer mode, we can't inspect the element type
  // Use function name and parameter position heuristics instead
  
  if (FunctionName.contains("Wait") || FunctionName.contains("Test")) {
    return (Index == 1) ? ParameterRole::Status : ParameterRole::Request;
  }
  
  if (FunctionName.contains("Comm_")) {
    return ParameterRole::Communicator;
  }
  
  // First pointer parameter is usually a buffer
  if (Index == 0) {
    return ParameterRole::Buffer;
  }
  
  return ParameterRole::Unknown;
}

ParameterRole TypeAnalyzer::analyzeIntegerType(Type* IntTy, unsigned Index, StringRef FunctionName) {
  // Integer parameters in MPI functions are typically:
  // - Counts (number of elements)
  // - Ranks (process identifiers)
  // - Tags (message tags)
  // - Communicators (in some implementations)
  // - Datatypes (in some implementations)
  
  if (Index == 1 && (FunctionName.contains("Send") || FunctionName.contains("Recv") || 
                     FunctionName.contains("Bcast") || FunctionName.contains("Reduce"))) {
    return ParameterRole::Count;
  }
  
  if (FunctionName.contains("Send") && Index == 3) {
    return ParameterRole::Destination;
  }
  
  if (FunctionName.contains("Recv") && Index == 3) {
    return ParameterRole::Source;
  }
  
  if ((FunctionName.contains("Send") || FunctionName.contains("Recv")) && Index == 4) {
    return ParameterRole::Tag;
  }
  
  if (FunctionName.contains("Bcast") && Index == 3) {
    return ParameterRole::Root;
  }
  
  if (FunctionName.contains("Reduce") && Index == 5) {
    return ParameterRole::Root;
  }
  
  // Last integer parameter is often communicator
  return ParameterRole::Communicator;
}

MetadataExtractor::MetadataExtractor() {
  // Initialize the analyzers
  ParamAnalyzer = std::make_unique<ParameterAnalyzer>(nullptr); // Will be set later when DB is available
  TypeAnalyzer = std::make_unique<class TypeAnalyzer>();
}

MetadataExtractor::~MetadataExtractor() = default;

void MetadataExtractor::setFunctionDatabase(MPIFunctionDatabase* DB) {
  // Recreate the parameter analyzer with the new database
  ParamAnalyzer = std::make_unique<ParameterAnalyzer>(DB);
}

MPICallMetadata MetadataExtractor::extractMetadata(const CallBase& Site) {
  MPICallMetadata Metadata;
  
  if (!Site.CallInst) {
    LLVM_DEBUG(dbgs() << "Invalid call site for metadata extraction\n");
    return Metadata;
  }
  
  // Extract basic information
  Metadata.FunctionName = Site.FunctionName;
  Metadata.FunctionType = Site.Type;
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    Metadata.ReturnType = CI->getType();
    Metadata.CallConv = CI->getCallingConv();
    
    // Extract parameters
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      Metadata.Parameters.push_back(Arg);
    }
  }
  
  // Analyze parameters for roles using the proper analyzer
  if (ParamAnalyzer) {
    Metadata.ParameterInfos = ParamAnalyzer->analyzeCall(Site);
  } else {
    // Fallback to basic analysis
    Metadata.ParameterInfos = analyzeParameters(Site);
  }
  
  // Create named parameter map
  Metadata.NamedParameters = extractNamedParameters(Site, Metadata.ParameterInfos);
  
  LLVM_DEBUG(dbgs() << "Extracted metadata for " << Site.FunctionName 
                    << " with " << Metadata.Parameters.size() << " parameters\n");
  
  return Metadata;
}

Value* MetadataExtractor::extractCommunicator(const CallBase& Site) {
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    // Use parameter analysis to find communicator
    if (ParamAnalyzer) {
      auto ParamInfos = ParamAnalyzer->analyzeCall(Site);
      for (const auto& Info : ParamInfos) {
        if (Info.Role == ParameterRole::Communicator) {
          return Info.ParamValue;
        }
      }
    }
    
    // Enhanced communicator detection with comprehensive MPI function support
    StringRef FuncName = Site.FunctionName;
    
    // Point-to-point communication functions
    if (FuncName.starts_with("MPI_Send") || FuncName.starts_with("MPI_Recv") ||
        FuncName.starts_with("MPI_Isend") || FuncName.starts_with("MPI_Irecv") ||
        FuncName.starts_with("MPI_Rsend") || FuncName.starts_with("MPI_Ssend") ||
        FuncName.starts_with("MPI_Bsend") || FuncName.starts_with("MPI_Sendrecv")) {
      if (CI->arg_size() >= 6) {
        Value* CommArg = CI->getArgOperand(5); // Communicator at index 5
        // Handle MPI_Sendrecv which has communicator at different position
        if (FuncName.starts_with("MPI_Sendrecv") && CI->arg_size() >= 12) {
          CommArg = CI->getArgOperand(11);
        }
        return CommArg;
      }
    }
    
    // Collective communication functions
    if (FuncName.starts_with("MPI_Bcast") || FuncName.starts_with("MPI_Reduce") ||
        FuncName.starts_with("MPI_Allreduce") || FuncName.starts_with("MPI_Scatter") ||
        FuncName.starts_with("MPI_Gather") || FuncName.starts_with("MPI_Allgather") ||
        FuncName.starts_with("MPI_Alltoall") || FuncName.starts_with("MPI_Barrier")) {
      // Most collective operations have communicator as last parameter
      if (CI->arg_size() > 0) {
        return CI->getArgOperand(CI->arg_size() - 1);
      }
    }
    
    // Communicator management functions
    if (FuncName.starts_with("MPI_Comm_")) {
      if (FuncName == "MPI_Comm_create" || FuncName == "MPI_Comm_split" ||
          FuncName == "MPI_Comm_dup" || FuncName == "MPI_Comm_create_group") {
        // Input communicator is typically first parameter
        if (CI->arg_size() >= 1) {
          return CI->getArgOperand(0);
        }
      } else if (FuncName == "MPI_Comm_rank" || FuncName == "MPI_Comm_size" ||
                 FuncName == "MPI_Comm_compare" || FuncName == "MPI_Comm_free") {
        // These functions take communicator as first parameter
        if (CI->arg_size() >= 1) {
          return CI->getArgOperand(0);
        }
      }
    }
    
    // Topology functions
    if (FuncName.starts_with("MPI_Cart_") || FuncName.starts_with("MPI_Graph_") ||
        FuncName.starts_with("MPI_Dist_graph_")) {
      // Topology functions typically have communicator as first parameter
      if (CI->arg_size() >= 1) {
        return CI->getArgOperand(0);
      }
    }
    
    // Handle compile-time constants (MPI_COMM_WORLD, MPI_COMM_SELF)
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      
      // Check for constant communicator values
      if (auto* ConstInt = dyn_cast<ConstantInt>(Arg)) {
        // MPI_COMM_WORLD is typically 0x44000000 or similar implementation-defined value
        // MPI_COMM_SELF is typically 0x44000001 or similar
        // We can't know exact values without MPI implementation, but we can detect
        // if this looks like a communicator based on position and function
        if (TypeAnalyzer && TypeAnalyzer->isCommunicatorType(Arg->getType())) {
          return Arg;
        }
      }
      
      // Check for global variables that might be communicators
      if (auto* GV = dyn_cast<GlobalVariable>(Arg)) {
        StringRef VarName = GV->getName();
        if (VarName.contains("MPI_COMM") || VarName.contains("comm")) {
          return Arg;
        }
      }
      
      // Check for load instructions from communicator variables
      if (auto* LI = dyn_cast<LoadInst>(Arg)) {
        if (auto* GEP = dyn_cast<GetElementPtrInst>(LI->getPointerOperand())) {
          if (auto* GV = dyn_cast<GlobalVariable>(GEP->getPointerOperand())) {
            StringRef VarName = GV->getName();
            if (VarName.contains("MPI_COMM") || VarName.contains("comm")) {
              return Arg;
            }
          }
        }
      }
    }
    
    // Generic fallback: last parameter if it looks like a communicator
    if (CI->arg_size() > 0) {
      Value* LastArg = CI->getArgOperand(CI->arg_size() - 1);
      if (TypeAnalyzer && TypeAnalyzer->isCommunicatorType(LastArg->getType())) {
        return LastArg;
      }
    }
  }
  
  return nullptr;
}

std::vector<Value*> MetadataExtractor::extractBufferInfo(const CallBase& Site) {
  std::vector<Value*> BufferInfo;
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    // Use parameter analysis to find buffer-related parameters
    if (ParamAnalyzer) {
      auto ParamInfos = ParamAnalyzer->analyzeCall(Site);
      
      std::vector<Value*> Buffers;
      std::vector<Value*> Counts;
      std::vector<Value*> Datatypes;
      
      // Collect all buffer-related parameters
      for (const auto& Info : ParamInfos) {
        switch (Info.Role) {
          case ParameterRole::Buffer:
            Buffers.push_back(Info.ParamValue);
            break;
          case ParameterRole::Count:
          case ParameterRole::SendCount:
          case ParameterRole::RecvCount:
            Counts.push_back(Info.ParamValue);
            break;
          case ParameterRole::Datatype:
          case ParameterRole::SendType:
          case ParameterRole::RecvType:
            Datatypes.push_back(Info.ParamValue);
            break;
          default:
            break;
        }
      }
      
      // Handle different MPI function patterns
      StringRef FuncName = Site.FunctionName;
      
      // For functions with multiple buffers (e.g., MPI_Sendrecv, MPI_Reduce)
      if (FuncName.starts_with("MPI_Sendrecv")) {
        // MPI_Sendrecv has sendbuf, recvbuf, sendcount, recvcount, sendtype, recvtype
        if (Buffers.size() >= 2 && Counts.size() >= 2 && Datatypes.size() >= 2) {
          BufferInfo.push_back(Buffers[0]); // sendbuf
          BufferInfo.push_back(Counts[0]);  // sendcount
          BufferInfo.push_back(Datatypes[0]); // sendtype
          BufferInfo.push_back(Buffers[1]); // recvbuf
          BufferInfo.push_back(Counts[1]);  // recvcount
          BufferInfo.push_back(Datatypes[1]); // recvtype
        }
      } else if (FuncName.starts_with("MPI_Reduce") && !FuncName.starts_with("MPI_Allreduce")) {
        // MPI_Reduce has sendbuf and recvbuf
        if (Buffers.size() >= 2 && !Counts.empty() && !Datatypes.empty()) {
          BufferInfo.push_back(Buffers[0]); // sendbuf
          BufferInfo.push_back(Buffers[1]); // recvbuf
          BufferInfo.push_back(Counts[0]);  // count
          BufferInfo.push_back(Datatypes[0]); // datatype
        }
      } else if (FuncName.starts_with("MPI_Alltoall") && FuncName.contains("v")) {
        // Variable count alltoall operations (MPI_Alltoallv, MPI_Alltoallw)
        if (!Buffers.empty()) {
          BufferInfo.push_back(Buffers[0]); // sendbuf
          if (Buffers.size() > 1) {
            BufferInfo.push_back(Buffers[1]); // recvbuf
          }
          // Add count arrays and displacement arrays
          for (size_t i = 0; i < Counts.size(); ++i) {
            BufferInfo.push_back(Counts[i]);
          }
          for (size_t i = 0; i < Datatypes.size(); ++i) {
            BufferInfo.push_back(Datatypes[i]);
          }
        }
      } else {
        // Standard single buffer operations
        if (!Buffers.empty()) BufferInfo.push_back(Buffers[0]);
        if (!Counts.empty()) BufferInfo.push_back(Counts[0]);
        if (!Datatypes.empty()) BufferInfo.push_back(Datatypes[0]);
      }
    } else {
      // Enhanced fallback heuristics for complex MPI patterns
      StringRef FuncName = Site.FunctionName;
      
      // Standard point-to-point and collective operations
      if (FuncName.starts_with("MPI_Send") || FuncName.starts_with("MPI_Recv") ||
          FuncName.starts_with("MPI_Isend") || FuncName.starts_with("MPI_Irecv") ||
          FuncName.starts_with("MPI_Bcast") || FuncName.starts_with("MPI_Allreduce")) {
        if (CI->arg_size() >= 3) {
          BufferInfo.push_back(CI->getArgOperand(0)); // Buffer
          BufferInfo.push_back(CI->getArgOperand(1)); // Count
          BufferInfo.push_back(CI->getArgOperand(2)); // Datatype
        }
      }
      // MPI_Sendrecv pattern
      else if (FuncName.starts_with("MPI_Sendrecv")) {
        if (CI->arg_size() >= 12) {
          BufferInfo.push_back(CI->getArgOperand(0)); // sendbuf
          BufferInfo.push_back(CI->getArgOperand(1)); // sendcount
          BufferInfo.push_back(CI->getArgOperand(2)); // sendtype
          BufferInfo.push_back(CI->getArgOperand(6)); // recvbuf
          BufferInfo.push_back(CI->getArgOperand(7)); // recvcount
          BufferInfo.push_back(CI->getArgOperand(8)); // recvtype
        }
      }
      // MPI_Reduce pattern
      else if (FuncName.starts_with("MPI_Reduce") && !FuncName.starts_with("MPI_Allreduce")) {
        if (CI->arg_size() >= 7) {
          BufferInfo.push_back(CI->getArgOperand(0)); // sendbuf
          BufferInfo.push_back(CI->getArgOperand(1)); // recvbuf
          BufferInfo.push_back(CI->getArgOperand(2)); // count
          BufferInfo.push_back(CI->getArgOperand(3)); // datatype
        }
      }
      // Scatter/Gather operations
      else if (FuncName.starts_with("MPI_Scatter") || FuncName.starts_with("MPI_Gather")) {
        if (CI->arg_size() >= 8) {
          BufferInfo.push_back(CI->getArgOperand(0)); // sendbuf
          BufferInfo.push_back(CI->getArgOperand(1)); // sendcount
          BufferInfo.push_back(CI->getArgOperand(2)); // sendtype
          BufferInfo.push_back(CI->getArgOperand(3)); // recvbuf
          BufferInfo.push_back(CI->getArgOperand(4)); // recvcount
          BufferInfo.push_back(CI->getArgOperand(5)); // recvtype
        }
      }
      // Variable count operations (Scatterv, Gatherv, Alltoallv)
      else if (FuncName.contains("v") && (FuncName.starts_with("MPI_Scatter") ||
                                          FuncName.starts_with("MPI_Gather") ||
                                          FuncName.starts_with("MPI_Alltoall"))) {
        if (CI->arg_size() >= 4) {
          BufferInfo.push_back(CI->getArgOperand(0)); // sendbuf
          BufferInfo.push_back(CI->getArgOperand(1)); // sendcounts array
          BufferInfo.push_back(CI->getArgOperand(2)); // displs array
          BufferInfo.push_back(CI->getArgOperand(3)); // sendtype
          if (CI->arg_size() >= 8) {
            BufferInfo.push_back(CI->getArgOperand(4)); // recvbuf
            BufferInfo.push_back(CI->getArgOperand(5)); // recvcounts array
            BufferInfo.push_back(CI->getArgOperand(6)); // recvdispls array
            BufferInfo.push_back(CI->getArgOperand(7)); // recvtype
          }
        }
      }
    }
    
    // Handle compile-time constants and runtime variables
    for (size_t i = 0; i < BufferInfo.size(); ++i) {
      Value* V = BufferInfo[i];
      
      // Detect compile-time constants
      if (isa<Constant>(V)) {
        LLVM_DEBUG(dbgs() << "Found compile-time constant buffer parameter at index " << i << "\n");
        // For constants, we might want to optimize validation at compile time
        continue;
      }
      
      // Detect runtime variables that might need special handling
      if (auto* LI = dyn_cast<LoadInst>(V)) {
        LLVM_DEBUG(dbgs() << "Found runtime variable buffer parameter at index " << i << "\n");
        // This is a runtime variable - full runtime validation needed
        continue;
      }
      
      // Handle derived datatypes and vector types
      if (auto* GEP = dyn_cast<GetElementPtrInst>(V)) {
        LLVM_DEBUG(dbgs() << "Found GEP-based buffer parameter (derived type) at index " << i << "\n");
        // This might be accessing a field in a struct or array element
        continue;
      }
      
      // Handle function parameters passed through
      if (isa<Argument>(V)) {
        LLVM_DEBUG(dbgs() << "Found function argument buffer parameter at index " << i << "\n");
        // This is a parameter passed from caller - need runtime validation
        continue;
      }
    }
  }
  
  return BufferInfo;
}

Value* MetadataExtractor::extractRequestHandle(const CallBase& Site) {
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    // Use parameter analysis to find request handle
    if (ParamAnalyzer) {
      auto ParamInfos = ParamAnalyzer->analyzeCall(Site);
      for (const auto& Info : ParamInfos) {
        if (Info.Role == ParameterRole::Request) {
          return Info.ParamValue;
        }
      }
    }
    
    // Enhanced request handle detection with comprehensive MPI function support
    StringRef FuncName = Site.FunctionName;
    
    // Non-blocking point-to-point operations
    if (FuncName.starts_with("MPI_Isend") || FuncName.starts_with("MPI_Irecv") ||
        FuncName.starts_with("MPI_Irsend") || FuncName.starts_with("MPI_Issend") ||
        FuncName.starts_with("MPI_Ibsend")) {
      if (CI->arg_size() >= 7) {
        return CI->getArgOperand(6); // Request at index 6
      }
    }
    
    // Non-blocking collective operations
    if (FuncName.starts_with("MPI_Ibcast") || FuncName.starts_with("MPI_Iallreduce") ||
        FuncName.starts_with("MPI_Ireduce") || FuncName.starts_with("MPI_Iscatter") ||
        FuncName.starts_with("MPI_Igather") || FuncName.starts_with("MPI_Iallgather") ||
        FuncName.starts_with("MPI_Ialltoall") || FuncName.starts_with("MPI_Ibarrier")) {
      // Non-blocking collectives typically have request as last parameter
      if (CI->arg_size() > 0) {
        return CI->getArgOperand(CI->arg_size() - 1);
      }
    }
    
    // Request completion operations
    if (FuncName.starts_with("MPI_Wait")) {
      if (CI->arg_size() >= 1) {
        Value* RequestArg = CI->getArgOperand(0);
        
        // Handle MPI_Waitall, MPI_Waitsome, MPI_Waitany
        if (FuncName == "MPI_Waitall" || FuncName == "MPI_Waitsome") {
          // First parameter is count, second is array of requests
          if (CI->arg_size() >= 2) {
            return CI->getArgOperand(1); // Request array
          }
        } else if (FuncName == "MPI_Waitany") {
          // First parameter is count, second is array of requests
          if (CI->arg_size() >= 2) {
            return CI->getArgOperand(1); // Request array
          }
        } else {
          // MPI_Wait - single request
          return RequestArg;
        }
      }
    }
    
    // Test operations (similar to Wait operations)
    if (FuncName.starts_with("MPI_Test")) {
      if (CI->arg_size() >= 1) {
        Value* RequestArg = CI->getArgOperand(0);
        
        // Handle MPI_Testall, MPI_Testsome, MPI_Testany
        if (FuncName == "MPI_Testall" || FuncName == "MPI_Testsome") {
          if (CI->arg_size() >= 2) {
            return CI->getArgOperand(1); // Request array
          }
        } else if (FuncName == "MPI_Testany") {
          if (CI->arg_size() >= 2) {
            return CI->getArgOperand(1); // Request array
          }
        } else {
          // MPI_Test - single request
          return RequestArg;
        }
      }
    }
    
    // Request management operations
    if (FuncName == "MPI_Request_free" || FuncName == "MPI_Cancel" ||
        FuncName == "MPI_Request_get_status") {
      if (CI->arg_size() >= 1) {
        return CI->getArgOperand(0);
      }
    }
    
    // Start operations (MPI_Start, MPI_Startall)
    if (FuncName.starts_with("MPI_Start")) {
      if (FuncName == "MPI_Startall") {
        // First parameter is count, second is array of requests
        if (CI->arg_size() >= 2) {
          return CI->getArgOperand(1); // Request array
        }
      } else {
        // MPI_Start - single request
        if (CI->arg_size() >= 1) {
          return CI->getArgOperand(0);
        }
      }
    }
    
    // Handle compile-time constants (MPI_REQUEST_NULL)
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      
      // Check for constant request values
      if (auto* ConstInt = dyn_cast<ConstantInt>(Arg)) {
        // MPI_REQUEST_NULL is typically 0 or a specific implementation-defined value
        if (TypeAnalyzer && TypeAnalyzer->isRequestType(Arg->getType())) {
          LLVM_DEBUG(dbgs() << "Found compile-time constant request (possibly MPI_REQUEST_NULL)\n");
          return Arg;
        }
      }
      
      // Check for null pointer constants
      if (auto* ConstPtr = dyn_cast<ConstantPointerNull>(Arg)) {
        if (TypeAnalyzer && TypeAnalyzer->isRequestType(Arg->getType())) {
          LLVM_DEBUG(dbgs() << "Found null pointer request constant\n");
          return Arg;
        }
      }
      
      // Check for global variables that might be request handles
      if (auto* GV = dyn_cast<GlobalVariable>(Arg)) {
        StringRef VarName = GV->getName();
        if (VarName.contains("request") || VarName.contains("req") || 
            VarName.contains("MPI_REQUEST")) {
          return Arg;
        }
      }
      
      // Check for load instructions from request variables
      if (auto* LI = dyn_cast<LoadInst>(Arg)) {
        if (auto* AI = dyn_cast<AllocaInst>(LI->getPointerOperand())) {
          // This is loading from a local request variable
          LLVM_DEBUG(dbgs() << "Found load from local request variable\n");
          return Arg;
        }
        
        if (auto* GEP = dyn_cast<GetElementPtrInst>(LI->getPointerOperand())) {
          if (auto* GV = dyn_cast<GlobalVariable>(GEP->getPointerOperand())) {
            StringRef VarName = GV->getName();
            if (VarName.contains("request") || VarName.contains("req")) {
              return Arg;
            }
          }
          
          // Handle arrays of requests
          if (auto* ArrayAlloca = dyn_cast<AllocaInst>(GEP->getPointerOperand())) {
            LLVM_DEBUG(dbgs() << "Found access to request array element\n");
            return Arg;
          }
        }
      }
      
      // Handle arrays of requests (for Waitall, Testall, etc.)
      if (auto* GEP = dyn_cast<GetElementPtrInst>(Arg)) {
        if (auto* ArrayAlloca = dyn_cast<AllocaInst>(GEP->getPointerOperand())) {
          Type* AllocatedType = ArrayAlloca->getAllocatedType();
          if (AllocatedType->isArrayTy()) {
            LLVM_DEBUG(dbgs() << "Found request array parameter\n");
            return Arg;
          }
        }
      }
      
      // Handle function parameters that are requests
      if (auto* ArgInst = dyn_cast<Argument>(Arg)) {
        if (TypeAnalyzer && TypeAnalyzer->isRequestType(Arg->getType())) {
          LLVM_DEBUG(dbgs() << "Found function argument request parameter\n");
          return Arg;
        }
      }
    }
    
    // Generic fallback: look for pointer types that could be requests
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      if (TypeAnalyzer && TypeAnalyzer->isRequestType(Arg->getType())) {
        return Arg;
      }
    }
  }
  
  return nullptr;
}

Value* MetadataExtractor::extractStatus(const CallBase& Site) {
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    // Use parameter analysis to find status object
    if (ParamAnalyzer) {
      auto ParamInfos = ParamAnalyzer->analyzeCall(Site);
      for (const auto& Info : ParamInfos) {
        if (Info.Role == ParameterRole::Status) {
          return Info.ParamValue;
        }
      }
    }
    
    // Fallback heuristics
    StringRef FuncName = Site.FunctionName;
    
    // Wait operations have status as second parameter
    if (FuncName.starts_with("MPI_Wait") || FuncName.starts_with("MPI_Test")) {
      if (CI->arg_size() >= 2) {
        Value* SecondArg = CI->getArgOperand(1);
        if (TypeAnalyzer && TypeAnalyzer->isStatusType(SecondArg->getType())) {
          return SecondArg;
        }
      }
    }
    
    // Recv operations have status as last parameter
    if (FuncName.starts_with("MPI_Recv")) {
      if (CI->arg_size() >= 7) {
        Value* LastArg = CI->getArgOperand(6);
        if (TypeAnalyzer && TypeAnalyzer->isStatusType(LastArg->getType())) {
          return LastArg;
        }
      }
    }
  }
  
  return nullptr;
}

std::vector<ParameterInfo> MetadataExtractor::analyzeParameters(const CallBase& Site) {
  std::vector<ParameterInfo> Infos;
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      Type* ArgType = Arg->getType();
      
      ParameterRole Role = determineParameterRole(Site, i, ArgType);
      
      // Determine input/output nature based on type and role
      bool IsInput = true;  // Most parameters are input
      bool IsOutput = ArgType->isPointerTy(); // Pointers might be output
      
      Infos.emplace_back(Arg, ArgType, Role, IsInput, IsOutput, i);
    }
  }
  
  return Infos;
}

ParameterRole MetadataExtractor::determineParameterRole(const CallBase& Site, 
                                                        unsigned Index, 
                                                        Type* ParamType) {
  // Use TypeAnalyzer if available
  if (TypeAnalyzer) {
    ParameterRole Role = TypeAnalyzer->analyzeType(ParamType, Index, Site.FunctionName);
    if (Role != ParameterRole::Unknown) {
      return Role;
    }
  }
  
  // Fallback to basic heuristics
  StringRef FuncName = Site.FunctionName;
  
  // Function-specific parameter role detection
  if (FuncName.starts_with("MPI_Send") || FuncName.starts_with("MPI_Isend")) {
    switch (Index) {
      case 0: return ParameterRole::Buffer;
      case 1: return ParameterRole::Count;
      case 2: return ParameterRole::Datatype;
      case 3: return ParameterRole::Destination;
      case 4: return ParameterRole::Tag;
      case 5: return ParameterRole::Communicator;
      case 6: return ParameterRole::Request; // For Isend
      default: return ParameterRole::Unknown;
    }
  }
  
  if (FuncName.starts_with("MPI_Recv") || FuncName.starts_with("MPI_Irecv")) {
    switch (Index) {
      case 0: return ParameterRole::Buffer;
      case 1: return ParameterRole::Count;
      case 2: return ParameterRole::Datatype;
      case 3: return ParameterRole::Source;
      case 4: return ParameterRole::Tag;
      case 5: return ParameterRole::Communicator;
      case 6: return (FuncName.starts_with("MPI_Irecv")) ? ParameterRole::Request : ParameterRole::Status;
      default: return ParameterRole::Unknown;
    }
  }
  
  if (FuncName.starts_with("MPI_Bcast")) {
    switch (Index) {
      case 0: return ParameterRole::Buffer;
      case 1: return ParameterRole::Count;
      case 2: return ParameterRole::Datatype;
      case 3: return ParameterRole::Root;
      case 4: return ParameterRole::Communicator;
      default: return ParameterRole::Unknown;
    }
  }
  
  if (FuncName.starts_with("MPI_Wait")) {
    switch (Index) {
      case 0: return ParameterRole::Request;
      case 1: return ParameterRole::Status;
      default: return ParameterRole::Unknown;
    }
  }
  
  // Generic type-based heuristics
  if (ParamType->isPointerTy()) {
    if (Index == 0) {
      return ParameterRole::Buffer; // First pointer often buffer
    }
    return ParameterRole::Unknown;
  }
  
  if (ParamType->isIntegerTy()) {
    if (Index == 1) {
      return ParameterRole::Count; // Second parameter often count
    }
    return ParameterRole::Rank; // Other integers often ranks or tags
  }
  
  return ParameterRole::Unknown;
}

std::map<std::string, Value*> 
MetadataExtractor::extractNamedParameters(const CallBase& Site,
                                          const std::vector<ParameterInfo>& Infos) {
  std::map<std::string, Value*> NamedParams;
  
  // TODO: Map parameter roles to names for easier access
  for (const auto& Info : Infos) {
    switch (Info.Role) {
      case ParameterRole::Buffer:
        NamedParams["buffer"] = Info.ParamValue;
        break;
      case ParameterRole::Count:
        NamedParams["count"] = Info.ParamValue;
        break;
      case ParameterRole::Communicator:
        NamedParams["communicator"] = Info.ParamValue;
        break;
      case ParameterRole::Request:
        NamedParams["request"] = Info.ParamValue;
        break;
      default:
        break;
    }
  }
  
  return NamedParams;
}

//===----------------------------------------------------------------------===//
// Fortran-specific Type Analysis Implementation
//===----------------------------------------------------------------------===//

bool TypeAnalyzer::isFortranArrayDescriptor(Type* T) {
  if (!T || !T->isPointerTy()) return false;
  
  // Check for various Fortran array descriptor patterns
  return isCFIDescriptor(T) || isGFortranArrayDescriptor(T) || 
         isIntelArrayDescriptor(T) || isPGIArrayDescriptor(T);
}

bool TypeAnalyzer::isCFIDescriptor(Type* T) {
  if (!T || !T->isPointerTy()) return false;
  
  // CFI (C Fortran Interoperability) descriptors are standardized in Fortran 2018
  // They typically have a specific structure with base_addr, elem_len, version, etc.
  // In opaque pointer mode, we can't inspect the structure directly,
  // so we rely on naming conventions and context
  return true; // Conservative approach - assume any pointer could be CFI descriptor
}

bool TypeAnalyzer::isGFortranArrayDescriptor(Type* T) {
  if (!T || !T->isPointerTy()) return false;
  
  // GFortran array descriptors have a specific structure
  // In opaque pointer mode, we use heuristics based on usage patterns
  return true; // Conservative approach
}

bool TypeAnalyzer::isIntelArrayDescriptor(Type* T) {
  if (!T || !T->isPointerTy()) return false;
  
  // Intel Fortran array descriptors have their own structure
  return true; // Conservative approach
}

bool TypeAnalyzer::isPGIArrayDescriptor(Type* T) {
  if (!T || !T->isPointerTy()) return false;
  
  // PGI/NVIDIA HPC SDK array descriptors
  return true; // Conservative approach
}

bool TypeAnalyzer::isFortranDerivedType(Type* T) {
  if (!T) return false;
  
  // Fortran derived types can be structures or pointers to structures
  // In opaque pointer mode, we use conservative heuristics
  return T->isPointerTy() || T->isStructTy();
}

bool TypeAnalyzer::isFortranCharacterLength(Type* T, unsigned Index) {
  if (!T || !T->isIntegerTy()) return false;
  
  // Character length parameters are typically integer types
  // They often appear as hidden parameters after character string parameters
  return T->isIntegerTy();
}

bool TypeAnalyzer::isFortranOptionalPresent(Type* T, unsigned Index) {
  if (!T || !T->isIntegerTy()) return false;
  
  // Optional presence flags are typically integer types (0/1 or logical)
  return T->isIntegerTy(1) || T->isIntegerTy(32); // i1 or i32 typically
}

ParameterRole TypeAnalyzer::analyzeFortranParameter(Type* T, unsigned Index, StringRef FunctionName) {
  if (!T) return ParameterRole::Unknown;
  
  // Check for Fortran-specific parameter types
  if (isFortranArrayDescriptor(T)) {
    return ParameterRole::ArrayDescriptor;
  }
  
  if (isFortranDerivedType(T)) {
    return ParameterRole::DerivedType;
  }
  
  if (isFortranCharacterLength(T, Index)) {
    return ParameterRole::CharacterLength;
  }
  
  if (isFortranOptionalPresent(T, Index)) {
    return ParameterRole::OptionalPresent;
  }
  
  // Fall back to standard analysis
  return analyzeType(T, Index, FunctionName);
}

//===----------------------------------------------------------------------===//
// Fortran-specific Metadata Extraction Implementation
//===----------------------------------------------------------------------===//

std::vector<Value*> MetadataExtractor::extractFortranArrayDescriptors(const CallBase& Site) {
  std::vector<Value*> Descriptors;
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    // Look for array descriptor parameters
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      Type* ArgType = Arg->getType();
      
      if (TypeAnalyzer && TypeAnalyzer->isFortranArrayDescriptor(ArgType)) {
        Descriptors.push_back(Arg);
        LLVM_DEBUG(dbgs() << "Found Fortran array descriptor at parameter " << i << "\n");
      }
    }
    
    // Handle specific MPI functions that commonly use array descriptors
    StringRef FuncName = Site.FunctionName;
    
    // Functions with assumed-shape arrays
    if (FuncName.contains("_f90") || FuncName.contains("_f08")) {
      // Fortran 90/2008 interfaces often use array descriptors
      for (unsigned i = 0; i < CI->arg_size(); ++i) {
        Value* Arg = CI->getArgOperand(i);
        if (Arg->getType()->isPointerTy()) {
          // Check if this looks like a buffer parameter that might have a descriptor
          if (i == 0 || (i < 3 && FuncName.contains("Send")) || 
              (i < 3 && FuncName.contains("Recv"))) {
            Descriptors.push_back(Arg);
          }
        }
      }
    }
    
    // Handle multi-dimensional array operations
    if (FuncName.contains("Scatterv") || FuncName.contains("Gatherv") || 
        FuncName.contains("Alltoallv") || FuncName.contains("Alltoallw")) {
      // These functions often involve complex array layouts
      for (unsigned i = 0; i < CI->arg_size(); ++i) {
        Value* Arg = CI->getArgOperand(i);
        if (Arg->getType()->isPointerTy()) {
          // Arrays of counts, displacements, and types may have descriptors
          Descriptors.push_back(Arg);
        }
      }
    }
  }
  
  return Descriptors;
}

std::vector<Value*> MetadataExtractor::extractCharacterLengths(const CallBase& Site) {
  std::vector<Value*> Lengths;
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    StringRef FuncName = Site.FunctionName;
    
    // Look for character string parameters that would have hidden length parameters
    std::vector<unsigned> CharacterParamIndices;
    
    // Identify character string parameters based on function signature
    if (FuncName.contains("_name") || FuncName.contains("_string") || 
        FuncName.contains("_info") || FuncName.contains("_key") || 
        FuncName.contains("_value")) {
      // These functions likely have character string parameters
      for (unsigned i = 0; i < CI->arg_size(); ++i) {
        Value* Arg = CI->getArgOperand(i);
        if (Arg->getType()->isPointerTy()) {
          CharacterParamIndices.push_back(i);
        }
      }
    }
    
    // Look for hidden length parameters
    // In Fortran, character length parameters are typically passed as hidden
    // parameters at the end of the parameter list
    for (unsigned charIdx : CharacterParamIndices) {
      // Look for corresponding length parameter
      // Different compilers place length parameters differently:
      // - gfortran: at the end of parameter list
      // - Intel: immediately after the character parameter
      // - PGI: at the end with specific naming
      
      // Check immediately after character parameter (Intel style)
      if (charIdx + 1 < CI->arg_size()) {
        Value* NextArg = CI->getArgOperand(charIdx + 1);
        if (NextArg->getType()->isIntegerTy()) {
          Lengths.push_back(NextArg);
          LLVM_DEBUG(dbgs() << "Found character length parameter at index " << (charIdx + 1) << "\n");
        }
      }
      
      // Check at end of parameter list (gfortran style)
      if (CI->arg_size() > CharacterParamIndices.size()) {
        unsigned lengthIdx = CI->arg_size() - CharacterParamIndices.size() + 
                           std::distance(CharacterParamIndices.begin(), 
                                       std::find(CharacterParamIndices.begin(), 
                                               CharacterParamIndices.end(), charIdx));
        if (lengthIdx < CI->arg_size()) {
          Value* LengthArg = CI->getArgOperand(lengthIdx);
          if (LengthArg->getType()->isIntegerTy()) {
            Lengths.push_back(LengthArg);
            LLVM_DEBUG(dbgs() << "Found character length parameter at end index " << lengthIdx << "\n");
          }
        }
      }
    }
  }
  
  return Lengths;
}

std::vector<Value*> MetadataExtractor::extractOptionalPresenceFlags(const CallBase& Site) {
  std::vector<Value*> Flags;
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    // Look for optional parameter presence flags
    // These are typically boolean (i1) or integer (i32) parameters
    // that indicate whether optional parameters are present
    
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      Type* ArgType = Arg->getType();
      
      if (TypeAnalyzer && TypeAnalyzer->isFortranOptionalPresent(ArgType, i)) {
        Flags.push_back(Arg);
        LLVM_DEBUG(dbgs() << "Found optional presence flag at parameter " << i << "\n");
      }
    }
    
    // Handle specific MPI functions with optional parameters
    StringRef FuncName = Site.FunctionName;
    
    // MPI_Init may have optional argc/argv in Fortran
    if (FuncName == "MPI_Init" && CI->arg_size() > 1) {
      // Look for presence flags for optional parameters
      for (unsigned i = 1; i < CI->arg_size(); ++i) {
        Value* Arg = CI->getArgOperand(i);
        if (Arg->getType()->isIntegerTy(1)) {
          Flags.push_back(Arg);
        }
      }
    }
    
    // Status parameters in MPI_Wait, MPI_Test, etc. can be optional (MPI_STATUS_IGNORE)
    if (FuncName.contains("Wait") || FuncName.contains("Test")) {
      // Look for status presence flags
      for (unsigned i = 0; i < CI->arg_size(); ++i) {
        Value* Arg = CI->getArgOperand(i);
        if (auto* ConstInt = dyn_cast<ConstantInt>(Arg)) {
          // Check if this might be MPI_STATUS_IGNORE or similar
          if (ConstInt->isZero()) {
            Flags.push_back(Arg);
          }
        }
      }
    }
  }
  
  return Flags;
}

std::vector<Value*> MetadataExtractor::extractDerivedTypeInfo(const CallBase& Site) {
  std::vector<Value*> DerivedTypes;
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    // Look for Fortran derived type parameters
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      Type* ArgType = Arg->getType();
      
      if (TypeAnalyzer && TypeAnalyzer->isFortranDerivedType(ArgType)) {
        DerivedTypes.push_back(Arg);
        LLVM_DEBUG(dbgs() << "Found Fortran derived type at parameter " << i << "\n");
      }
    }
    
    // Handle specific MPI functions that commonly use derived types
    StringRef FuncName = Site.FunctionName;
    
    // MPI datatype creation functions often work with derived types
    if (FuncName.contains("Type_create") || FuncName.contains("Type_struct") ||
        FuncName.contains("Type_indexed") || FuncName.contains("Type_vector")) {
      // These functions create or manipulate derived datatypes
      for (unsigned i = 0; i < CI->arg_size(); ++i) {
        Value* Arg = CI->getArgOperand(i);
        if (Arg->getType()->isPointerTy()) {
          // Structure definitions, displacement arrays, etc.
          DerivedTypes.push_back(Arg);
        }
      }
    }
    
    // User-defined reduction operations may involve derived types
    if (FuncName.contains("Op_create") || FuncName.contains("Reduce") ||
        FuncName.contains("Allreduce") || FuncName.contains("Scan")) {
      // Look for user-defined operation functions or derived type buffers
      for (unsigned i = 0; i < CI->arg_size(); ++i) {
        Value* Arg = CI->getArgOperand(i);
        if (Arg->getType()->isPointerTy()) {
          // Could be a pointer to a derived type or operation function
          DerivedTypes.push_back(Arg);
        }
      }
    }
  }
  
  return DerivedTypes;
}

bool MetadataExtractor::isFortranParameterPassing(const CallBase& Site) {
  if (!Site.CallInst) return false;
  
  StringRef FuncName = Site.FunctionName;
  
  // Check for Fortran-specific function name patterns
  if (FuncName.contains("_f90") || FuncName.contains("_f08") || 
      FuncName.contains("_MOD_") || FuncName.contains("_mod_")) {
    return true;
  }
  
  // Check for Fortran mangling patterns
  if (FuncName.ends_with("_") || FuncName.ends_with("__") ||
      FuncName.upper() == FuncName) {
    return true;
  }
  
  // Check calling convention
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    CallingConv::ID CC = CI->getCallingConv();
    // Fortran often uses specific calling conventions
    if (CC != CallingConv::C) {
      return true;
    }
    
    // Check parameter patterns typical of Fortran
    // Fortran passes everything by reference, so lots of pointer parameters
    unsigned pointerCount = 0;
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      if (CI->getArgOperand(i)->getType()->isPointerTy()) {
        pointerCount++;
      }
    }
    
    // If most parameters are pointers, likely Fortran pass-by-reference
    if (CI->arg_size() > 0 && pointerCount * 2 > CI->arg_size()) {
      return true;
    }
  }
  
  return false;
}

std::vector<Value*> MetadataExtractor::handleFortranPassByReference(const CallBase& Site) {
  std::vector<Value*> References;
  
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    // In Fortran, all parameters are passed by reference
    // This means even scalar values are passed as pointers
    
    for (unsigned i = 0; i < CI->arg_size(); ++i) {
      Value* Arg = CI->getArgOperand(i);
      
      if (Arg->getType()->isPointerTy()) {
        References.push_back(Arg);
        
        // Analyze what this reference points to
        if (auto* LI = dyn_cast<LoadInst>(Arg)) {
          // This is loading a value to pass by reference
          LLVM_DEBUG(dbgs() << "Found Fortran pass-by-reference load at parameter " << i << "\n");
        } else if (auto* AI = dyn_cast<AllocaInst>(Arg)) {
          // Direct reference to local variable
          LLVM_DEBUG(dbgs() << "Found Fortran pass-by-reference alloca at parameter " << i << "\n");
        } else if (auto* GEP = dyn_cast<GetElementPtrInst>(Arg)) {
          // Reference to array element or structure field
          LLVM_DEBUG(dbgs() << "Found Fortran pass-by-reference GEP at parameter " << i << "\n");
        }
      }
    }
    
    // Handle specific Fortran MPI patterns
    StringRef FuncName = Site.FunctionName;
    
    // Fortran MPI functions typically have an additional error code parameter
    if (FuncName.contains("_f90") || FuncName.contains("_f08") || 
        isFortranParameterPassing(Site)) {
      // Last parameter is often the error code (IERROR)
      if (CI->arg_size() > 0) {
        Value* LastArg = CI->getArgOperand(CI->arg_size() - 1);
        if (LastArg->getType()->isPointerTy()) {
          // This is likely the IERROR parameter passed by reference
          References.push_back(LastArg);
          LLVM_DEBUG(dbgs() << "Found Fortran IERROR parameter\n");
        }
      }
    }
  }
  
  return References;
}
