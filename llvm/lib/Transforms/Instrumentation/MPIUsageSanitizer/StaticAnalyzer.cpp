//===- StaticAnalyzer.cpp - MPI Static Analysis Engine ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the StaticAnalyzer class which performs compile-time
// analysis to optimize instrumentation overhead while maintaining correctness
// guarantees.
//
//===----------------------------------------------------------------------===//

#include "StaticAnalyzer.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "mpi-static-analyzer"

//===----------------------------------------------------------------------===//
// DataFlowAnalyzer Implementation
//===----------------------------------------------------------------------===//

DataFlowAnalyzer::DataFlowAnalyzer() {
  LLVM_DEBUG(dbgs() << "Initializing MPI Data Flow Analyzer\n");
}

DataFlowAnalyzer::~DataFlowAnalyzer() = default;

void DataFlowAnalyzer::initialize(Function& F, DominatorTree* DT, AAResults* AA) {
  CurrentFunction = &F;
  this->DT = DT;
  this->AA = AA;
  ConstantFlowCache.clear();
  BoundsCache.clear();
}

bool DataFlowAnalyzer::analyzeCallSite(const CallSite& Site, const MPICallMetadata& Metadata) {
  if (!CurrentFunction) return false;
  
  LLVM_DEBUG(dbgs() << "Analyzing data flow for MPI call: " << Site.FunctionName << "\n");
  
  // Analyze key parameters for data flow properties
  bool hasGoodFlow = true;
  
  // Check communicator flow if present
  auto CommIt = Metadata.NamedParameters.find("communicator");
  if (CommIt != Metadata.NamedParameters.end()) {
    hasGoodFlow &= analyzeCommunicatorFlow(CommIt->second);
  }
  
  // Check buffer flow if present
  auto BufferIt = Metadata.NamedParameters.find("buffer");
  if (BufferIt != Metadata.NamedParameters.end()) {
    auto CountIt = Metadata.NamedParameters.find("count");
    Value* Count = CountIt != Metadata.NamedParameters.end() ? CountIt->second : nullptr;
    hasGoodFlow &= analyzeBufferFlow(BufferIt->second, Count);
  }
  
  return hasGoodFlow;
}

bool DataFlowAnalyzer::isConstantFlow(Value* V) {
  if (!V) return false;
  
  // Check cache first
  auto It = ConstantFlowCache.find(V);
  if (It != ConstantFlowCache.end()) {
    return It->second;
  }
  
  bool result = false;
  
  // Direct constants
  if (isa<Constant>(V)) {
    result = true;
  }
  // Trace backwards to find origin
  else {
    SmallVector<Value*, 8> origins = traceValueOrigin(V);
    result = std::all_of(origins.begin(), origins.end(), 
                        [](Value* Origin) { return isa<Constant>(Origin); });
  }
  
  ConstantFlowCache[V] = result;
  return result;
}

bool DataFlowAnalyzer::hasKnownBounds(Value* V) {
  if (!V) return false;
  
  auto It = BoundsCache.find(V);
  if (It != BoundsCache.end()) {
    return It->second;
  }
  
  bool result = false;
  
  // Check for constant bounds
  if (isConstantFlow(V)) {
    result = true;
  }
  // Check for alloca with known size
  else if (auto* AI = dyn_cast<AllocaInst>(V)) {
    result = AI->isArrayAllocation() && isConstantFlow(AI->getArraySize());
  }
  // Check for GEP with constant indices
  else if (auto* GEP = dyn_cast<GetElementPtrInst>(V)) {
    result = std::all_of(GEP->idx_begin(), GEP->idx_end(),
                        [this](Use& U) { return isConstantFlow(U.get()); });
  }
  
  BoundsCache[V] = result;
  return result;
}

bool DataFlowAnalyzer::analyzeCommunicatorFlow(Value* Comm) {
  if (!Comm) return false;
  
  // MPI_COMM_WORLD and MPI_COMM_SELF are always safe
  if (auto* GV = dyn_cast<GlobalVariable>(Comm)) {
    StringRef Name = GV->getName();
    if (Name.contains("MPI_COMM_WORLD") || Name.contains("MPI_COMM_SELF")) {
      return true;
    }
  }
  
  // Check if communicator flows from a constant or known source
  return isConstantFlow(Comm);
}

bool DataFlowAnalyzer::analyzeBufferFlow(Value* Buffer, Value* Count) {
  if (!Buffer) return false;
  
  bool bufferSafe = hasKnownBounds(Buffer);
  bool countSafe = Count ? isConstantFlow(Count) : true;
  
  return bufferSafe && countSafe;
}

bool DataFlowAnalyzer::hasAliasingConcerns(Value* V1, Value* V2) {
  if (!AA || !V1 || !V2) return true; // Conservative assumption
  
  return AA->alias(V1, V2) != AliasResult::NoAlias;
}

bool DataFlowAnalyzer::isInLoop(const CallSite& Site) {
  if (!Site.CallInst || !CurrentFunction) return false;
  
  BasicBlock* BB = Site.CallInst->getParent();
  
  // Simple loop detection - check if block has back edges
  for (BasicBlock* Pred : predecessors(BB)) {
    if (DT && DT->dominates(BB, Pred)) {
      return true; // Back edge found
    }
  }
  
  return false;
}

bool DataFlowAnalyzer::hasComplexControlFlow(const CallSite& Site) {
  if (!Site.CallInst || !CurrentFunction) return false;
  
  BasicBlock* BB = Site.CallInst->getParent();
  
  // Check for multiple predecessors or successors
  unsigned PredCount = std::distance(pred_begin(BB), pred_end(BB));
  unsigned SuccCount = std::distance(succ_begin(BB), succ_end(BB));
  
  return PredCount > 1 || SuccCount > 1;
}

SmallVector<Value*, 8> DataFlowAnalyzer::traceValueOrigin(Value* V) {
  SmallVector<Value*, 8> origins;
  SmallVector<Value*, 8> worklist;
  SmallPtrSet<Value*, 16> visited;
  
  worklist.push_back(V);
  
  while (!worklist.empty()) {
    Value* Current = worklist.pop_back_val();
    if (!visited.insert(Current).second) continue;
    
    if (isa<Constant>(Current) || isa<Argument>(Current)) {
      origins.push_back(Current);
      continue;
    }
    
    if (auto* Inst = dyn_cast<Instruction>(Current)) {
      for (Use& Op : Inst->operands()) {
        worklist.push_back(Op.get());
      }
    }
  }
  
  return origins;
}

bool DataFlowAnalyzer::isDerivedFromParameter(Value* V) {
  SmallVector<Value*, 8> origins = traceValueOrigin(V);
  return std::any_of(origins.begin(), origins.end(),
                    [](Value* Origin) { return isa<Argument>(Origin); });
}

bool DataFlowAnalyzer::isDerivedFromGlobal(Value* V) {
  SmallVector<Value*, 8> origins = traceValueOrigin(V);
  return std::any_of(origins.begin(), origins.end(),
                    [](Value* Origin) { return isa<GlobalVariable>(Origin); });
}

bool DataFlowAnalyzer::analyzePhiNode(PHINode* Phi) {
  // All incoming values should have constant flow
  for (unsigned i = 0; i < Phi->getNumIncomingValues(); ++i) {
    if (!isConstantFlow(Phi->getIncomingValue(i))) {
      return false;
    }
  }
  return true;
}

bool DataFlowAnalyzer::analyzeSelectInst(SelectInst* Select) {
  return isConstantFlow(Select->getTrueValue()) && 
         isConstantFlow(Select->getFalseValue());
}

//===----------------------------------------------------------------------===//
// ConstantAnalyzer Implementation
//===----------------------------------------------------------------------===//

ConstantAnalyzer::ConstantAnalyzer() {
  LLVM_DEBUG(dbgs() << "Initializing MPI Constant Analyzer\n");
}

ConstantAnalyzer::~ConstantAnalyzer() = default;

bool ConstantAnalyzer::isCompileTimeConstant(Value* V) {
  if (!V) return false;
  
  auto It = ConstantCache.find(V);
  if (It != ConstantCache.end()) {
    return It->second;
  }
  
  bool result = false;
  
  if (isa<Constant>(V)) {
    result = true;
  } else if (isConstantExpression(V)) {
    result = true;
  } else if (isEffectivelyConstant(V)) {
    result = true;
  }
  
  ConstantCache[V] = result;
  return result;
}

Constant* ConstantAnalyzer::getConstantValue(Value* V) {
  if (!V) return nullptr;
  
  auto It = ValueCache.find(V);
  if (It != ValueCache.end()) {
    return It->second;
  }
  
  Constant* result = dyn_cast<Constant>(V);
  if (!result && isConstantExpression(V)) {
    // Try to evaluate constant expression
    if (auto* CE = dyn_cast<ConstantExpr>(V)) {
      result = CE;
    }
  }
  
  ValueCache[V] = result;
  return result;
}

bool ConstantAnalyzer::isConstantBufferSize(Value* Count, Value* Datatype) {
  if (!Count) return false;
  
  bool countConstant = isCompileTimeConstant(Count);
  bool datatypeConstant = Datatype ? isConstantDatatype(Datatype) : true;
  
  return countConstant && datatypeConstant;
}

bool ConstantAnalyzer::isConstantCommunicator(Value* Comm) {
  if (!Comm) return false;
  
  // Check for standard communicators
  if (auto* GV = dyn_cast<GlobalVariable>(Comm)) {
    StringRef Name = GV->getName();
    if (Name.contains("MPI_COMM_WORLD") || Name.contains("MPI_COMM_SELF") ||
        Name.contains("MPI_COMM_NULL")) {
      return true;
    }
  }
  
  return isCompileTimeConstant(Comm);
}

bool ConstantAnalyzer::analyzeParameters(const MPICallMetadata& Metadata) {
  bool allConstant = true;
  
  for (const auto& ParamInfo : Metadata.ParameterInfos) {
    switch (ParamInfo.Role) {
      case ParameterRole::Count:
      case ParameterRole::Tag:
      case ParameterRole::Root:
      case ParameterRole::Source:
      case ParameterRole::Destination:
      case ParameterRole::Rank:
        allConstant &= isCompileTimeConstant(ParamInfo.ParamValue);
        break;
      case ParameterRole::Communicator:
        allConstant &= isConstantCommunicator(ParamInfo.ParamValue);
        break;
      case ParameterRole::Datatype:
        allConstant &= isConstantDatatype(ParamInfo.ParamValue);
        break;
      default:
        // Other parameters don't affect constant analysis
        break;
    }
  }
  
  return allConstant;
}

bool ConstantAnalyzer::hasConstantBehavior(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Environment functions have constant behavior
  if (Site.Type == MPIFunctionType::Environment) {
    return true;
  }
  
  // Check if all relevant parameters are constant
  return analyzeParameters(Metadata);
}

bool ConstantAnalyzer::isConstantDatatype(Value* Datatype) {
  if (!Datatype) return false;
  
  // Check for standard MPI datatypes
  if (auto* GV = dyn_cast<GlobalVariable>(Datatype)) {
    StringRef Name = GV->getName();
    if (Name.contains("MPI_INT") || Name.contains("MPI_DOUBLE") ||
        Name.contains("MPI_CHAR") || Name.contains("MPI_FLOAT")) {
      return true;
    }
  }
  
  return isCompileTimeConstant(Datatype);
}

bool ConstantAnalyzer::isConstantTag(Value* Tag) {
  return isCompileTimeConstant(Tag);
}

bool ConstantAnalyzer::isConstantRank(Value* Rank) {
  return isCompileTimeConstant(Rank);
}

bool ConstantAnalyzer::isConstantExpression(Value* V) {
  if (auto* CE = dyn_cast<ConstantExpr>(V)) {
    return true;
  }
  
  if (auto* GEP = dyn_cast<GetElementPtrInst>(V)) {
    return isConstantGEP(GEP);
  }
  
  return false;
}

bool ConstantAnalyzer::isEffectivelyConstant(Value* V) {
  // Check for loop invariant values, function parameters with known values, etc.
  // This is a simplified implementation
  
  if (auto* Load = dyn_cast<LoadInst>(V)) {
    // Check if loading from a constant global
    if (auto* GV = dyn_cast<GlobalVariable>(Load->getPointerOperand())) {
      return GV->isConstant();
    }
  }
  
  return false;
}

bool ConstantAnalyzer::isConstantGEP(GetElementPtrInst* GEP) {
  if (!GEP) return false;
  
  // Check if all indices are constant
  for (auto& Idx : GEP->indices()) {
    if (!isa<Constant>(Idx.get())) {
      return false;
    }
  }
  
  return true;
}

//===----------------------------------------------------------------------===//
// StaticAnalyzer Implementation
//===----------------------------------------------------------------------===//

StaticAnalyzer::StaticAnalyzer() 
  : DFAnalyzer(std::make_unique<DataFlowAnalyzer>()),
    ConstAnalyzer(std::make_unique<ConstantAnalyzer>()),
    DeadlockAnalyzer_(std::make_unique<DeadlockAnalyzer>()) {
  LLVM_DEBUG(dbgs() << "Initializing MPI Static Analyzer\n");
}

StaticAnalyzer::~StaticAnalyzer() = default;

void StaticAnalyzer::initialize(Function& F, DominatorTree* DT, AAResults* AA) {
  CurrentFunction = &F;
  DFAnalyzer->initialize(F, DT, AA);
  DeadlockAnalyzer_->initialize(F, DT);
}

AnalysisResult StaticAnalyzer::analyzeCallSite(const CallSite& Site, const MPICallMetadata& Metadata) {
  AnalysisResult Result;
  
  LLVM_DEBUG(dbgs() << "Analyzing MPI call site: " << Site.FunctionName << "\n");
  
  // Perform constant analysis
  Result.HasConstantParameters = ConstAnalyzer->hasConstantBehavior(Site, Metadata);
  
  // Perform data flow analysis
  DFAnalyzer->analyzeCallSite(Site, Metadata);
  
  // Check for known communicator and buffer size
  auto CommIt = Metadata.NamedParameters.find("communicator");
  if (CommIt != Metadata.NamedParameters.end()) {
    Result.HasKnownCommunicator = ConstAnalyzer->isConstantCommunicator(CommIt->second);
  }
  
  auto CountIt = Metadata.NamedParameters.find("count");
  if (CountIt != Metadata.NamedParameters.end()) {
    auto DatatypeIt = Metadata.NamedParameters.find("datatype");
    Value* Datatype = DatatypeIt != Metadata.NamedParameters.end() ? DatatypeIt->second : nullptr;
    Result.HasKnownBufferSize = ConstAnalyzer->isConstantBufferSize(CountIt->second, Datatype);
  }
  
  // Check control flow properties
  Result.IsInLoop = DFAnalyzer->isInLoop(Site);
  Result.HasComplexControlFlow = DFAnalyzer->hasComplexControlFlow(Site);
  
  // Analyze safety properties
  Result.IsSafe = isProvablySafe(Site, Metadata);
  Result.CouldCauseDeadlock = couldCauseDeadlock(Site, Metadata);
  Result.CouldCauseDataRace = hasDataRaceRisk(Site, Metadata);
  
  // Determine optimization level
  Result.RecommendedLevel = getRecommendedOptimizationLevel(Site, Metadata);
  
  return Result;
}

bool StaticAnalyzer::isProvablySafe(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Enhanced safety analysis for Task 10.2
  
  // Check for compile-time constants
  if (ConstAnalyzer->hasConstantBehavior(Site, Metadata)) {
    LLVM_DEBUG(dbgs() << "MPI call " << Site.FunctionName << " has constant behavior\n");
    return true;
  }
  
  // Check function type safety
  if (analyzeFunctionTypeSafety(Site.Type)) {
    return true;
  }
  
  // Check for known safe patterns
  if (matchesSafePattern(Site, Metadata)) {
    return true;
  }
  
  // Enhanced safety pattern analysis
  if (analyzeSafetyPatterns(Site, Metadata)) {
    return true;
  }
  
  // Check communicator safety
  if (analyzeCommunicatorSafety(Site, Metadata)) {
    return true;
  }
  
  // Check buffer safety
  if (analyzeBufferSafety(Site, Metadata)) {
    return true;
  }
  
  // Check operation ordering safety
  if (analyzeOperationOrdering(Site, Metadata)) {
    return true;
  }
  
  return false;
}

bool StaticAnalyzer::hasCompileTimeConstants(const CallSite& Site, const MPICallMetadata& Metadata) {
  return ConstAnalyzer->analyzeParameters(Metadata);
}

bool StaticAnalyzer::couldCauseDeadlock(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Use the DeadlockAnalyzer for comprehensive deadlock analysis
  if (DeadlockAnalyzer_->analyzeDeadlockRisk(Site, Metadata)) {
    return true;
  }
  
  // Collective operations have potential for deadlock
  if (Site.Type == MPIFunctionType::Collective) {
    return !analyzeCollectiveSafety(Site, Metadata);
  }
  
  // Blocking point-to-point operations can cause deadlock
  if (Site.Type == MPIFunctionType::PointToPoint && !Site.IsIndirect) {
    StringRef FuncName = Site.FunctionName;
    if (FuncName.contains("MPI_Send") || FuncName.contains("MPI_Recv")) {
      return !analyzePointToPointSafety(Site, Metadata);
    }
  }
  
  return false;
}

bool StaticAnalyzer::hasDataRaceRisk(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Enhanced data race analysis for Task 10.2
  
  // Non-blocking operations may have data race risks
  if (Site.Type == MPIFunctionType::PointToPoint) {
    StringRef FuncName = Site.FunctionName;
    if (FuncName.contains("MPI_Isend") || FuncName.contains("MPI_Irecv")) {
      return analyzeNonBlockingDataRace(Site, Metadata);
    }
  }
  
  // One-sided operations have data race potential
  if (Site.Type == MPIFunctionType::Window) {
    return analyzeOneSidedDataRace(Site, Metadata);
  }
  
  // Request handle operations can have data races
  if (Site.Type == MPIFunctionType::Request) {
    return analyzeRequestHandleRace(Site, Metadata);
  }
  
  // Check for memory consistency issues
  if (analyzeMemoryConsistency(Site, Metadata)) {
    return true;
  }
  
  return false;
}

bool StaticAnalyzer::canOptimizeInstrumentation(const CallSite& Site, const MPICallMetadata& Metadata) {
  return isProvablySafe(Site, Metadata) && 
         !couldCauseDeadlock(Site, Metadata) && 
         !hasDataRaceRisk(Site, Metadata);
}

OptimizationLevel StaticAnalyzer::getRecommendedOptimizationLevel(const CallSite& Site, 
                                                                  const MPICallMetadata& Metadata) {
  // Start with no optimization
  OptimizationLevel Level = OptimizationLevel::None;
  
  // Increase optimization level based on analysis
  if (ConstAnalyzer->hasConstantBehavior(Site, Metadata)) {
    Level = OptimizationLevel::Moderate;
  }
  
  if (isProvablySafe(Site, Metadata)) {
    Level = OptimizationLevel::Aggressive;
  }
  
  // Environment functions can be maximally optimized
  if (Site.Type == MPIFunctionType::Environment) {
    Level = OptimizationLevel::Maximum;
  }
  
  // Reduce optimization for risky operations
  if (couldCauseDeadlock(Site, Metadata) || hasDataRaceRisk(Site, Metadata)) {
    Level = OptimizationLevel::Minimal;
  }
  
  return Level;
}

// Legacy methods for backward compatibility
bool StaticAnalyzer::analyzeConstants(const CallSite& Site, const MPICallMetadata& Metadata) {
  return ConstAnalyzer->analyzeParameters(Metadata);
}

bool StaticAnalyzer::performDataFlowAnalysis(const CallSite& Site) {
  // This method is kept for backward compatibility but doesn't have metadata
  // Create empty metadata for the call
  MPICallMetadata EmptyMetadata;
  return DFAnalyzer->analyzeCallSite(Site, EmptyMetadata);
}

bool StaticAnalyzer::analyzeFunctionTypeSafety(MPIFunctionType Type) {
  switch (Type) {
    case MPIFunctionType::Environment:
      // MPI_Init, MPI_Finalize are generally safe
      return true;
    case MPIFunctionType::Info:
    case MPIFunctionType::Error:
      // Info and error functions are typically safe
      return true;
    default:
      return false;
  }
}

bool StaticAnalyzer::matchesSafePattern(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for known safe usage patterns
  // This is a placeholder for more sophisticated pattern matching
  
  StringRef FuncName = Site.FunctionName;
  
  // MPI_Comm_rank and MPI_Comm_size are generally safe
  if (FuncName.contains("MPI_Comm_rank") || FuncName.contains("MPI_Comm_size")) {
    return true;
  }
  
  return false;
}

bool StaticAnalyzer::analyzeCollectiveSafety(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Collective operations are safe if:
  // 1. Communicator is known and constant
  // 2. All processes call the same collective
  // 3. Parameters are consistent across processes
  
  auto CommIt = Metadata.NamedParameters.find("communicator");
  if (CommIt != Metadata.NamedParameters.end()) {
    if (ConstAnalyzer->isConstantCommunicator(CommIt->second)) {
      return true;
    }
  }
  
  return false;
}

bool StaticAnalyzer::analyzePointToPointSafety(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Point-to-point operations are safe if:
  // 1. Source/destination ranks are known
  // 2. Tags are consistent
  // 3. No circular dependencies
  
  bool hasConstantRank = false;
  bool hasConstantTag = false;
  
  auto SourceIt = Metadata.NamedParameters.find("source");
  auto DestIt = Metadata.NamedParameters.find("destination");
  if (SourceIt != Metadata.NamedParameters.end()) {
    hasConstantRank = ConstAnalyzer->isConstantRank(SourceIt->second);
  } else if (DestIt != Metadata.NamedParameters.end()) {
    hasConstantRank = ConstAnalyzer->isConstantRank(DestIt->second);
  }
  
  auto TagIt = Metadata.NamedParameters.find("tag");
  if (TagIt != Metadata.NamedParameters.end()) {
    hasConstantTag = ConstAnalyzer->isConstantTag(TagIt->second);
  }
  
  return hasConstantRank && hasConstantTag;
}

//===----------------------------------------------------------------------===//
// DeadlockAnalyzer Implementation
//===----------------------------------------------------------------------===//

DeadlockAnalyzer::DeadlockAnalyzer() {
  LLVM_DEBUG(dbgs() << "Initializing MPI Deadlock Analyzer\n");
}

DeadlockAnalyzer::~DeadlockAnalyzer() = default;

void DeadlockAnalyzer::initialize(Function& F, DominatorTree* DT) {
  CurrentFunction = &F;
  this->DT = DT;
  DeadlockRiskCache.clear();
  CommunicatorSafetyCache.clear();
}

bool DeadlockAnalyzer::analyzeDeadlockRisk(const CallSite& Site, const MPICallMetadata& Metadata) {
  if (!Site.CallInst) return false;
  
  // Check cache first
  auto It = DeadlockRiskCache.find(Site.CallInst);
  if (It != DeadlockRiskCache.end()) {
    return It->second;
  }
  
  bool hasRisk = false;
  
  // Analyze based on MPI function type
  switch (Site.Type) {
    case MPIFunctionType::Collective:
      hasRisk = analyzeCollectiveDeadlock(Site, Metadata);
      break;
    case MPIFunctionType::PointToPoint:
      hasRisk = analyzePointToPointDeadlock(Site, Metadata);
      break;
    case MPIFunctionType::Request:
      hasRisk = analyzeRequestLifecycle(Site, Metadata);
      break;
    default:
      hasRisk = false;
      break;
  }
  
  // Check for general deadlock patterns
  if (!hasRisk) {
    hasRisk = hasCircularDependency(Site, Metadata) ||
              hasMismatchedCollectives(Site, Metadata) ||
              hasBlockingNonBlockingMix(Site, Metadata) ||
              hasImproperSynchronization(Site, Metadata);
  }
  
  DeadlockRiskCache[Site.CallInst] = hasRisk;
  return hasRisk;
}

bool DeadlockAnalyzer::analyzeCollectiveDeadlock(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Collective operations can deadlock if not called by all processes
  // or if called in different orders
  
  StringRef FuncName = Site.FunctionName;
  
  // Barrier operations are particularly sensitive
  if (FuncName.contains("MPI_Barrier")) {
    return hasBarrierSyncIssues(Site, Metadata);
  }
  
  // Check for rank-dependent collective calls
  if (hasRankDependentDeadlock(Site, Metadata)) {
    return true;
  }
  
  // Check communicator safety
  auto CommIt = Metadata.NamedParameters.find("communicator");
  if (CommIt != Metadata.NamedParameters.end()) {
    return !analyzeCommunicatorDeadlock(CommIt->second);
  }
  
  return false;
}

bool DeadlockAnalyzer::analyzePointToPointDeadlock(const CallSite& Site, const MPICallMetadata& Metadata) {
  StringRef FuncName = Site.FunctionName;
  
  // Blocking send/recv operations can cause deadlock
  if (FuncName.contains("MPI_Send") || FuncName.contains("MPI_Recv")) {
    return detectSendRecvCycle(Site, Metadata);
  }
  
  // Wildcard receives can cause deadlock in certain patterns
  if (FuncName.contains("MPI_Recv") && hasWildcardReceiveDeadlock(Site, Metadata)) {
    return true;
  }
  
  return false;
}

bool DeadlockAnalyzer::hasCircularDependency(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Simplified circular dependency detection
  // In a real implementation, this would analyze the control flow graph
  // and communication patterns to detect potential cycles
  
  StringRef FuncName = Site.FunctionName;
  
  // Check for alternating send/recv patterns that could create cycles
  if (FuncName.contains("MPI_Send") || FuncName.contains("MPI_Recv")) {
    // Look for rank-dependent communication patterns
    auto DestIt = Metadata.NamedParameters.find("destination");
    auto SourceIt = Metadata.NamedParameters.find("source");
    
    if (DestIt != Metadata.NamedParameters.end() || SourceIt != Metadata.NamedParameters.end()) {
      // If destination/source is not constant, there might be circular dependencies
      Value* Target = DestIt != Metadata.NamedParameters.end() ? DestIt->second : SourceIt->second;
      if (Target && !isa<Constant>(Target)) {
        return true;
      }
    }
  }
  
  return false;
}

bool DeadlockAnalyzer::hasMismatchedCollectives(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for potential mismatched collective operations
  // This is a simplified check - real implementation would need global analysis
  
  if (Site.Type != MPIFunctionType::Collective) {
    return false;
  }
  
  // Check if collective is called conditionally based on rank
  return hasRankDependentDeadlock(Site, Metadata);
}

bool DeadlockAnalyzer::hasBlockingNonBlockingMix(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for improper mixing of blocking and non-blocking operations
  StringRef FuncName = Site.FunctionName;
  
  // This is a simplified check - real implementation would analyze
  // the entire function for mixed patterns
  if (FuncName.contains("MPI_Send") && !FuncName.contains("MPI_Isend")) {
    // Blocking send - check if there are non-blocking operations nearby
    // For now, return false as this requires more complex analysis
    return false;
  }
  
  return false;
}

bool DeadlockAnalyzer::hasImproperSynchronization(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for improper synchronization patterns
  StringRef FuncName = Site.FunctionName;
  
  // Wait operations without proper request handling
  if (FuncName.contains("MPI_Wait") || FuncName.contains("MPI_Waitall")) {
    return analyzeRequestLifecycle(Site, Metadata);
  }
  
  return false;
}

bool DeadlockAnalyzer::analyzeRequestLifecycle(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Analyze request handle lifecycle for potential issues
  auto RequestIt = Metadata.NamedParameters.find("request");
  if (RequestIt == Metadata.NamedParameters.end()) {
    return false;
  }
  
  Value* Request = RequestIt->second;
  if (!Request) {
    return false;
  }
  
  // Check if request is properly initialized
  // This is a simplified check - real implementation would trace request origins
  if (isa<UndefValue>(Request) || isa<ConstantPointerNull>(Request)) {
    return true;
  }
  
  return false;
}

bool DeadlockAnalyzer::detectSendRecvCycle(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Detect potential send-receive cycles
  // This is a simplified implementation
  
  auto DestIt = Metadata.NamedParameters.find("destination");
  auto SourceIt = Metadata.NamedParameters.find("source");
  auto TagIt = Metadata.NamedParameters.find("tag");
  
  // If destination/source and tag are not constant, there's potential for cycles
  bool hasVariableTarget = false;
  bool hasVariableTag = false;
  
  if (DestIt != Metadata.NamedParameters.end() && !isa<Constant>(DestIt->second)) {
    hasVariableTarget = true;
  }
  if (SourceIt != Metadata.NamedParameters.end() && !isa<Constant>(SourceIt->second)) {
    hasVariableTarget = true;
  }
  if (TagIt != Metadata.NamedParameters.end() && !isa<Constant>(TagIt->second)) {
    hasVariableTag = true;
  }
  
  return hasVariableTarget || hasVariableTag;
}

bool DeadlockAnalyzer::hasBarrierSyncIssues(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for barrier synchronization issues
  // Barriers can deadlock if not called by all processes in the communicator
  
  // Check if barrier is called conditionally
  if (hasRankDependentDeadlock(Site, Metadata)) {
    return true;
  }
  
  return false;
}

bool DeadlockAnalyzer::analyzeCommunicatorDeadlock(Value* Comm) {
  if (!Comm) return false;
  
  // Check cache first
  auto It = CommunicatorSafetyCache.find(Comm);
  if (It != CommunicatorSafetyCache.end()) {
    return It->second;
  }
  
  bool isSafe = false;
  
  // Standard communicators are generally safe
  if (auto* GV = dyn_cast<GlobalVariable>(Comm)) {
    StringRef Name = GV->getName();
    if (Name.contains("MPI_COMM_WORLD") || Name.contains("MPI_COMM_SELF")) {
      isSafe = true;
    }
  }
  
  // Constant communicators are safer
  if (isa<Constant>(Comm)) {
    isSafe = true;
  }
  
  CommunicatorSafetyCache[Comm] = isSafe;
  return isSafe;
}

bool DeadlockAnalyzer::hasRankDependentDeadlock(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for rank-dependent control flow that could cause deadlock
  // This is a simplified check - real implementation would analyze control flow
  
  if (!CurrentFunction) return false;
  
  // Look for calls to MPI_Comm_rank in the same function
  for (auto& BB : *CurrentFunction) {
    for (auto& I : BB) {
      if (auto* CI = dyn_cast<CallInst>(&I)) {
        if (Function* F = CI->getCalledFunction()) {
          if (F->getName().contains("MPI_Comm_rank")) {
            // Found rank query - this might lead to rank-dependent behavior
            return true;
          }
        }
      }
    }
  }
  
  return false;
}

bool DeadlockAnalyzer::analyzeTagDeadlock(Value* Tag, const CallSite& Site) {
  // Analyze tag usage for potential deadlock
  if (!Tag) return false;
  
  // Wildcard tags (MPI_ANY_TAG) can cause issues
  if (auto* CI = dyn_cast<ConstantInt>(Tag)) {
    // Check for MPI_ANY_TAG value (typically -1)
    if (CI->isMinusOne()) {
      return true;
    }
  }
  
  return false;
}

bool DeadlockAnalyzer::hasWildcardReceiveDeadlock(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for wildcard receive deadlock potential
  auto SourceIt = Metadata.NamedParameters.find("source");
  auto TagIt = Metadata.NamedParameters.find("tag");
  
  bool hasWildcardSource = false;
  bool hasWildcardTag = false;
  
  if (SourceIt != Metadata.NamedParameters.end()) {
    if (auto* CI = dyn_cast<ConstantInt>(SourceIt->second)) {
      // Check for MPI_ANY_SOURCE value (typically -1)
      if (CI->isMinusOne()) {
        hasWildcardSource = true;
      }
    }
  }
  
  if (TagIt != Metadata.NamedParameters.end()) {
    hasWildcardTag = analyzeTagDeadlock(TagIt->second, Site);
  }
  
  return hasWildcardSource || hasWildcardTag;
}

//===----------------------------------------------------------------------===//
// Enhanced StaticAnalyzer Safety Analysis Methods
//===----------------------------------------------------------------------===//

bool StaticAnalyzer::analyzeSafetyPatterns(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Analyze comprehensive safety patterns
  StringRef FuncName = Site.FunctionName;
  
  // Environment functions are generally safe
  if (Site.Type == MPIFunctionType::Environment) {
    return true;
  }
  
  // Info and error functions are typically safe
  if (Site.Type == MPIFunctionType::Info || Site.Type == MPIFunctionType::Error) {
    return true;
  }
  
  // Query functions (rank, size) are safe
  if (FuncName.contains("MPI_Comm_rank") || FuncName.contains("MPI_Comm_size") ||
      FuncName.contains("MPI_Get_count") || FuncName.contains("MPI_Test")) {
    return true;
  }
  
  return false;
}

bool StaticAnalyzer::analyzeOperationOrdering(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Analyze MPI operation ordering for safety
  // This is a simplified implementation
  
  // Single operations without dependencies are generally safe
  if (Site.Type == MPIFunctionType::Environment || 
      Site.Type == MPIFunctionType::Info ||
      Site.Type == MPIFunctionType::Error) {
    return true;
  }
  
  return false;
}

bool StaticAnalyzer::analyzeCommunicatorSafety(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for safe communicator usage patterns
  auto CommIt = Metadata.NamedParameters.find("communicator");
  if (CommIt == Metadata.NamedParameters.end()) {
    return true; // No communicator parameter
  }
  
  Value* Comm = CommIt->second;
  if (!Comm) return false;
  
  // Standard communicators are safe
  if (auto* GV = dyn_cast<GlobalVariable>(Comm)) {
    StringRef Name = GV->getName();
    if (Name.contains("MPI_COMM_WORLD") || Name.contains("MPI_COMM_SELF") ||
        Name.contains("MPI_COMM_NULL")) {
      return true;
    }
  }
  
  // Constant communicators are generally safer
  return isa<Constant>(Comm);
}

bool StaticAnalyzer::analyzeBufferSafety(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Analyze buffer safety (bounds, alignment, etc.)
  auto BufferIt = Metadata.NamedParameters.find("buffer");
  if (BufferIt == Metadata.NamedParameters.end()) {
    return true; // No buffer parameter
  }
  
  Value* Buffer = BufferIt->second;
  if (!Buffer) return false;
  
  // Check if buffer has known bounds
  if (DFAnalyzer->hasKnownBounds(Buffer)) {
    return true;
  }
  
  // Check for null buffer (valid for some operations)
  if (isa<ConstantPointerNull>(Buffer)) {
    // Null buffers are valid for some MPI operations (e.g., MPI_Barrier)
    StringRef FuncName = Site.FunctionName;
    if (FuncName.contains("MPI_Barrier") || FuncName.contains("MPI_Comm_")) {
      return true;
    }
  }
  
  return false;
}

bool StaticAnalyzer::analyzeNonBlockingDataRace(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Enhanced data race analysis for non-blocking operations
  StringRef FuncName = Site.FunctionName;
  
  // Non-blocking operations have inherent data race potential
  if (FuncName.contains("MPI_Isend") || FuncName.contains("MPI_Irecv")) {
    // Check if buffer is accessed after the call
    auto BufferIt = Metadata.NamedParameters.find("buffer");
    if (BufferIt != Metadata.NamedParameters.end()) {
      // In a real implementation, we would analyze buffer usage after the call
      // For now, assume there's always potential for data races
      return true;
    }
  }
  
  return false;
}

bool StaticAnalyzer::analyzeOneSidedDataRace(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Analyze one-sided communication data race potential
  StringRef FuncName = Site.FunctionName;
  
  // One-sided operations (RMA) have data race potential
  if (FuncName.contains("MPI_Put") || FuncName.contains("MPI_Get") || 
      FuncName.contains("MPI_Accumulate")) {
    return !analyzeWindowSynchronization(Site, Metadata);
  }
  
  return false;
}

bool StaticAnalyzer::analyzeRequestHandleRace(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for request handle data races
  auto RequestIt = Metadata.NamedParameters.find("request");
  if (RequestIt == Metadata.NamedParameters.end()) {
    return false;
  }
  
  Value* Request = RequestIt->second;
  if (!Request) return false;
  
  // Check for concurrent access to the same request handle
  // This is a simplified check - real implementation would need alias analysis
  return !isa<Constant>(Request);
}

bool StaticAnalyzer::analyzeWindowSynchronization(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Analyze window synchronization for data races
  StringRef FuncName = Site.FunctionName;
  
  // Synchronization operations are safer
  if (FuncName.contains("MPI_Win_fence") || FuncName.contains("MPI_Win_lock") ||
      FuncName.contains("MPI_Win_unlock")) {
    return true;
  }
  
  // Window creation/destruction operations
  if (FuncName.contains("MPI_Win_create") || FuncName.contains("MPI_Win_free")) {
    return true;
  }
  
  return false;
}

bool StaticAnalyzer::analyzeMemoryConsistency(const CallSite& Site, const MPICallMetadata& Metadata) {
  // Check for memory consistency issues in MPI operations
  StringRef FuncName = Site.FunctionName;
  
  // One-sided operations without proper synchronization can have consistency issues
  if (Site.Type == MPIFunctionType::Window) {
    if (FuncName.contains("MPI_Put") || FuncName.contains("MPI_Get") || 
        FuncName.contains("MPI_Accumulate")) {
      // Check if there's proper synchronization
      return !analyzeWindowSynchronization(Site, Metadata);
    }
  }
  
  return false;
}