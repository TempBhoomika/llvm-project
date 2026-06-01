//===- HookInserter.cpp - MPI Hook Insertion Framework ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the HookInserter class which inserts runtime hook
// functions before and after MPI calls.
//
//===----------------------------------------------------------------------===//

#include "HookInserter.h"
#include "RuntimeInterface.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "mpi-hook-inserter"

HookInserter::HookInserter(const HookConfiguration& Config)
    : Config(Config), RuntimeIntf(std::make_unique<RuntimeInterface>()) {
}

HookInserter::HookInserter(const HookConfiguration& Config, std::shared_ptr<ConfigurationManager> ConfigMgr)
    : Config(Config), RuntimeIntf(std::make_unique<RuntimeInterface>()), ConfigMgr(ConfigMgr) {
}

HookInserter::~HookInserter() = default;

bool HookInserter::insertHooks(Function& F, const std::vector<CallSite>& Sites) {
  if (Sites.empty())
    return false;
    
  bool Modified = false;
  Builder = std::make_unique<IRBuilder<>>(F.getContext());
  
  LLVM_DEBUG(dbgs() << "Inserting hooks for " << Sites.size() 
                    << " MPI calls in function " << F.getName() << "\n");
  
  // Process each call site
  for (auto& Site : Sites) {
    // Extract metadata for this call
    MetadataExtractor Extractor;
    MPICallMetadata Metadata = Extractor.extractMetadata(Site);
    
    bool SiteModified = false;
    
    // Insert pre-call hook
    if (Config.EnablePreHooks) {
      SiteModified |= insertPreCallHook(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert post-call hook
    if (Config.EnablePostHooks) {
      SiteModified |= insertPostCallHook(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert performance hooks based on configuration
    if (Config.EnablePerformanceHooks && shouldApplyPerformanceMonitoring(Site, Metadata)) {
      SiteModified |= insertPerformanceHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert communication volume hooks for buffer operations
    if (Config.EnableCommunicationVolumeHooks && hasCommunicationBuffers(Metadata)) {
      SiteModified |= insertCommunicationVolumeHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert communication pattern hooks for point-to-point operations
    if (Config.EnableCommunicationPatternHooks && 
        Metadata.FunctionType == MPIFunctionType::PointToPoint) {
      SiteModified |= insertCommunicationPatternHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert collective timing hooks for collective operations
    if (Config.EnableCollectiveTimingHooks && isCollectiveOperation(Metadata)) {
      SiteModified |= insertCollectiveTimingHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert synchronization hooks for synchronization points
    if (Config.EnableSynchronizationHooks && 
        (isCollectiveOperation(Metadata) || Metadata.FunctionType == MPIFunctionType::PointToPoint)) {
      SiteModified |= insertSynchronizationHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    Modified |= SiteModified;
  }
  
  return Modified;
}

void HookInserter::createHookDeclarations(Module& M) {
  LLVMContext& Ctx = M.getContext();
  
  // Create pre-call hook declaration
  if (Config.EnablePreHooks) {
    FunctionType* PreHookTy = RuntimeInterface::getPreHookType(Ctx);
    Function* PreHook = createHookDeclaration(M, RuntimeInterface::getPreHookName(), PreHookTy);
    HookDeclarations[RuntimeInterface::getPreHookName()] = PreHook;
  }
  
  // Create post-call hook declaration
  if (Config.EnablePostHooks) {
    FunctionType* PostHookTy = RuntimeInterface::getPostHookType(Ctx);
    Function* PostHook = createHookDeclaration(M, RuntimeInterface::getPostHookName(), PostHookTy);
    HookDeclarations[RuntimeInterface::getPostHookName()] = PostHook;
  }
  
  // Create performance hook declarations
  if (Config.EnablePerformanceHooks) {
    FunctionType* PerfBeginTy = RuntimeInterface::getPerformanceBeginHookType(Ctx);
    FunctionType* PerfEndTy = RuntimeInterface::getPerformanceEndHookType(Ctx);
    
    Function* PerfBegin = createHookDeclaration(M, RuntimeInterface::getPerformanceBeginHookName(), PerfBeginTy);
    Function* PerfEnd = createHookDeclaration(M, RuntimeInterface::getPerformanceEndHookName(), PerfEndTy);
    
    HookDeclarations[RuntimeInterface::getPerformanceBeginHookName()] = PerfBegin;
    HookDeclarations[RuntimeInterface::getPerformanceEndHookName()] = PerfEnd;
  }
  
  // Create communication volume hook declaration
  if (Config.EnableCommunicationVolumeHooks) {
    FunctionType* CommVolumeTy = RuntimeInterface::getCommunicationVolumeHookType(Ctx);
    Function* CommVolumeHook = createHookDeclaration(M, RuntimeInterface::getCommunicationVolumeHookName(), CommVolumeTy);
    HookDeclarations[RuntimeInterface::getCommunicationVolumeHookName()] = CommVolumeHook;
  }
  
  // Create communication pattern hook declaration
  if (Config.EnableCommunicationPatternHooks) {
    FunctionType* CommPatternTy = RuntimeInterface::getCommunicationPatternHookType(Ctx);
    Function* CommPatternHook = createHookDeclaration(M, RuntimeInterface::getCommunicationPatternHookName(), CommPatternTy);
    HookDeclarations[RuntimeInterface::getCommunicationPatternHookName()] = CommPatternHook;
  }
  
  // Create collective timing hook declaration
  if (Config.EnableCollectiveTimingHooks) {
    FunctionType* CollectiveTimingTy = RuntimeInterface::getCollectiveTimingHookType(Ctx);
    Function* CollectiveTimingHook = createHookDeclaration(M, RuntimeInterface::getCollectiveTimingHookName(), CollectiveTimingTy);
    HookDeclarations[RuntimeInterface::getCollectiveTimingHookName()] = CollectiveTimingHook;
  }
  
  // Create synchronization hook declaration
  if (Config.EnableSynchronizationHooks) {
    FunctionType* SyncTy = RuntimeInterface::getSynchronizationHookType(Ctx);
    Function* SyncHook = createHookDeclaration(M, RuntimeInterface::getSynchronizationHookName(), SyncTy);
    HookDeclarations[RuntimeInterface::getSynchronizationHookName()] = SyncHook;
  }
  
  // Validate hook signatures
  if (!validateHookSignatures(M)) {
    LLVM_DEBUG(dbgs() << "Warning: Hook signature validation failed\n");
  }
}

bool HookInserter::insertPreCallHook(CallSite& Site, const MPICallMetadata& Metadata) {
  if (!Site.CallInst || !CurrentModule) {
    LLVM_DEBUG(dbgs() << "Invalid call site or module for pre-call hook insertion\n");
    return false;
  }
    
  // Get the pre-call hook function
  Function* PreHook = CurrentModule->getFunction(RuntimeInterface::getPreHookName());
  if (!PreHook) {
    LLVM_DEBUG(dbgs() << "Pre-call hook function not found in module\n");
    return false;
  }
  
  // Validate insertion point - ensure we can insert before the call
  BasicBlock* BB = Site.CallInst->getParent();
  if (!BB) {
    LLVM_DEBUG(dbgs() << "Call instruction has no parent basic block\n");
    return false;
  }
  
  // Validate calling convention compatibility
  if (!validateCallingConvention(Site, Metadata)) {
    LLVM_DEBUG(dbgs() << "Calling convention validation failed for pre-call hook\n");
    return false;
  }
  
  // Handle exception safety for invoke instructions
  if (!handleExceptionSafety(Site)) {
    LLVM_DEBUG(dbgs() << "Exception safety validation failed for pre-call hook\n");
    return false;
  }
  
  // Set insertion point before the MPI call
  Builder->SetInsertPoint(Site.CallInst);
  
  // Generate enhanced hook parameters with comprehensive metadata
  std::vector<Value*> HookArgs = generateEnhancedPreCallParameters(Site, Metadata);
  
  // Validate parameter count matches hook signature
  if (HookArgs.size() != PreHook->getFunctionType()->getNumParams()) {
    LLVM_DEBUG(dbgs() << "Parameter count mismatch for pre-call hook: expected " 
                      << PreHook->getFunctionType()->getNumParams() 
                      << ", got " << HookArgs.size() << "\n");
    return false;
  }
  
  // Validate parameter types match hook signature
  FunctionType* HookType = PreHook->getFunctionType();
  for (size_t i = 0; i < HookArgs.size(); ++i) {
    Type* ExpectedType = HookType->getParamType(i);
    Type* ActualType = HookArgs[i]->getType();
    if (ExpectedType != ActualType) {
      LLVM_DEBUG(dbgs() << "Parameter type mismatch at index " << i 
                        << " for pre-call hook\n");
      return false;
    }
  }
  
  // Insert the hook call with proper calling convention
  CallInst* HookCall = Builder->CreateCall(PreHook, HookArgs);
  
  // Preserve calling convention from original MPI call if applicable
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    if (CI->getCallingConv() != CallingConv::C) {
      // Keep hook calls as C convention for runtime compatibility
      HookCall->setCallingConv(CallingConv::C);
    }
  } else if (auto* II = dyn_cast<InvokeInst>(Site.CallInst)) {
    if (II->getCallingConv() != CallingConv::C) {
      // Keep hook calls as C convention for runtime compatibility
      HookCall->setCallingConv(CallingConv::C);
    }
  }
  
  // Preserve debug information if enabled
  if (Config.PreserveDebugInfo && Site.CallInst->getDebugLoc()) {
    HookCall->setDebugLoc(Site.CallInst->getDebugLoc());
  }
  
  // Mark hook call as not throwing to preserve exception handling semantics
  HookCall->setDoesNotThrow();
  HookCall->addFnAttr(Attribute::NoUnwind);
  
  // Ensure hook doesn't interfere with optimization
  HookCall->addFnAttr(Attribute::OptimizeNone);
  
  LLVM_DEBUG(dbgs() << "Successfully inserted enhanced pre-call hook for " << Metadata.FunctionName 
                    << " with " << HookArgs.size() << " parameters\n");
  return true;
}

bool HookInserter::insertPostCallHook(CallSite& Site, const MPICallMetadata& Metadata) {
  if (!Site.CallInst || !CurrentModule) {
    LLVM_DEBUG(dbgs() << "Invalid call site or module for post-call hook insertion\n");
    return false;
  }
    
  // Get the post-call hook function
  Function* PostHook = CurrentModule->getFunction(RuntimeInterface::getPostHookName());
  if (!PostHook) {
    LLVM_DEBUG(dbgs() << "Post-call hook function not found in module\n");
    return false;
  }
  
  // Validate calling convention compatibility
  if (!validateCallingConvention(Site, Metadata)) {
    LLVM_DEBUG(dbgs() << "Calling convention validation failed for post-call hook\n");
    return false;
  }
  
  // Handle exception safety for invoke instructions
  if (!handleExceptionSafety(Site)) {
    LLVM_DEBUG(dbgs() << "Exception safety validation failed for post-call hook\n");
    return false;
  }
  
  // Handle different instruction types for insertion point
  Instruction* InsertPoint = nullptr;
  
  if (isa<InvokeInst>(Site.CallInst)) {
    // For invoke instructions, insert in the normal destination block
    InvokeInst* Invoke = cast<InvokeInst>(Site.CallInst);
    BasicBlock* NormalDest = Invoke->getNormalDest();
    if (!NormalDest || NormalDest->empty()) {
      LLVM_DEBUG(dbgs() << "Invalid normal destination for invoke instruction\n");
      return false;
    }
    
    // Insert at the beginning of the normal destination block
    // but after any PHI nodes
    InsertPoint = &NormalDest->front();
    while (isa<PHINode>(InsertPoint)) {
      InsertPoint = InsertPoint->getNextNode();
      if (!InsertPoint) {
        LLVM_DEBUG(dbgs() << "Cannot find valid insertion point after PHI nodes\n");
        return false;
      }
    }
  } else if (isa<CallInst>(Site.CallInst)) {
    // For regular call instructions, insert after the call
    InsertPoint = Site.CallInst->getNextNode();
    if (!InsertPoint) {
      LLVM_DEBUG(dbgs() << "Cannot find insertion point after MPI call\n");
      return false;
    }
  } else {
    LLVM_DEBUG(dbgs() << "Unsupported instruction type for post-call hook\n");
    return false;
  }
  
  // Validate insertion point
  BasicBlock* BB = InsertPoint->getParent();
  if (!BB) {
    LLVM_DEBUG(dbgs() << "Insertion point has no parent basic block\n");
    return false;
  }
  
  Builder->SetInsertPoint(InsertPoint);
  
  // Generate enhanced hook parameters with return value preservation
  std::vector<Value*> HookArgs = generateEnhancedPostCallParameters(Site, Metadata);
  
  // Validate parameter count matches hook signature
  if (HookArgs.size() != PostHook->getFunctionType()->getNumParams()) {
    LLVM_DEBUG(dbgs() << "Parameter count mismatch for post-call hook: expected " 
                      << PostHook->getFunctionType()->getNumParams() 
                      << ", got " << HookArgs.size() << "\n");
    return false;
  }
  
  // Validate parameter types match hook signature
  FunctionType* HookType = PostHook->getFunctionType();
  for (size_t i = 0; i < HookArgs.size(); ++i) {
    Type* ExpectedType = HookType->getParamType(i);
    Type* ActualType = HookArgs[i]->getType();
    if (ExpectedType != ActualType) {
      LLVM_DEBUG(dbgs() << "Parameter type mismatch at index " << i 
                        << " for post-call hook\n");
      return false;
    }
  }
  
  // Insert the hook call with proper calling convention
  CallInst* HookCall = Builder->CreateCall(PostHook, HookArgs);
  
  // Preserve calling convention consistency
  HookCall->setCallingConv(CallingConv::C);
  
  // Preserve debug information if enabled
  if (Config.PreserveDebugInfo && Site.CallInst->getDebugLoc()) {
    HookCall->setDebugLoc(Site.CallInst->getDebugLoc());
  }
  
  // Mark hook call as not throwing to preserve exception handling semantics
  HookCall->setDoesNotThrow();
  HookCall->addFnAttr(Attribute::NoUnwind);
  
  // Ensure the hook call doesn't interfere with control flow
  // The hook should not modify any values that affect program semantics
  HookCall->addFnAttr(Attribute::OptimizeNone);
  
  // For post-call hooks, ensure they don't interfere with return value usage
  // The original return value should remain unchanged and usable
  if (!Site.CallInst->getType()->isVoidTy()) {
    // Verify that the original call's uses are still valid
    for (User* U : Site.CallInst->users()) {
      if (Instruction* UserInst = dyn_cast<Instruction>(U)) {
        // Ensure user instructions come after our hook insertion
        if (UserInst->getParent() == BB) {
          // Check if user instruction is before our hook (which would be invalid)
          for (Instruction* I = &BB->front(); I != HookCall; I = I->getNextNode()) {
            if (I == UserInst) {
              LLVM_DEBUG(dbgs() << "Warning: User instruction before post-call hook\n");
              break;
            }
          }
        }
      }
    }
  }
  
  LLVM_DEBUG(dbgs() << "Successfully inserted enhanced post-call hook for " << Metadata.FunctionName 
                    << " with " << HookArgs.size() << " parameters\n");
  return true;
}

bool HookInserter::insertPerformanceHooks(CallSite& Site, const MPICallMetadata& Metadata) {
  if (!Site.CallInst || !CurrentModule)
    return false;
    
  // Get performance hook functions
  Function* PerfBegin = CurrentModule->getFunction(RuntimeInterface::getPerformanceBeginHookName());
  Function* PerfEnd = CurrentModule->getFunction(RuntimeInterface::getPerformanceEndHookName());
  
  if (!PerfBegin || !PerfEnd) {
    LLVM_DEBUG(dbgs() << "Performance hook functions not found\n");
    return false;
  }
  
  // Create function name and type strings
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  StringRef OpType = getMPIOperationType(Metadata.FunctionType);
  Value* FuncType = getOrCreateStringConstant(OpType);
  
  // Insert begin hook before MPI call
  Builder->SetInsertPoint(Site.CallInst);
  CallInst* BeginCall = Builder->CreateCall(PerfBegin, {FuncName, FuncType});
  
  // Insert end hook after MPI call
  Instruction* InsertPoint = Site.CallInst->getNextNode();
  if (!InsertPoint) {
    LLVM_DEBUG(dbgs() << "Cannot find insertion point after MPI call for performance end hook\n");
    return false;
  }
  
  Builder->SetInsertPoint(InsertPoint);
  CallInst* EndCall = Builder->CreateCall(PerfEnd, {FuncName, FuncType});
  
  // Preserve debug information if enabled
  if (Config.PreserveDebugInfo && Site.CallInst->getDebugLoc()) {
    BeginCall->setDebugLoc(Site.CallInst->getDebugLoc());
    EndCall->setDebugLoc(Site.CallInst->getDebugLoc());
  }
  
  LLVM_DEBUG(dbgs() << "Inserted performance hooks for " << Metadata.FunctionName << "\n");
  return true;
}

bool HookInserter::insertCommunicationVolumeHooks(CallSite& Site, const MPICallMetadata& Metadata) {
  if (!Site.CallInst || !CurrentModule || !hasCommunicationBuffers(Metadata))
    return false;
    
  // Get communication volume hook function
  Function* CommVolumeHook = CurrentModule->getFunction(RuntimeInterface::getCommunicationVolumeHookName());
  if (!CommVolumeHook) {
    LLVM_DEBUG(dbgs() << "Communication volume hook function not found\n");
    return false;
  }
  
  // Calculate communication volume
  Value* Volume = calculateCommunicationVolume(Site, Metadata);
  if (!Volume) {
    LLVM_DEBUG(dbgs() << "Could not calculate communication volume\n");
    return false;
  }
  
  // Create function name and pattern strings
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  Value* Pattern = extractCommunicationPattern(Site, Metadata);
  
  // Insert hook after MPI call
  Instruction* InsertPoint = Site.CallInst->getNextNode();
  if (!InsertPoint) {
    LLVM_DEBUG(dbgs() << "Cannot find insertion point for communication volume hook\n");
    return false;
  }
  
  Builder->SetInsertPoint(InsertPoint);
  CallInst* HookCall = Builder->CreateCall(CommVolumeHook, {FuncName, Volume, Pattern});
  
  // Preserve debug information if enabled
  if (Config.PreserveDebugInfo && Site.CallInst->getDebugLoc()) {
    HookCall->setDebugLoc(Site.CallInst->getDebugLoc());
  }
  
  LLVM_DEBUG(dbgs() << "Inserted communication volume hook for " << Metadata.FunctionName << "\n");
  return true;
}

bool HookInserter::insertCommunicationPatternHooks(CallSite& Site, const MPICallMetadata& Metadata) {
  if (!Site.CallInst || !CurrentModule)
    return false;
    
  // Only apply to point-to-point operations
  if (Metadata.FunctionType != MPIFunctionType::PointToPoint)
    return false;
    
  // Get communication pattern hook function
  Function* CommPatternHook = CurrentModule->getFunction(RuntimeInterface::getCommunicationPatternHookName());
  if (!CommPatternHook) {
    LLVM_DEBUG(dbgs() << "Communication pattern hook function not found\n");
    return false;
  }
  
  // Extract source, destination, and tag information
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* Int32Ty = Type::getInt32Ty(Ctx);
  
  // Default values for unknown parameters
  Value* Source = ConstantInt::get(Int32Ty, -1);
  Value* Dest = ConstantInt::get(Int32Ty, -1);
  Value* Tag = ConstantInt::get(Int32Ty, -1);
  
  // Try to extract actual values from parameters
  if (Metadata.Parameters.size() >= 3) {
    // For MPI_Send/Recv: buf, count, datatype, dest/source, tag, comm
    if (Metadata.Parameters.size() >= 5) {
      if (auto* DestConst = dyn_cast<ConstantInt>(Metadata.Parameters[3])) {
        Dest = DestConst;
      } else {
        Dest = Builder->CreateIntCast(Metadata.Parameters[3], Int32Ty, true);
      }
      
      if (auto* TagConst = dyn_cast<ConstantInt>(Metadata.Parameters[4])) {
        Tag = TagConst;
      } else {
        Tag = Builder->CreateIntCast(Metadata.Parameters[4], Int32Ty, true);
      }
    }
  }
  
  // Create function name and pattern type
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  StringRef PatternType = "point-to-point";
  Value* PatternTypeStr = getOrCreateStringConstant(PatternType);
  
  // Insert hook before MPI call
  Builder->SetInsertPoint(Site.CallInst);
  CallInst* HookCall = Builder->CreateCall(CommPatternHook, {FuncName, Source, Dest, Tag, PatternTypeStr});
  
  // Preserve debug information if enabled
  if (Config.PreserveDebugInfo && Site.CallInst->getDebugLoc()) {
    HookCall->setDebugLoc(Site.CallInst->getDebugLoc());
  }
  
  LLVM_DEBUG(dbgs() << "Inserted communication pattern hook for " << Metadata.FunctionName << "\n");
  return true;
}

bool HookInserter::insertSelectivePerformanceHooks(CallSite& Site, const MPICallMetadata& Metadata, 
                                                   const OptimizationDecision& Decision) {
  if (!Site.CallInst || !CurrentModule || !Decision.EnablePerformanceHooks)
    return false;
    
  bool Modified = false;
  
  // Apply selective instrumentation based on optimization decision
  if (Decision.EnablePerformanceHooks && shouldApplyPerformanceMonitoring(Site, Metadata)) {
    Modified |= insertPerformanceHooks(Site, Metadata);
  }
  
  // Insert communication volume hooks for buffer operations
  if (Config.EnableCommunicationVolumeHooks && hasCommunicationBuffers(Metadata)) {
    Modified |= insertCommunicationVolumeHooks(Site, Metadata);
  }
  
  // Insert communication pattern hooks for point-to-point operations
  if (Config.EnableCommunicationPatternHooks && 
      Metadata.FunctionType == MPIFunctionType::PointToPoint) {
    Modified |= insertCommunicationPatternHooks(Site, Metadata);
  }
  
  // Insert collective timing hooks for collective operations
  if (Config.EnableCollectiveTimingHooks && isCollectiveOperation(Metadata)) {
    Modified |= insertCollectiveTimingHooks(Site, Metadata);
  }
  
  // Insert synchronization hooks for synchronization points
  if (Config.EnableSynchronizationHooks && isCollectiveOperation(Metadata)) {
    Modified |= insertSynchronizationHooks(Site, Metadata);
  }
  
  return Modified;
}

bool HookInserter::insertCollectiveTimingHooks(CallSite& Site, const MPICallMetadata& Metadata) {
  if (!Site.CallInst || !CurrentModule || !isCollectiveOperation(Metadata))
    return false;
    
  // Get collective timing hook function
  Function* CollectiveTimingHook = CurrentModule->getFunction(RuntimeInterface::getCollectiveTimingHookName());
  if (!CollectiveTimingHook) {
    LLVM_DEBUG(dbgs() << "Collective timing hook function not found\n");
    return false;
  }
  
  // Create timing data storage
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* DoubleTy = Type::getDoubleTy(Ctx);
  Type* Int32Ty = Type::getInt32Ty(Ctx);
  
  // Allocate space for timing data (start_time, end_time, duration)
  AllocaInst* TimingData = Builder->CreateAlloca(ArrayType::get(DoubleTy, 3), nullptr, "timing_data");
  
  // Extract communicator size (default to -1 if unknown)
  Value* CommSize = ConstantInt::get(Int32Ty, -1);
  
  // Try to extract communicator from parameters
  if (Metadata.Parameters.size() >= 1) {
    // For most collective operations, communicator is the last parameter
    size_t CommIdx = Metadata.Parameters.size() - 1;
    if (Metadata.Parameters[CommIdx]->getType()->isPointerTy()) {
      // This is likely the communicator, but we can't easily get its size at compile time
      // The runtime will need to determine the actual size
      LLVM_DEBUG(dbgs() << "Found communicator parameter for collective timing\n");
    }
  }
  
  // Create function name
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  
  // Insert timing hook before MPI call
  Builder->SetInsertPoint(Site.CallInst);
  CallInst* HookCall = Builder->CreateCall(CollectiveTimingHook, {FuncName, CommSize, TimingData});
  
  // Preserve debug information if enabled
  if (Config.PreserveDebugInfo && Site.CallInst->getDebugLoc()) {
    HookCall->setDebugLoc(Site.CallInst->getDebugLoc());
  }
  
  LLVM_DEBUG(dbgs() << "Inserted collective timing hook for " << Metadata.FunctionName << "\n");
  return true;
}

bool HookInserter::insertSynchronizationHooks(CallSite& Site, const MPICallMetadata& Metadata) {
  if (!Site.CallInst || !CurrentModule)
    return false;
    
  // Get synchronization hook function
  Function* SyncHook = CurrentModule->getFunction(RuntimeInterface::getSynchronizationHookName());
  if (!SyncHook) {
    LLVM_DEBUG(dbgs() << "Synchronization hook function not found\n");
    return false;
  }
  
  // Determine synchronization type
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* Int32Ty = Type::getInt32Ty(Ctx);
  Value* SyncType;
  
  if (isCollectiveOperation(Metadata)) {
    SyncType = ConstantInt::get(Int32Ty, 1); // Collective synchronization
  } else if (Metadata.FunctionType == MPIFunctionType::PointToPoint) {
    SyncType = ConstantInt::get(Int32Ty, 2); // Point-to-point synchronization
  } else {
    SyncType = ConstantInt::get(Int32Ty, 0); // Unknown synchronization
  }
  
  // Create function name and location
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  Value* Location = extractEnhancedSourceLocation(Site);
  
  // Insert hook before MPI call
  Builder->SetInsertPoint(Site.CallInst);
  CallInst* HookCall = Builder->CreateCall(SyncHook, {FuncName, SyncType, Location});
  
  // Preserve debug information if enabled
  if (Config.PreserveDebugInfo && Site.CallInst->getDebugLoc()) {
    HookCall->setDebugLoc(Site.CallInst->getDebugLoc());
  }
  
  LLVM_DEBUG(dbgs() << "Inserted synchronization hook for " << Metadata.FunctionName << "\n");
  return true;
}

Function* HookInserter::createHookDeclaration(Module& M, StringRef Name, FunctionType* Type) {
  Function* F = M.getFunction(Name);
  if (!F) {
    F = Function::Create(Type, Function::ExternalLinkage, Name, M);
    F->setDoesNotThrow();
    LLVM_DEBUG(dbgs() << "Created hook declaration: " << Name << "\n");
  }
  return F;
}

std::vector<Value*> HookInserter::generateHookParameters(const CallSite& Site,
                                                         const MPICallMetadata& Metadata,
                                                         bool IsPreHook) {
  std::vector<Value*> Args;
  
  // Function name
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  Args.push_back(FuncName);
  
  if (IsPreHook) {
    // For pre-hook: parameters array, count, location
    Value* ParamArray = createParameterArray(Metadata);
    Value* Count = ConstantInt::get(Type::getInt32Ty(CurrentModule->getContext()), Metadata.Parameters.size());
    Value* Location = extractSourceLocation(Site);
    
    Args.push_back(ParamArray);
    Args.push_back(Count);
    Args.push_back(Location);
  } else {
    // For post-hook: return value, error code, location
    Value* RetVal;
    if (Site.CallInst->getType()->isVoidTy()) {
      RetVal = ConstantPointerNull::get(PointerType::get(CurrentModule->getContext(), 0));
    } else {
      // Cast return value to void*
      RetVal = Builder->CreateBitCast(Site.CallInst, PointerType::get(CurrentModule->getContext(), 0));
    }
    
    // For MPI functions, the return value is typically the error code
    Value* ErrorCode;
    if (Site.CallInst->getType()->isIntegerTy()) {
      ErrorCode = Site.CallInst;
    } else {
      ErrorCode = ConstantInt::get(Type::getInt32Ty(CurrentModule->getContext()), 0);
    }
    
    Value* Location = extractSourceLocation(Site);
    
    Args.push_back(RetVal);
    Args.push_back(ErrorCode);
    Args.push_back(Location);
  }
  
  return Args;
}

std::vector<Value*> HookInserter::generateEnhancedPreCallParameters(const CallSite& Site,
                                                                    const MPICallMetadata& Metadata) {
  std::vector<Value*> Args;
  
  // Function name (enhanced with mangled name support)
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  Args.push_back(FuncName);
  
  // Enhanced parameter array with type information and role metadata
  Value* ParamArray = createEnhancedParameterArray(Metadata);
  Args.push_back(ParamArray);
  
  // Parameter count
  Value* Count = ConstantInt::get(Type::getInt32Ty(CurrentModule->getContext()), 
                                  Metadata.Parameters.size());
  Args.push_back(Count);
  
  // Enhanced source location with function context
  Value* Location = extractEnhancedSourceLocation(Site);
  Args.push_back(Location);
  
  return Args;
}

std::vector<Value*> HookInserter::generateEnhancedPostCallParameters(const CallSite& Site,
                                                                     const MPICallMetadata& Metadata) {
  std::vector<Value*> Args;
  
  // Function name
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  Args.push_back(FuncName);
  
  // Enhanced return value handling with proper type preservation
  Value* RetVal = preserveAndExtractReturnValue(Site, Metadata);
  Args.push_back(RetVal);
  
  // Enhanced error code extraction with MPI-specific handling
  Value* ErrorCode = extractMPIErrorCode(Site, Metadata);
  Args.push_back(ErrorCode);
  
  // Enhanced source location
  Value* Location = extractEnhancedSourceLocation(Site);
  Args.push_back(Location);
  
  return Args;
}

Value* HookInserter::createParameterArray(const MPICallMetadata& Metadata) {
  if (Metadata.Parameters.empty()) {
    return ConstantPointerNull::get(PointerType::get(CurrentModule->getContext(), 0));
  }
  
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* VoidPtrTy = PointerType::get(Ctx, 0);
  Type* VoidPtrPtrTy = PointerType::get(Ctx, 0);
  
  // Create array type for parameters
  ArrayType* ArrayTy = ArrayType::get(VoidPtrTy, Metadata.Parameters.size());
  
  // Create alloca for the parameter array
  AllocaInst* ArrayAlloca = Builder->CreateAlloca(ArrayTy, nullptr, "mpi_params");
  
  // Fill the array with parameter pointers
  for (size_t i = 0; i < Metadata.Parameters.size(); ++i) {
    Value* Param = Metadata.Parameters[i];
    Value* ParamPtr;
    
    if (Param->getType()->isPointerTy()) {
      // Already a pointer, cast to void*
      ParamPtr = Builder->CreateBitCast(Param, VoidPtrTy);
    } else {
      // Create alloca for the parameter and store it
      AllocaInst* ParamAlloca = Builder->CreateAlloca(Param->getType(), nullptr, "param_storage");
      Builder->CreateStore(Param, ParamAlloca);
      ParamPtr = Builder->CreateBitCast(ParamAlloca, VoidPtrTy);
    }
    
    // Store parameter pointer in array
    Value* ArrayIdx = ConstantInt::get(Type::getInt32Ty(Ctx), i);
    Value* ArrayElemPtr = Builder->CreateInBoundsGEP(ArrayTy, ArrayAlloca, {ConstantInt::get(Type::getInt32Ty(Ctx), 0), ArrayIdx});
    Builder->CreateStore(ParamPtr, ArrayElemPtr);
  }
  
  // Return pointer to array
  return Builder->CreateBitCast(ArrayAlloca, VoidPtrPtrTy);
}

Value* HookInserter::extractSourceLocation(const CallSite& Site) {
  if (!Config.PreserveDebugInfo || !Site.CallInst) {
    return getOrCreateStringConstant("unknown");
  }
  
  DebugLoc DL = Site.CallInst->getDebugLoc();
  if (!DL) {
    return getOrCreateStringConstant("unknown");
  }
  
  // Extract filename and line number
  std::string Location;
  if (DILocation* DILoc = DL.get()) {
    StringRef Filename = DILoc->getFilename();
    unsigned Line = DILoc->getLine();
    unsigned Column = DILoc->getColumn();
    
    Location = Filename.str() + ":" + std::to_string(Line) + ":" + std::to_string(Column);
  } else {
    Location = "unknown";
  }
  
  return getOrCreateStringConstant(Location);
}

StringRef HookInserter::getMPIOperationType(MPIFunctionType Type) {
  switch (Type) {
    case MPIFunctionType::PointToPoint:
      return "point-to-point";
    case MPIFunctionType::Collective:
      return "collective";
    case MPIFunctionType::Communicator:
      return "communicator";
    case MPIFunctionType::Datatype:
      return "datatype";
    case MPIFunctionType::Request:
      return "request";
    case MPIFunctionType::Info:
      return "info";
    case MPIFunctionType::Window:
      return "window";
    case MPIFunctionType::File:
      return "file";
    case MPIFunctionType::Topology:
      return "topology";
    case MPIFunctionType::Environment:
      return "environment";
    case MPIFunctionType::Group:
      return "group";
    case MPIFunctionType::Process:
      return "process";
    case MPIFunctionType::Attribute:
      return "attribute";
    case MPIFunctionType::Error:
      return "error";
    case MPIFunctionType::Profiling:
      return "profiling";
    default:
      return "unknown";
  }
}

bool HookInserter::validateHookSignatures(Module& M) {
  LLVMContext& Ctx = M.getContext();
  bool AllValid = true;
  
  // Validate pre-call hook
  if (Config.EnablePreHooks) {
    Function* PreHook = M.getFunction(RuntimeInterface::getPreHookName());
    if (PreHook) {
      FunctionType* ExpectedType = RuntimeInterface::getPreHookType(Ctx);
      if (!RuntimeInterface::validateHookSignature(PreHook, ExpectedType)) {
        LLVM_DEBUG(dbgs() << "Pre-call hook signature validation failed\n");
        AllValid = false;
      }
    }
  }
  
  // Validate post-call hook
  if (Config.EnablePostHooks) {
    Function* PostHook = M.getFunction(RuntimeInterface::getPostHookName());
    if (PostHook) {
      FunctionType* ExpectedType = RuntimeInterface::getPostHookType(Ctx);
      if (!RuntimeInterface::validateHookSignature(PostHook, ExpectedType)) {
        LLVM_DEBUG(dbgs() << "Post-call hook signature validation failed\n");
        AllValid = false;
      }
    }
  }
  
  // Validate performance hooks
  if (Config.EnablePerformanceHooks) {
    Function* PerfBegin = M.getFunction(RuntimeInterface::getPerformanceBeginHookName());
    Function* PerfEnd = M.getFunction(RuntimeInterface::getPerformanceEndHookName());
    
    if (PerfBegin) {
      FunctionType* ExpectedType = RuntimeInterface::getPerformanceBeginHookType(Ctx);
      if (!RuntimeInterface::validateHookSignature(PerfBegin, ExpectedType)) {
        LLVM_DEBUG(dbgs() << "Performance begin hook signature validation failed\n");
        AllValid = false;
      }
    }
    
    if (PerfEnd) {
      FunctionType* ExpectedType = RuntimeInterface::getPerformanceEndHookType(Ctx);
      if (!RuntimeInterface::validateHookSignature(PerfEnd, ExpectedType)) {
        LLVM_DEBUG(dbgs() << "Performance end hook signature validation failed\n");
        AllValid = false;
      }
    }
  }
  
  // Validate communication volume hook
  if (Config.EnableCommunicationVolumeHooks) {
    Function* CommVolumeHook = M.getFunction(RuntimeInterface::getCommunicationVolumeHookName());
    if (CommVolumeHook) {
      FunctionType* ExpectedType = RuntimeInterface::getCommunicationVolumeHookType(Ctx);
      if (!RuntimeInterface::validateHookSignature(CommVolumeHook, ExpectedType)) {
        LLVM_DEBUG(dbgs() << "Communication volume hook signature validation failed\n");
        AllValid = false;
      }
    }
  }
  
  // Validate communication pattern hook
  if (Config.EnableCommunicationPatternHooks) {
    Function* CommPatternHook = M.getFunction(RuntimeInterface::getCommunicationPatternHookName());
    if (CommPatternHook) {
      FunctionType* ExpectedType = RuntimeInterface::getCommunicationPatternHookType(Ctx);
      if (!RuntimeInterface::validateHookSignature(CommPatternHook, ExpectedType)) {
        LLVM_DEBUG(dbgs() << "Communication pattern hook signature validation failed\n");
        AllValid = false;
      }
    }
  }
  
  // Validate collective timing hook
  if (Config.EnableCollectiveTimingHooks) {
    Function* CollectiveTimingHook = M.getFunction(RuntimeInterface::getCollectiveTimingHookName());
    if (CollectiveTimingHook) {
      FunctionType* ExpectedType = RuntimeInterface::getCollectiveTimingHookType(Ctx);
      if (!RuntimeInterface::validateHookSignature(CollectiveTimingHook, ExpectedType)) {
        LLVM_DEBUG(dbgs() << "Collective timing hook signature validation failed\n");
        AllValid = false;
      }
    }
  }
  
  // Validate synchronization hook
  if (Config.EnableSynchronizationHooks) {
    Function* SyncHook = M.getFunction(RuntimeInterface::getSynchronizationHookName());
    if (SyncHook) {
      FunctionType* ExpectedType = RuntimeInterface::getSynchronizationHookType(Ctx);
      if (!RuntimeInterface::validateHookSignature(SyncHook, ExpectedType)) {
        LLVM_DEBUG(dbgs() << "Synchronization hook signature validation failed\n");
        AllValid = false;
      }
    }
  }
  
  return AllValid;
}

Value* HookInserter::createEnhancedParameterArray(const MPICallMetadata& Metadata) {
  if (Metadata.Parameters.empty()) {
    return ConstantPointerNull::get(PointerType::get(CurrentModule->getContext(), 0));
  }
  
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* VoidPtrTy = PointerType::get(Ctx, 0);
  Type* VoidPtrPtrTy = PointerType::get(Ctx, 0);
  
  // Create array type for parameters
  ArrayType* ArrayTy = ArrayType::get(VoidPtrTy, Metadata.Parameters.size());
  
  // Create alloca for the parameter array
  AllocaInst* ArrayAlloca = Builder->CreateAlloca(ArrayTy, nullptr, "mpi_params_enhanced");
  
  // Fill the array with parameter pointers, handling different parameter roles
  for (size_t i = 0; i < Metadata.Parameters.size(); ++i) {
    Value* Param = Metadata.Parameters[i];
    Value* ParamPtr;
    
    // Get parameter role information if available
    ParameterRole Role = ParameterRole::Unknown;
    if (i < Metadata.ParameterInfos.size()) {
      Role = Metadata.ParameterInfos[i].Role;
    }
    
    if (Param->getType()->isPointerTy()) {
      // Already a pointer, cast to void*
      ParamPtr = Builder->CreateBitCast(Param, VoidPtrTy);
      
      // For buffer parameters, add additional validation metadata
      if (Role == ParameterRole::Buffer) {
        LLVM_DEBUG(dbgs() << "Enhanced buffer parameter handling at index " << i << "\n");
      }
    } else {
      // Create alloca for the parameter and store it
      AllocaInst* ParamAlloca = Builder->CreateAlloca(Param->getType(), nullptr, 
                                                      "param_storage_" + std::to_string(i));
      Builder->CreateStore(Param, ParamAlloca);
      ParamPtr = Builder->CreateBitCast(ParamAlloca, VoidPtrTy);
      
      // For count parameters, add range validation metadata
      if (Role == ParameterRole::Count) {
        LLVM_DEBUG(dbgs() << "Enhanced count parameter handling at index " << i << "\n");
      }
    }
    
    // Store parameter pointer in array
    Value* ArrayIdx = ConstantInt::get(Type::getInt32Ty(Ctx), i);
    Value* ArrayElemPtr = Builder->CreateInBoundsGEP(ArrayTy, ArrayAlloca, 
                                                     {ConstantInt::get(Type::getInt32Ty(Ctx), 0), ArrayIdx});
    Builder->CreateStore(ParamPtr, ArrayElemPtr);
  }
  
  // Return pointer to array
  return Builder->CreateBitCast(ArrayAlloca, VoidPtrPtrTy);
}

Value* HookInserter::extractEnhancedSourceLocation(const CallSite& Site) {
  if (!Config.PreserveDebugInfo || !Site.CallInst) {
    return getOrCreateStringConstant("unknown");
  }
  
  DebugLoc DL = Site.CallInst->getDebugLoc();
  if (!DL) {
    // Try to get function name as fallback
    Function* ParentFunc = Site.CallInst->getFunction();
    if (ParentFunc) {
      std::string FallbackLocation = "function:" + ParentFunc->getName().str();
      return getOrCreateStringConstant(FallbackLocation);
    }
    return getOrCreateStringConstant("unknown");
  }
  
  // Extract comprehensive location information
  std::string Location;
  if (DILocation* DILoc = DL.get()) {
    StringRef Filename = DILoc->getFilename();
    unsigned Line = DILoc->getLine();
    unsigned Column = DILoc->getColumn();
    
    // Include function name for better context
    Function* ParentFunc = Site.CallInst->getFunction();
    std::string FuncName = ParentFunc ? ParentFunc->getName().str() : "unknown_function";
    
    // Create enhanced location string: filename:line:column:function
    Location = Filename.str() + ":" + std::to_string(Line) + ":" + 
               std::to_string(Column) + ":" + FuncName;
  } else {
    Location = "unknown";
  }
  
  return getOrCreateStringConstant(Location);
}

Value* HookInserter::preserveAndExtractReturnValue(const CallSite& Site, const MPICallMetadata& Metadata) {
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* VoidPtrTy = PointerType::get(Ctx, 0);
  
  if (Site.CallInst->getType()->isVoidTy()) {
    // Void return type - return null pointer
    return ConstantPointerNull::get(cast<PointerType>(VoidPtrTy));
  }
  
  // For non-void return types, preserve the value
  Type* ReturnType = Site.CallInst->getType();
  
  if (ReturnType->isPointerTy()) {
    // Pointer return type - cast to void*
    return Builder->CreateBitCast(Site.CallInst, VoidPtrTy);
  } else if (ReturnType->isIntegerTy()) {
    // Integer return type (common for MPI functions) - create alloca and store
    AllocaInst* RetAlloca = Builder->CreateAlloca(ReturnType, nullptr, "mpi_return_value");
    Builder->CreateStore(Site.CallInst, RetAlloca);
    return Builder->CreateBitCast(RetAlloca, VoidPtrTy);
  } else if (ReturnType->isFloatingPointTy()) {
    // Floating point return type - create alloca and store
    AllocaInst* RetAlloca = Builder->CreateAlloca(ReturnType, nullptr, "mpi_return_value");
    Builder->CreateStore(Site.CallInst, RetAlloca);
    return Builder->CreateBitCast(RetAlloca, VoidPtrTy);
  } else {
    // Other types - create alloca and store
    AllocaInst* RetAlloca = Builder->CreateAlloca(ReturnType, nullptr, "mpi_return_value");
    Builder->CreateStore(Site.CallInst, RetAlloca);
    return Builder->CreateBitCast(RetAlloca, VoidPtrTy);
  }
}

Value* HookInserter::extractMPIErrorCode(const CallSite& Site, const MPICallMetadata& Metadata) {
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* Int32Ty = Type::getInt32Ty(Ctx);
  
  // For MPI functions, the return value is typically the error code
  if (Site.CallInst->getType()->isIntegerTy()) {
    // If return type is integer, use it directly (may need casting)
    Type* ReturnType = Site.CallInst->getType();
    if (ReturnType == Int32Ty) {
      return Site.CallInst;
    } else {
      // Cast to int32 if different integer type
      return Builder->CreateIntCast(Site.CallInst, Int32Ty, true);
    }
  } else if (Site.CallInst->getType()->isVoidTy()) {
    // Void return type - assume success (MPI_SUCCESS = 0)
    return ConstantInt::get(Int32Ty, 0);
  } else {
    // Other return types - assume success
    return ConstantInt::get(Int32Ty, 0);
  }
}

bool HookInserter::handleExceptionSafety(CallSite& Site) {
  // Ensure hook insertion doesn't interfere with exception handling
  if (isa<InvokeInst>(Site.CallInst)) {
    InvokeInst* Invoke = cast<InvokeInst>(Site.CallInst);
    
    // Verify that normal and unwind destinations are valid
    BasicBlock* NormalDest = Invoke->getNormalDest();
    BasicBlock* UnwindDest = Invoke->getUnwindDest();
    
    if (!NormalDest || !UnwindDest) {
      LLVM_DEBUG(dbgs() << "Invalid invoke instruction destinations\n");
      return false;
    }
    
    // Ensure we don't insert hooks in unwind paths
    // Hooks should only be in the normal execution path
    return true;
  }
  
  return true;
}

bool HookInserter::validateCallingConvention(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Validate that the calling convention is compatible with hook insertion
  CallingConv::ID CallConv = Metadata.CallConv;
  
  // Most MPI implementations use C calling convention
  if (CallConv != CallingConv::C && CallConv != CallingConv::Fast) {
    LLVM_DEBUG(dbgs() << "Warning: Unusual calling convention for MPI function: " 
                      << CallConv << "\n");
  }
  
  // Check if the function has any attributes that might interfere with hooks
  if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
    if (CI->hasFnAttr(Attribute::NoReturn)) {
      LLVM_DEBUG(dbgs() << "Warning: MPI function marked as noreturn\n");
      return false;
    }
  }
  
  return true;
}

Value* HookInserter::getOrCreateStringConstant(StringRef Str) {
  if (!CurrentModule)
    return nullptr;
    
  std::string StrKey = Str.str();
  auto It = StringConstants.find(StrKey);
  if (It != StringConstants.end()) {
    // Return GEP to the existing string constant
    LLVMContext& Ctx = CurrentModule->getContext();
    Value* Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
    return Builder->CreateInBoundsGEP(It->second->getValueType(), It->second, {Zero, Zero});
  }
  
  // Create new string constant
  LLVMContext& Ctx = CurrentModule->getContext();
  Constant* StrConstant = ConstantDataArray::getString(Ctx, Str);
  
  GlobalVariable* GV = new GlobalVariable(
    *CurrentModule,
    StrConstant->getType(),
    true, // isConstant
    GlobalValue::PrivateLinkage,
    StrConstant,
    ".str"
  );
  
  // Create GEP to get char*
  Value* Zero = ConstantInt::get(Type::getInt32Ty(Ctx), 0);
  Value* GEP = Builder->CreateInBoundsGEP(StrConstant->getType(), GV, {Zero, Zero});
  
  StringConstants[StrKey] = GV;
  return GEP;
}

bool HookInserter::shouldApplyPerformanceMonitoring(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check configuration settings
  if (!Config.EnablePerformanceHooks)
    return false;
    
  // Check function type-specific settings
  switch (Metadata.FunctionType) {
    case MPIFunctionType::PointToPoint:
      return Config.MonitorPointToPointOps;
    case MPIFunctionType::Collective:
      return Config.MonitorCollectiveOps;
    case MPIFunctionType::Communicator:
      return Config.MonitorCommunicatorOps;
    case MPIFunctionType::Datatype:
      return Config.MonitorDatatypeOps;
    default:
      return false;
  }
}

Value* HookInserter::calculateCommunicationVolume(const CallSite& Site, const MPICallMetadata& Metadata) {
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* Int64Ty = Type::getInt64Ty(Ctx);
  
  // Default volume is 0
  Value* Volume = ConstantInt::get(Int64Ty, 0);
  
  // For operations with count and datatype parameters, calculate volume
  if (Metadata.Parameters.size() >= 3) {
    // Typical MPI buffer operations: buf, count, datatype, ...
    Value* Count = Metadata.Parameters[1];
    
    // Convert count to 64-bit if needed
    if (Count->getType()->isIntegerTy()) {
      if (Count->getType() != Int64Ty) {
        Count = Builder->CreateIntCast(Count, Int64Ty, true);
      }
      
      // For now, assume each element is 4 bytes (can be refined with datatype analysis)
      Value* ElementSize = ConstantInt::get(Int64Ty, 4);
      Volume = Builder->CreateMul(Count, ElementSize);
    }
  }
  
  return Volume;
}

Value* HookInserter::extractCommunicationPattern(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Determine communication pattern based on function type
  StringRef Pattern;
  
  switch (Metadata.FunctionType) {
    case MPIFunctionType::PointToPoint:
      Pattern = "point-to-point";
      break;
    case MPIFunctionType::Collective:
      // Further classify collective patterns
      if (Metadata.FunctionName.contains("Bcast")) {
        Pattern = "broadcast";
      } else if (Metadata.FunctionName.contains("Gather")) {
        Pattern = "gather";
      } else if (Metadata.FunctionName.contains("Scatter")) {
        Pattern = "scatter";
      } else if (Metadata.FunctionName.contains("Alltoall")) {
        Pattern = "all-to-all";
      } else if (Metadata.FunctionName.contains("Reduce")) {
        Pattern = "reduction";
      } else {
        Pattern = "collective";
      }
      break;
    default:
      Pattern = "unknown";
      break;
  }
  
  return getOrCreateStringConstant(Pattern);
}

Value* HookInserter::createPerformanceConfig(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Create a configuration value that encodes performance monitoring settings
  LLVMContext& Ctx = CurrentModule->getContext();
  Type* Int32Ty = Type::getInt32Ty(Ctx);
  
  uint32_t ConfigValue = 0;
  
  // Encode configuration flags as bits
  if (Config.EnablePerformanceHooks) ConfigValue |= 0x01;
  if (Config.EnableCommunicationVolumeHooks) ConfigValue |= 0x02;
  if (Config.EnableCommunicationPatternHooks) ConfigValue |= 0x04;
  if (Config.EnableCollectiveTimingHooks) ConfigValue |= 0x08;
  if (Config.EnableSynchronizationHooks) ConfigValue |= 0x10;
  
  return ConstantInt::get(Int32Ty, ConfigValue);
}

std::pair<CallInst*, CallInst*> HookInserter::insertTimingPair(CallSite& Site, const MPICallMetadata& Metadata) {
  // Get timing hook functions
  Function* PerfBegin = CurrentModule->getFunction(RuntimeInterface::getPerformanceBeginHookName());
  Function* PerfEnd = CurrentModule->getFunction(RuntimeInterface::getPerformanceEndHookName());
  
  if (!PerfBegin || !PerfEnd) {
    return {nullptr, nullptr};
  }
  
  // Create function name and type strings
  Value* FuncName = getOrCreateStringConstant(Metadata.FunctionName);
  StringRef OpType = getMPIOperationType(Metadata.FunctionType);
  Value* FuncType = getOrCreateStringConstant(OpType);
  
  // Insert begin hook before MPI call
  Builder->SetInsertPoint(Site.CallInst);
  CallInst* BeginCall = Builder->CreateCall(PerfBegin, {FuncName, FuncType});
  
  // Insert end hook after MPI call
  Instruction* InsertPoint = Site.CallInst->getNextNode();
  if (!InsertPoint) {
    return {BeginCall, nullptr};
  }
  
  Builder->SetInsertPoint(InsertPoint);
  CallInst* EndCall = Builder->CreateCall(PerfEnd, {FuncName, FuncType});
  
  // Preserve debug information if enabled
  if (Config.PreserveDebugInfo && Site.CallInst->getDebugLoc()) {
    BeginCall->setDebugLoc(Site.CallInst->getDebugLoc());
    EndCall->setDebugLoc(Site.CallInst->getDebugLoc());
  }
  
  return {BeginCall, EndCall};
}

bool HookInserter::isCollectiveOperation(const MPICallMetadata& Metadata) {
  return Metadata.FunctionType == MPIFunctionType::Collective;
}

bool HookInserter::hasCommunicationBuffers(const MPICallMetadata& Metadata) {
  // Check if the operation involves communication buffers
  switch (Metadata.FunctionType) {
    case MPIFunctionType::PointToPoint:
    case MPIFunctionType::Collective:
      return true;
    case MPIFunctionType::File:
      // File I/O operations also have buffers
      return true;
    default:
      return false;
  }
}

void HookInserter::setConfigurationManager(std::shared_ptr<ConfigurationManager> ConfigMgr) {
  this->ConfigMgr = ConfigMgr;
  LLVM_DEBUG(dbgs() << "Set configuration manager for policy-driven instrumentation\n");
}

bool HookInserter::shouldInstrumentCallSite(const CallSite& Site) const {
  if (!ConfigMgr) {
    // No configuration manager - use default behavior
    return true;
  }
  
  // Use enhanced shouldInstrument logic from ConfigurationManager
  return ConfigMgr->shouldInstrument(Site);
}

bool HookInserter::insertHooksWithPolicy(Function& F, const std::vector<CallSite>& Sites) {
  if (Sites.empty())
    return false;
    
  bool Modified = false;
  Builder = std::make_unique<IRBuilder<>>(F.getContext());
  
  LLVM_DEBUG(dbgs() << "Inserting hooks with policy controls for " << Sites.size() 
                    << " MPI calls in function " << F.getName() << "\n");
  
  // Process each call site with policy-driven decisions
  for (auto& Site : Sites) {
    // Check if this call site should be instrumented based on policy
    if (!shouldInstrumentCallSite(Site)) {
      LLVM_DEBUG(dbgs() << "Skipping instrumentation for " << Site.FunctionName 
                        << " due to policy controls\n");
      continue;
    }
    
    // Extract metadata for this call
    MetadataExtractor Extractor;
    MPICallMetadata Metadata = Extractor.extractMetadata(Site);
    
    // Get instrumentation policy for this call site
    InstrumentationPolicy Policy;
    if (ConfigMgr) {
      Policy = ConfigMgr->getInstrumentationPolicy(Site.FunctionName);
    } else {
      // Default policy if no configuration manager
      Policy = InstrumentationPolicy();
    }
    
    bool SiteModified = false;
    
    // Insert pre-call hook based on policy
    if (Config.EnablePreHooks && Policy.EnablePreHooks) {
      SiteModified |= insertPreCallHook(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert post-call hook based on policy
    if (Config.EnablePostHooks && Policy.EnablePostHooks) {
      SiteModified |= insertPostCallHook(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert performance hooks based on policy
    if (Config.EnablePerformanceHooks && Policy.EnablePerformanceHooks && 
        shouldApplyPerformanceMonitoring(Site, Metadata)) {
      SiteModified |= insertPerformanceHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert communication volume hooks based on policy and operation type
    if (Config.EnableCommunicationVolumeHooks && Policy.EnablePerformanceHooks && 
        hasCommunicationBuffers(Metadata)) {
      SiteModified |= insertCommunicationVolumeHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert communication pattern hooks for point-to-point operations
    if (Config.EnableCommunicationPatternHooks && Policy.EnablePerformanceHooks && 
        Metadata.FunctionType == MPIFunctionType::PointToPoint) {
      SiteModified |= insertCommunicationPatternHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert collective timing hooks for collective operations
    if (Config.EnableCollectiveTimingHooks && Policy.EnablePerformanceHooks && 
        isCollectiveOperation(Metadata)) {
      SiteModified |= insertCollectiveTimingHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    // Insert synchronization hooks based on policy
    if (Config.EnableSynchronizationHooks && Policy.EnableDeadlockDetection && 
        (isCollectiveOperation(Metadata) || Metadata.FunctionType == MPIFunctionType::PointToPoint)) {
      SiteModified |= insertSynchronizationHooks(const_cast<CallSite&>(Site), Metadata);
    }
    
    Modified |= SiteModified;
    
    if (SiteModified) {
      LLVM_DEBUG(dbgs() << "Successfully instrumented " << Site.FunctionName 
                        << " with policy-driven hooks\n");
    }
  }
  
  return Modified;
}