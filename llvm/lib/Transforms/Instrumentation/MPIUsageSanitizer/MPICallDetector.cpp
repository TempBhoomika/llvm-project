//===- MPICallDetector.cpp - MPI Call Detection Engine ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the MPICallDetector class which identifies MPI function
// calls within LLVM IR modules.
//
//===----------------------------------------------------------------------===//

#include "MPICallDetector.h"
#include "MPIFunctionDatabase.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include <algorithm>

using namespace llvm;

#define DEBUG_TYPE "mpi-call-detector"

MPICallDetector::MPICallDetector() 
    : FunctionDB(std::make_unique<MPIFunctionDatabase>()),
      ManglingHandler(std::make_unique<NameManglingHandler>()) {
  FunctionDB->initialize();
  LLVM_DEBUG(dbgs() << "Initialized MPI Call Detector\n");
}

MPICallDetector::~MPICallDetector() = default;

void MPICallDetector::initialize(Module& M) {
  CurrentModule = &M;
  LLVM_DEBUG(dbgs() << "MPICallDetector initialized with module: " << M.getName() << "\n");
}

std::vector<CallSite> MPICallDetector::detectMPICalls(Function& F) {
  return detectMPICalls(F, nullptr);
}

std::vector<CallSite> MPICallDetector::detectMPICalls(Function& F, AAResults* AA) {
  std::vector<CallSite> MPICalls;
  
  LLVM_DEBUG(dbgs() << "Detecting MPI calls in function: " << F.getName() << "\n");
  
  // Detect direct calls
  auto DirectCalls = detectDirectCalls(F);
  MPICalls.insert(MPICalls.end(), DirectCalls.begin(), DirectCalls.end());
  
  // Detect indirect calls with or without alias analysis
  std::vector<CallSite> IndirectCalls;
  if (AA) {
    IndirectCalls = detectIndirectCallsWithAA(F, AA);
  } else {
    IndirectCalls = detectIndirectCalls(F);
  }
  MPICalls.insert(MPICalls.end(), IndirectCalls.begin(), IndirectCalls.end());
  
  LLVM_DEBUG(dbgs() << "Detected " << MPICalls.size() << " MPI calls in function " 
                    << F.getName() << " (" << DirectCalls.size() << " direct, " 
                    << IndirectCalls.size() << " indirect)\n");
  
  return MPICalls;
}

std::vector<CallSite> MPICallDetector::detectDirectCalls(Function& F) {
  std::vector<CallSite> DirectCalls;
  
  for (BasicBlock& BB : F) {
    for (Instruction& I : BB) {
      if (auto* CI = dyn_cast<CallInst>(&I)) {
        // Check for direct function calls
        if (Function* CalledFunc = CI->getCalledFunction()) {
          StringRef FuncName = CalledFunc->getName();
          
          // Try multiple name variations to handle different bindings
          std::vector<StringRef> NamesToCheck = {
            FuncName,
            normalizeFunctionName(FuncName)
          };
          
          // Add demangled variants for Fortran functions
          if (ManglingHandler->isFortranMangled(FuncName)) {
            std::string DemangledName = ManglingHandler->demangleFortranName(FuncName);
            NamesToCheck.push_back(StringRef(DemangledName));
          }
          
          // Add demangled variants for C++ functions
          if (FuncName.substr(0, 7) == "_ZN3MPI" || FuncName.substr(0, 8) == "_ZNK3MPI") {
            std::string CXXName = demangledCXXMPIName(FuncName);
            if (!CXXName.empty()) {
              NamesToCheck.push_back(StringRef(CXXName));
              // Also try the C equivalent
              std::string CName = convertCXXNameToC(StringRef(CXXName));
              NamesToCheck.push_back(StringRef(CName));
            }
          }
          
          // Check each name variant
          for (StringRef NameToCheck : NamesToCheck) {
            if (isMPIFunction(NameToCheck)) {
              MPIFunctionType Type = classifyMPIFunction(NameToCheck);
              DirectCalls.emplace_back(CI, NameToCheck, Type, false);
              
              LLVM_DEBUG(dbgs() << "Found direct MPI call: " << NameToCheck 
                                << " (original: " << FuncName << ")\n");
              break; // Found match, no need to check other variants
            }
          }
        }
        // Check for calls through function pointers that might be MPI functions
        else if (Value* CalledValue = CI->getCalledOperand()) {
          // This is an indirect call - we'll handle it in detectIndirectCalls
          // but we can do some basic checks here for obvious cases
          if (auto* GV = dyn_cast<GlobalVariable>(CalledValue)) {
            StringRef VarName = GV->getName();
            if (isMPIFunction(VarName)) {
              MPIFunctionType Type = classifyMPIFunction(VarName);
              DirectCalls.emplace_back(CI, VarName, Type, true);
              
              LLVM_DEBUG(dbgs() << "Found MPI call through global variable: " << VarName << "\n");
            }
          }
        }
      }
      // Also check invoke instructions (for exception handling)
      else if (auto* II = dyn_cast<InvokeInst>(&I)) {
        if (Function* CalledFunc = II->getCalledFunction()) {
          StringRef FuncName = CalledFunc->getName();
          StringRef NormalizedName = normalizeFunctionName(FuncName);
          
          // Also check C++ demangled names for invoke instructions
          std::vector<StringRef> NamesToCheck = { FuncName, NormalizedName };
          
          if (FuncName.substr(0, 7) == "_ZN3MPI" || FuncName.substr(0, 8) == "_ZNK3MPI") {
            std::string CXXName = demangledCXXMPIName(FuncName);
            if (!CXXName.empty()) {
              NamesToCheck.push_back(StringRef(CXXName));
              std::string CName = convertCXXNameToC(StringRef(CXXName));
              NamesToCheck.push_back(StringRef(CName));
            }
          }
          
          for (StringRef NameToCheck : NamesToCheck) {
            if (isMPIFunction(NameToCheck)) {
              MPIFunctionType Type = classifyMPIFunction(NameToCheck);
              DirectCalls.emplace_back(II, NameToCheck, Type, false);
              
              LLVM_DEBUG(dbgs() << "Found direct MPI invoke: " << NameToCheck << "\n");
              break;
            }
          }
        }
      }
    }
  }
  
  return DirectCalls;
}

std::vector<CallSite> MPICallDetector::detectIndirectCalls(Function& F) {
  return detectIndirectCallsWithAA(F, nullptr);
}

std::vector<CallSite> MPICallDetector::detectIndirectCallsWithAA(Function& F, AAResults* AA) {
  std::vector<CallSite> IndirectCalls;
  
  LLVM_DEBUG(dbgs() << "Detecting indirect MPI calls in function: " << F.getName() 
                    << (AA ? " (with alias analysis)" : " (without alias analysis)") << "\n");
  
  // Look for indirect calls that might be MPI functions
  for (BasicBlock& BB : F) {
    for (Instruction& I : BB) {
      if (auto* CI = dyn_cast<CallInst>(&I)) {
        // Skip direct calls (already handled)
        if (CI->getCalledFunction()) {
          continue;
        }
        
        Value* CalledValue = CI->getCalledOperand();
        
        // Analyze the indirect call with or without alias analysis
        bool IsMPICall = false;
        if (AA) {
          IsMPICall = analyzeIndirectCallWithAA(CI, CalledValue, AA);
        } else {
          IsMPICall = analyzeIndirectCall(CI, CalledValue);
        }
        
        if (IsMPICall) {
          // Try to determine the actual function name if possible
          StringRef FunctionName;
          if (AA) {
            FunctionName = inferFunctionNameWithAA(CalledValue, AA);
          } else {
            FunctionName = inferFunctionName(CalledValue);
          }
          
          MPIFunctionType Type = MPIFunctionType::Unknown;
          
          if (!FunctionName.empty()) {
            Type = classifyMPIFunction(FunctionName);
          }
          
          IndirectCalls.emplace_back(CI, FunctionName.empty() ? "unknown_mpi_function" : FunctionName, 
                                    Type, true);
          
          LLVM_DEBUG(dbgs() << "Found potential indirect MPI call: " << FunctionName << "\n");
        }
      }
      // Also check invoke instructions for indirect calls
      else if (auto* II = dyn_cast<InvokeInst>(&I)) {
        if (!II->getCalledFunction()) {
          Value* CalledValue = II->getCalledOperand();
          
          bool IsMPICall = false;
          if (AA) {
            IsMPICall = analyzeIndirectCallWithAA(II, CalledValue, AA);
          } else {
            IsMPICall = analyzeIndirectCall(II, CalledValue);
          }
          
          if (IsMPICall) {
            StringRef FunctionName;
            if (AA) {
              FunctionName = inferFunctionNameWithAA(CalledValue, AA);
            } else {
              FunctionName = inferFunctionName(CalledValue);
            }
            
            MPIFunctionType Type = MPIFunctionType::Unknown;
            
            if (!FunctionName.empty()) {
              Type = classifyMPIFunction(FunctionName);
            }
            
            IndirectCalls.emplace_back(II, FunctionName.empty() ? "unknown_mpi_function" : FunctionName, 
                                      Type, true);
            
            LLVM_DEBUG(dbgs() << "Found potential indirect MPI invoke: " << FunctionName << "\n");
          }
        }
      }
    }
  }
  
  LLVM_DEBUG(dbgs() << "Found " << IndirectCalls.size() << " indirect MPI calls\n");
  
  return IndirectCalls;
}

bool MPICallDetector::analyzeIndirectCall(CallInst* CI, Value* CalledValue) {
  return analyzeIndirectCallCommon(CI, CalledValue);
}

bool MPICallDetector::analyzeIndirectCall(InvokeInst* II, Value* CalledValue) {
  return analyzeIndirectCallCommon(II, CalledValue);
}

bool MPICallDetector::analyzeIndirectCallWithAA(CallInst* CI, Value* CalledValue, AAResults* AA) {
  return analyzeIndirectCallCommonWithAA(CI, CalledValue, AA);
}

bool MPICallDetector::analyzeIndirectCallWithAA(InvokeInst* II, Value* CalledValue, AAResults* AA) {
  return analyzeIndirectCallCommonWithAA(II, CalledValue, AA);
}

bool MPICallDetector::analyzeIndirectCallCommon(Instruction* CallInstruction, Value* CalledValue) {
  // Basic heuristics for detecting indirect MPI calls
  
  // 1. Check if the called value has an MPI-like name
  if (auto* GV = dyn_cast<GlobalVariable>(CalledValue)) {
    StringRef VarName = GV->getName();
    if (isMPIFunction(VarName) || VarName.contains("mpi") || VarName.contains("MPI")) {
      return true;
    }
  }
  
  // 2. Check if it's a load from a variable with MPI-like name
  if (auto* LI = dyn_cast<LoadInst>(CalledValue)) {
    if (auto* GV = dyn_cast<GlobalVariable>(LI->getPointerOperand())) {
      StringRef VarName = GV->getName();
      if (VarName.contains("mpi") || VarName.contains("MPI")) {
        return true;
      }
    }
    
    // Check if loading from a function pointer that was stored from an MPI function
    if (auto* AI = dyn_cast<AllocaInst>(LI->getPointerOperand())) {
      // Look for stores to this alloca that might be MPI functions
      for (User* U : AI->users()) {
        if (auto* SI = dyn_cast<StoreInst>(U)) {
          if (SI->getPointerOperand() == AI) {
            Value* StoredValue = SI->getValueOperand();
            if (auto* StoredFunc = dyn_cast<Function>(StoredValue)) {
              if (isMPIFunction(StoredFunc->getName())) {
                return true;
              }
            }
          }
        }
      }
    }
  }
  
  // 3. Check for bitcast of MPI functions
  if (auto* BC = dyn_cast<BitCastInst>(CalledValue)) {
    if (auto* Func = dyn_cast<Function>(BC->getOperand(0))) {
      if (isMPIFunction(Func->getName())) {
        return true;
      }
    }
  }
  
  // 4. Check for GEP instructions that might point to MPI function pointers
  if (auto* GEP = dyn_cast<GetElementPtrInst>(CalledValue)) {
    if (auto* GV = dyn_cast<GlobalVariable>(GEP->getPointerOperand())) {
      StringRef VarName = GV->getName();
      if (VarName.contains("mpi") || VarName.contains("MPI") || 
          VarName.contains("vtable") || VarName.contains("dispatch")) {
        return true;
      }
    }
  }
  
  // 5. Check function signature for MPI-like patterns
  FunctionType* FT = nullptr;
  if (auto* CI = dyn_cast<CallInst>(CallInstruction)) {
    FT = CI->getFunctionType();
  } else if (auto* II = dyn_cast<InvokeInst>(CallInstruction)) {
    FT = II->getFunctionType();
  }
  
  if (FT && hasMPILikeSignature(FT)) {
    // Additional check: look at the calling context
    if (hasMPICallingContext(CallInstruction)) {
      return true;
    }
  }
  
  return false;
}

bool MPICallDetector::analyzeIndirectCallCommonWithAA(Instruction* CallInstruction, Value* CalledValue, AAResults* AA) {
  // Enhanced heuristics for detecting indirect MPI calls using alias analysis
  
  // First, try the basic heuristics without alias analysis
  if (analyzeIndirectCallCommon(CallInstruction, CalledValue)) {
    return true;
  }
  
  // If alias analysis is available, use it for more sophisticated analysis
  if (!AA) {
    return false;
  }
  
  // Use alias analysis to resolve function pointers
  std::vector<Function*> PossibleFunctions = resolveFunctionPointer(CalledValue, AA);
  
  // Check if any of the resolved functions are MPI functions
  for (Function* F : PossibleFunctions) {
    if (F && isMPIFunction(F->getName())) {
      LLVM_DEBUG(dbgs() << "Alias analysis resolved indirect call to MPI function: " 
                        << F->getName() << "\n");
      return true;
    }
  }
  
  // Check for function pointers stored in global variables or allocas
  if (auto* LI = dyn_cast<LoadInst>(CalledValue)) {
    Value* LoadedFrom = LI->getPointerOperand();
    
    // Analyze what might have been stored to this location
    std::vector<Function*> StoredFunctions = analyzeStoresTo(LoadedFrom, AA);
    for (Function* F : StoredFunctions) {
      if (F && isMPIFunction(F->getName())) {
        LLVM_DEBUG(dbgs() << "Found MPI function stored to location: " << F->getName() << "\n");
        return true;
      }
    }
  }
  
  // Check for complex GEP patterns that might access MPI function tables
  if (auto* GEP = dyn_cast<GetElementPtrInst>(CalledValue)) {
    Value* BasePtr = GEP->getPointerOperand();
    
    // Check if the base pointer is a global variable with MPI-related name
    if (auto* GV = dyn_cast<GlobalVariable>(BasePtr)) {
      StringRef VarName = GV->getName();
      if (VarName.contains("mpi") || VarName.contains("MPI") || 
          VarName.contains("vtable") || VarName.contains("dispatch") ||
          VarName.contains("function_table") || VarName.contains("func_ptr")) {
        
        // Additional check: see if the global variable's initializer contains MPI functions
        if (GV->hasInitializer()) {
          if (auto* InitArray = dyn_cast<ConstantArray>(GV->getInitializer())) {
            for (unsigned i = 0; i < InitArray->getNumOperands(); ++i) {
              if (auto* Func = dyn_cast<Function>(InitArray->getOperand(i))) {
                if (isMPIFunction(Func->getName())) {
                  LLVM_DEBUG(dbgs() << "Found MPI function in global array: " 
                                    << Func->getName() << "\n");
                  return true;
                }
              }
            }
          }
        }
        return true;
      }
    }
  }
  
  // Check for phi nodes that might merge MPI function pointers
  if (auto* PHI = dyn_cast<PHINode>(CalledValue)) {
    for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
      Value* IncomingValue = PHI->getIncomingValue(i);
      
      // Recursively check each incoming value
      if (auto* IncomingFunc = dyn_cast<Function>(IncomingValue)) {
        if (isMPIFunction(IncomingFunc->getName())) {
          return true;
        }
      }
      
      // Check for loads from MPI function pointers
      if (auto* IncomingLoad = dyn_cast<LoadInst>(IncomingValue)) {
        std::vector<Function*> StoredFunctions = analyzeStoresTo(IncomingLoad->getPointerOperand(), AA);
        for (Function* F : StoredFunctions) {
          if (F && isMPIFunction(F->getName())) {
            return true;
          }
        }
      }
    }
  }
  
  // Check for select instructions that choose between function pointers
  if (auto* Select = dyn_cast<SelectInst>(CalledValue)) {
    Value* TrueValue = Select->getTrueValue();
    Value* FalseValue = Select->getFalseValue();
    
    // Check both possible values
    if (auto* TrueFunc = dyn_cast<Function>(TrueValue)) {
      if (isMPIFunction(TrueFunc->getName())) {
        return true;
      }
    }
    if (auto* FalseFunc = dyn_cast<Function>(FalseValue)) {
      if (isMPIFunction(FalseFunc->getName())) {
        return true;
      }
    }
  }
  
  return false;
}

StringRef MPICallDetector::inferFunctionName(Value* CalledValue) {
  // Try to infer the actual function name from the called value
  
  // Direct global variable reference
  if (auto* GV = dyn_cast<GlobalVariable>(CalledValue)) {
    StringRef VarName = GV->getName();
    if (isMPIFunction(VarName)) {
      return VarName;
    }
  }
  
  // Load from global variable
  if (auto* LI = dyn_cast<LoadInst>(CalledValue)) {
    if (auto* GV = dyn_cast<GlobalVariable>(LI->getPointerOperand())) {
      StringRef VarName = GV->getName();
      // Try to extract MPI function name from variable name
      if (VarName.substr(0, 4) == "mpi_" || VarName.substr(0, 4) == "MPI_") {
        return VarName;
      }
    }
    
    // Check for loads from allocas that store MPI function pointers
    if (auto* AI = dyn_cast<AllocaInst>(LI->getPointerOperand())) {
      for (User* U : AI->users()) {
        if (auto* SI = dyn_cast<StoreInst>(U)) {
          if (SI->getPointerOperand() == AI) {
            if (auto* Func = dyn_cast<Function>(SI->getValueOperand())) {
              if (isMPIFunction(Func->getName())) {
                return Func->getName();
              }
            }
          }
        }
      }
    }
  }
  
  // Bitcast of function
  if (auto* BC = dyn_cast<BitCastInst>(CalledValue)) {
    if (auto* Func = dyn_cast<Function>(BC->getOperand(0))) {
      if (isMPIFunction(Func->getName())) {
        return Func->getName();
      }
    }
  }
  
  return StringRef(); // Empty if we can't determine the name
}

StringRef MPICallDetector::inferFunctionNameWithAA(Value* CalledValue, AAResults* AA) {
  // First try the basic inference without alias analysis
  StringRef BasicName = inferFunctionName(CalledValue);
  if (!BasicName.empty()) {
    return BasicName;
  }
  
  // If alias analysis is available, use it for enhanced resolution
  if (!AA) {
    return StringRef();
  }
  
  // Use alias analysis to resolve function pointers
  std::vector<Function*> PossibleFunctions = resolveFunctionPointer(CalledValue, AA);
  
  // Return the first MPI function we find
  for (Function* F : PossibleFunctions) {
    if (F && isMPIFunction(F->getName())) {
      return F->getName();
    }
  }
  
  // Check for loads from function pointer variables
  if (auto* LI = dyn_cast<LoadInst>(CalledValue)) {
    std::vector<Function*> StoredFunctions = analyzeStoresTo(LI->getPointerOperand(), AA);
    for (Function* F : StoredFunctions) {
      if (F && isMPIFunction(F->getName())) {
        return F->getName();
      }
    }
  }
  
  return StringRef(); // Empty if we can't determine the name
}

std::vector<Function*> MPICallDetector::resolveFunctionPointer(Value* FunctionPtr, AAResults* AA) {
  std::vector<Function*> ResolvedFunctions;
  SmallPtrSet<Value*, 16> Visited;
  
  // Avoid infinite recursion
  if (Visited.count(FunctionPtr)) {
    return ResolvedFunctions;
  }
  Visited.insert(FunctionPtr);
  
  // Direct function reference
  if (auto* F = dyn_cast<Function>(FunctionPtr)) {
    ResolvedFunctions.push_back(F);
    return ResolvedFunctions;
  }
  
  // Bitcast of function
  if (auto* BC = dyn_cast<BitCastInst>(FunctionPtr)) {
    auto SubResults = resolveFunctionPointer(BC->getOperand(0), AA);
    ResolvedFunctions.insert(ResolvedFunctions.end(), SubResults.begin(), SubResults.end());
  }
  
  // Load from memory location
  if (auto* LI = dyn_cast<LoadInst>(FunctionPtr)) {
    Value* LoadedFrom = LI->getPointerOperand();
    
    // Analyze stores to this location
    auto StoredFunctions = analyzeStoresTo(LoadedFrom, AA);
    ResolvedFunctions.insert(ResolvedFunctions.end(), StoredFunctions.begin(), StoredFunctions.end());
  }
  
  // GEP into function table
  if (auto* GEP = dyn_cast<GetElementPtrInst>(FunctionPtr)) {
    Value* BasePtr = GEP->getPointerOperand();
    
    // Check if this is indexing into a global array of function pointers
    if (auto* GV = dyn_cast<GlobalVariable>(BasePtr)) {
      if (GV->hasInitializer()) {
        if (auto* InitArray = dyn_cast<ConstantArray>(GV->getInitializer())) {
          // Try to determine the index if it's constant
          if (GEP->hasAllConstantIndices()) {
            SmallVector<Value*, 4> Indices(GEP->idx_begin(), GEP->idx_end());
            if (Indices.size() >= 2) {
              if (auto* IndexConst = dyn_cast<ConstantInt>(Indices[1])) {
                uint64_t Index = IndexConst->getZExtValue();
                if (Index < InitArray->getNumOperands()) {
                  if (auto* Func = dyn_cast<Function>(InitArray->getOperand(Index))) {
                    ResolvedFunctions.push_back(Func);
                  }
                }
              }
            }
          } else {
            // Non-constant index - add all possible functions from the array
            for (unsigned i = 0; i < InitArray->getNumOperands(); ++i) {
              if (auto* Func = dyn_cast<Function>(InitArray->getOperand(i))) {
                ResolvedFunctions.push_back(Func);
              }
            }
          }
        }
      }
    }
  }
  
  // PHI node - merge results from all incoming values
  if (auto* PHI = dyn_cast<PHINode>(FunctionPtr)) {
    for (unsigned i = 0; i < PHI->getNumIncomingValues(); ++i) {
      Value* IncomingValue = PHI->getIncomingValue(i);
      if (!Visited.count(IncomingValue)) {
        auto SubResults = resolveFunctionPointer(IncomingValue, AA);
        ResolvedFunctions.insert(ResolvedFunctions.end(), SubResults.begin(), SubResults.end());
      }
    }
  }
  
  // Select instruction - check both possible values
  if (auto* Select = dyn_cast<SelectInst>(FunctionPtr)) {
    if (!Visited.count(Select->getTrueValue())) {
      auto TrueResults = resolveFunctionPointer(Select->getTrueValue(), AA);
      ResolvedFunctions.insert(ResolvedFunctions.end(), TrueResults.begin(), TrueResults.end());
    }
    if (!Visited.count(Select->getFalseValue())) {
      auto FalseResults = resolveFunctionPointer(Select->getFalseValue(), AA);
      ResolvedFunctions.insert(ResolvedFunctions.end(), FalseResults.begin(), FalseResults.end());
    }
  }
  
  return ResolvedFunctions;
}

std::vector<Function*> MPICallDetector::analyzeStoresTo(Value* Pointer, AAResults* AA) {
  std::vector<Function*> StoredFunctions;
  
  if (!CurrentModule) {
    return StoredFunctions;
  }
  
  // Create a memory location for the pointer - use a conservative approach
  MemoryLocation Loc(Pointer, LocationSize::beforeOrAfterPointer());
  
  // Scan all instructions in the module for stores that might alias with this location
  for (Function& F : *CurrentModule) {
    for (BasicBlock& BB : F) {
      for (Instruction& I : BB) {
        if (auto* SI = dyn_cast<StoreInst>(&I)) {
          Value* StoredValue = SI->getValueOperand();
          
          // Check if this store might alias with our pointer
          MemoryLocation StoreLoc = MemoryLocation::get(SI);
          if (AA->alias(Loc, StoreLoc) != AliasResult::NoAlias) {
            // This store might affect our pointer
            if (auto* StoredFunc = dyn_cast<Function>(StoredValue)) {
              StoredFunctions.push_back(StoredFunc);
            }
            // Also handle bitcasts of functions
            else if (auto* BC = dyn_cast<BitCastInst>(StoredValue)) {
              if (auto* StoredFunc = dyn_cast<Function>(BC->getOperand(0))) {
                StoredFunctions.push_back(StoredFunc);
              }
            }
          }
        }
      }
    }
  }
  
  // Also check for direct stores to the same pointer (without alias analysis)
  for (User* U : Pointer->users()) {
    if (auto* SI = dyn_cast<StoreInst>(U)) {
      if (SI->getPointerOperand() == Pointer) {
        Value* StoredValue = SI->getValueOperand();
        if (auto* StoredFunc = dyn_cast<Function>(StoredValue)) {
          StoredFunctions.push_back(StoredFunc);
        }
        else if (auto* BC = dyn_cast<BitCastInst>(StoredValue)) {
          if (auto* StoredFunc = dyn_cast<Function>(BC->getOperand(0))) {
            StoredFunctions.push_back(StoredFunc);
          }
        }
      }
    }
  }
  
  return StoredFunctions;
}

bool MPICallDetector::hasMPICallingContext(Instruction* CallInstruction) {
  // Check if the call is in a context that suggests MPI usage
  
  Function* ParentFunc = CallInstruction->getFunction();
  StringRef FuncName = ParentFunc->getName();
  
  // Check if the parent function has MPI-related name
  if (FuncName.contains("mpi") || FuncName.contains("MPI")) {
    return true;
  }
  
  // Check if there are other MPI calls in the same function
  for (BasicBlock& BB : *ParentFunc) {
    for (Instruction& I : BB) {
      if (&I == CallInstruction) continue; // Skip the current instruction
      
      if (auto* CI = dyn_cast<CallInst>(&I)) {
        if (Function* CalledFunc = CI->getCalledFunction()) {
          if (isMPIFunction(CalledFunc->getName())) {
            return true;
          }
        }
      }
    }
  }
  
  // Check for MPI-related constants or global variables used in the function
  for (BasicBlock& BB : *ParentFunc) {
    for (Instruction& I : BB) {
      for (Use& Op : I.operands()) {
        if (auto* GV = dyn_cast<GlobalVariable>(Op.get())) {
          StringRef VarName = GV->getName();
          if (VarName.contains("MPI_") || VarName.contains("mpi_")) {
            return true;
          }
        }
      }
    }
  }
  
  return false;
}

bool MPICallDetector::hasMPILikeSignature(FunctionType* FT) {
  // Check if function signature looks like an MPI function
  // MPI functions typically return int (error code) and have specific parameter patterns
  
  if (!FT->getReturnType()->isIntegerTy()) {
    return false;
  }
  
  unsigned NumParams = FT->getNumParams();
  
  // MPI functions typically have at least 1 parameter
  if (NumParams == 0) {
    return false;
  }
  
  // Look for common MPI parameter patterns
  // Many MPI functions have MPI_Comm as a parameter (typically represented as int or pointer)
  for (unsigned i = 0; i < NumParams; ++i) {
    Type* ParamType = FT->getParamType(i);
    
    // Check for pointer types (buffers, handles)
    if (ParamType->isPointerTy()) {
      return true;
    }
    
    // Check for integer types (counts, ranks, tags)
    if (ParamType->isIntegerTy()) {
      return true;
    }
  }
  
  return false;
}

bool MPICallDetector::isMPIFunction(StringRef FunctionName) {
  // First check the database
  if (FunctionDB->isMPIFunction(FunctionName)) {
    return true;
  }
  
  // Check common MPI function prefixes
  if (FunctionName.substr(0, 4) == "MPI_" || FunctionName.substr(0, 5) == "PMPI_") {
    return true;
  }
  
  // Check lowercase variants
  if (FunctionName.substr(0, 4) == "mpi_" || FunctionName.substr(0, 5) == "pmpi_") {
    return true;
  }
  
  // Check C++ MPI namespace functions
  if (FunctionName.substr(0, 5) == "MPI::" || FunctionName.contains("::")) {
    // Convert to C equivalent and check
    std::string CName = convertCXXNameToC(FunctionName);
    if (FunctionDB->isMPIFunction(StringRef(CName))) {
      return true;
    }
    return FunctionName.substr(0, 5) == "MPI::";
  }
  
  // Check C++ mangled names
  if (FunctionName.substr(0, 7) == "_ZN3MPI" || FunctionName.substr(0, 8) == "_ZNK3MPI") {
    return true;
  }
  
  // Check for Fortran mangled names
  if (ManglingHandler->isFortranMangled(FunctionName)) {
    std::string DemangledName = ManglingHandler->demangleFortranName(FunctionName);
    StringRef DemangledRef(DemangledName);
    if (DemangledRef.substr(0, 4) == "MPI_" || DemangledRef.substr(0, 5) == "PMPI_") {
      return true;
    }
  }
  
  return false;
}

MPIFunctionType MPICallDetector::classifyMPIFunction(StringRef FunctionName) {
  // First try the database
  MPIFunctionType Type = FunctionDB->classifyFunction(FunctionName);
  if (Type != MPIFunctionType::Unknown) {
    return Type;
  }
  
  // Fallback to heuristic classification based on function name
  return classifyByName(FunctionName);
}

MPIFunctionType MPICallDetector::classifyByName(StringRef FunctionName) {
  // Convert to uppercase for consistent matching
  std::string UpperNameStr = FunctionName.upper();
  StringRef UpperName(UpperNameStr);
  
  // Environment functions
  if (UpperName.contains("INIT") || UpperName.contains("FINALIZE") || 
      UpperName.contains("ABORT") || UpperName.contains("VERSION")) {
    return MPIFunctionType::Environment;
  }
  
  // Point-to-point communication
  if (UpperName.contains("SEND") || UpperName.contains("RECV") || 
      UpperName.contains("ISEND") || UpperName.contains("IRECV")) {
    return MPIFunctionType::PointToPoint;
  }
  
  // Collective operations
  if (UpperName.contains("BCAST") || UpperName.contains("REDUCE") || 
      UpperName.contains("GATHER") || UpperName.contains("SCATTER") || 
      UpperName.contains("ALLTOALL") || UpperName.contains("BARRIER")) {
    return MPIFunctionType::Collective;
  }
  
  // Communicator operations
  if (UpperName.contains("COMM_")) {
    return MPIFunctionType::Communicator;
  }
  
  // Request operations
  if (UpperName.contains("WAIT") || UpperName.contains("TEST") || 
      UpperName.contains("REQUEST")) {
    return MPIFunctionType::Request;
  }
  
  // Datatype operations
  if (UpperName.contains("TYPE_")) {
    return MPIFunctionType::Datatype;
  }
  
  // Window operations (RMA)
  if (UpperName.contains("WIN_") || UpperName.contains("GET") || 
      UpperName.contains("PUT") || UpperName.contains("ACCUMULATE")) {
    return MPIFunctionType::Window;
  }
  
  // File I/O operations
  if (UpperName.contains("FILE_")) {
    return MPIFunctionType::File;
  }
  
  // Group operations
  if (UpperName.contains("GROUP_")) {
    return MPIFunctionType::Group;
  }
  
  // Info operations
  if (UpperName.contains("INFO_")) {
    return MPIFunctionType::Info;
  }
  
  // Error handling
  if (UpperName.contains("ERROR") || UpperName.contains("ERRHANDLER")) {
    return MPIFunctionType::Error;
  }
  
  // Topology operations
  if (UpperName.contains("CART_") || UpperName.contains("GRAPH_") || 
      UpperName.contains("TOPO")) {
    return MPIFunctionType::Topology;
  }
  
  return MPIFunctionType::Unknown;
}

StringRef MPICallDetector::normalizeFunctionName(StringRef Name) {
  // Handle Fortran name mangling
  if (ManglingHandler->isFortranMangled(Name)) {
    // Store demangled name in a static string to ensure lifetime
    static std::string DemangledName = ManglingHandler->demangleFortranName(Name);
    return StringRef(DemangledName);
  }
  
  // Handle C++ mangled names (e.g., _ZN3MPI4InitERiRPPc -> MPI::Init)
  if (Name.substr(0, 7) == "_ZN3MPI" || Name.substr(0, 8) == "_ZNK3MPI") {
    // This is a C++ mangled MPI function name
    // Extract the class and method name from the mangled name
    static std::string CXXName = demangledCXXMPIName(Name);
    if (!CXXName.empty()) {
      return StringRef(CXXName);
    }
  }
  
  // Handle C++ namespace syntax (MPI::Function -> MPI_Function for lookup)
  if (Name.contains("::")) {
    static std::string ConvertedName = convertCXXNameToC(Name);
    return StringRef(ConvertedName);
  }
  
  // Handle case variations
  if (Name.substr(0, 4) == "mpi_" || Name.substr(0, 5) == "pmpi_") {
    // Convert to uppercase for consistency
    static std::string UpperName = Name.upper();
    return StringRef(UpperName);
  }
  
  return Name;
}

std::string MPICallDetector::demangledCXXMPIName(StringRef MangledName) {
  // Simple C++ demangling for MPI functions
  // This handles common patterns like _ZN3MPI4InitERiRPPc -> MPI::Init
  
  if (MangledName.substr(0, 7) == "_ZN3MPI") {
    // Extract the method name length and name
    StringRef Remaining = MangledName.substr(7); // Skip "_ZN3MPI"
    
    // Parse the method name length
    size_t MethodLenEnd = 0;
    while (MethodLenEnd < Remaining.size() && std::isdigit(Remaining[MethodLenEnd])) {
      MethodLenEnd++;
    }
    
    if (MethodLenEnd > 0) {
      int MethodLen = std::stoi(Remaining.substr(0, MethodLenEnd).str());
      if (MethodLen > 0 && MethodLenEnd + MethodLen <= Remaining.size()) {
        StringRef MethodName = Remaining.substr(MethodLenEnd, MethodLen);
        return "MPI::" + MethodName.str();
      }
    }
  } else if (MangledName.substr(0, 8) == "_ZNK3MPI") {
    // Const member function
    StringRef Remaining = MangledName.substr(8); // Skip "_ZNK3MPI"
    
    size_t MethodLenEnd = 0;
    while (MethodLenEnd < Remaining.size() && std::isdigit(Remaining[MethodLenEnd])) {
      MethodLenEnd++;
    }
    
    if (MethodLenEnd > 0) {
      int MethodLen = std::stoi(Remaining.substr(0, MethodLenEnd).str());
      if (MethodLen > 0 && MethodLenEnd + MethodLen <= Remaining.size()) {
        StringRef MethodName = Remaining.substr(MethodLenEnd, MethodLen);
        return "MPI::" + MethodName.str();
      }
    }
  }
  
  // Handle nested class methods like MPI::Comm::Send
  if (MangledName.contains("4Comm") || MangledName.contains("8Datatype") || 
      MangledName.contains("7Request") || MangledName.contains("5Group") ||
      MangledName.contains("3Win") || MangledName.contains("4Info")) {
    
    // Extract class and method names for common MPI classes
    if (MangledName.contains("4Comm")) {
      return extractCXXMethodName(MangledName, "Comm");
    } else if (MangledName.contains("8Datatype")) {
      return extractCXXMethodName(MangledName, "Datatype");
    } else if (MangledName.contains("7Request")) {
      return extractCXXMethodName(MangledName, "Request");
    } else if (MangledName.contains("5Group")) {
      return extractCXXMethodName(MangledName, "Group");
    } else if (MangledName.contains("3Win")) {
      return extractCXXMethodName(MangledName, "Win");
    } else if (MangledName.contains("4Info")) {
      return extractCXXMethodName(MangledName, "Info");
    }
  }
  
  return ""; // Return empty if we can't demangle
}

std::string MPICallDetector::extractCXXMethodName(StringRef MangledName, StringRef ClassName) {
  // Extract method name from mangled C++ MPI class method
  // Pattern: _ZNK3MPI4Comm4SendE... -> MPI::Comm::Send
  
  std::string ClassPattern = std::to_string(ClassName.size()) + ClassName.str();
  size_t ClassPos = MangledName.find(ClassPattern);
  
  if (ClassPos != std::string::npos) {
    StringRef Remaining = MangledName.substr(ClassPos + ClassPattern.size());
    
    // Parse method name length
    size_t MethodLenEnd = 0;
    while (MethodLenEnd < Remaining.size() && std::isdigit(Remaining[MethodLenEnd])) {
      MethodLenEnd++;
    }
    
    if (MethodLenEnd > 0) {
      int MethodLen = std::stoi(Remaining.substr(0, MethodLenEnd).str());
      if (MethodLen > 0 && MethodLenEnd + MethodLen <= Remaining.size()) {
        StringRef MethodName = Remaining.substr(MethodLenEnd, MethodLen);
        return "MPI::" + ClassName.str() + "::" + MethodName.str();
      }
    }
  }
  
  return "";
}

std::string MPICallDetector::convertCXXNameToC(StringRef CXXName) {
  // Convert C++ MPI function names to C equivalents for database lookup
  // MPI::Init -> MPI_Init
  // MPI::Comm::Send -> MPI_Send (simplified)
  
  std::string Result = CXXName.str();
  
  // Replace :: with _
  size_t pos = 0;
  while ((pos = Result.find("::", pos)) != std::string::npos) {
    Result.replace(pos, 2, "_");
    pos += 1;
  }
  
  // Handle nested class methods by simplifying them
  // MPI_Comm_Send -> MPI_Send
  if (Result.substr(0, 9) == "MPI_Comm_") {
    Result = "MPI_" + Result.substr(9);
  } else if (Result.substr(0, 13) == "MPI_Datatype_") {
    Result = "MPI_Type_" + Result.substr(13);
  } else if (Result.substr(0, 12) == "MPI_Request_") {
    Result = "MPI_" + Result.substr(12);
  } else if (Result.substr(0, 10) == "MPI_Group_") {
    Result = "MPI_Group_" + Result.substr(10);
  } else if (Result.substr(0, 8) == "MPI_Win_") {
    Result = "MPI_Win_" + Result.substr(8);
  } else if (Result.substr(0, 9) == "MPI_Info_") {
    Result = "MPI_Info_" + Result.substr(9);
  }
  
  return Result;
}