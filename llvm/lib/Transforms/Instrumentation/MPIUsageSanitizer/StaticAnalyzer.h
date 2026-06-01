//===- StaticAnalyzer.h - MPI Static Analysis Engine ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the StaticAnalyzer class which performs compile-time
// analysis to optimize instrumentation overhead while maintaining correctness
// guarantees.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_STATICANALYZER_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_STATICANALYZER_H

#include "MPICallDetector.h"
#include "MetadataExtractor.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Value.h"
#include <memory>

namespace llvm {

class Function;
class BasicBlock;
class Instruction;

/// Optimization level recommendations for instrumentation
enum class OptimizationLevel {
  None,        // No optimization, full instrumentation
  Minimal,     // Basic optimizations, most instrumentation
  Moderate,    // Balanced optimization and instrumentation
  Aggressive,  // Heavy optimization, minimal instrumentation
  Maximum      // Maximum optimization, safety-critical only
};

/// Analysis result for MPI call sites
struct AnalysisResult {
  bool IsSafe;                    // Operation is provably safe
  bool HasConstantParameters;     // Parameters are compile-time constants
  bool CouldCauseDeadlock;       // Potential deadlock risk
  bool CouldCauseDataRace;       // Potential data race risk
  OptimizationLevel RecommendedLevel; // Recommended optimization level
  
  // Additional analysis flags
  bool HasKnownCommunicator;     // Communicator is statically known
  bool HasKnownBufferSize;       // Buffer size is statically known
  bool IsInLoop;                 // Call is inside a loop
  bool HasComplexControlFlow;    // Complex control flow around call
  
  AnalysisResult() 
    : IsSafe(false), HasConstantParameters(false), CouldCauseDeadlock(false),
      CouldCauseDataRace(false), RecommendedLevel(OptimizationLevel::None),
      HasKnownCommunicator(false), HasKnownBufferSize(false), 
      IsInLoop(false), HasComplexControlFlow(false) {}
};

/// Data Flow Analyzer for compile-time analysis
class DataFlowAnalyzer {
public:
  DataFlowAnalyzer();
  ~DataFlowAnalyzer();
  
  /// Initialize analyzer with function context
  void initialize(Function& F, DominatorTree* DT = nullptr, AAResults* AA = nullptr);
  
  /// Analyze data flow for a specific call site
  bool analyzeCallSite(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check if a value flows from a constant source
  bool isConstantFlow(Value* V);
  
  /// Check if a value has known bounds
  bool hasKnownBounds(Value* V);
  
  /// Analyze communicator flow
  bool analyzeCommunicatorFlow(Value* Comm);
  
  /// Analyze buffer parameter flow
  bool analyzeBufferFlow(Value* Buffer, Value* Count = nullptr);
  
  /// Check for potential aliasing issues
  bool hasAliasingConcerns(Value* V1, Value* V2);
  
  /// Detect if call is in a loop
  bool isInLoop(const CallSite& Site);
  
  /// Analyze control flow complexity around call site
  bool hasComplexControlFlow(const CallSite& Site);

private:
  /// Trace value backwards to find its origin
  SmallVector<Value*, 8> traceValueOrigin(Value* V);
  
  /// Check if value is derived from function parameters
  bool isDerivedFromParameter(Value* V);
  
  /// Check if value is derived from global variables
  bool isDerivedFromGlobal(Value* V);
  
  /// Analyze phi nodes and select instructions
  bool analyzePhiNode(PHINode* Phi);
  bool analyzeSelectInst(SelectInst* Select);
  
  Function* CurrentFunction = nullptr;
  DominatorTree* DT = nullptr;
  AAResults* AA = nullptr;
  DenseMap<Value*, bool> ConstantFlowCache;
  DenseMap<Value*, bool> BoundsCache;
};

/// Deadlock Analyzer for detecting potential deadlock conditions
class DeadlockAnalyzer {
public:
  DeadlockAnalyzer();
  ~DeadlockAnalyzer();
  
  /// Initialize analyzer with function context
  void initialize(Function& F, DominatorTree* DT = nullptr);
  
  /// Analyze potential deadlock conditions for a call site
  bool analyzeDeadlockRisk(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check for collective operation deadlock potential
  bool analyzeCollectiveDeadlock(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check for point-to-point deadlock potential
  bool analyzePointToPointDeadlock(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze communication patterns for circular dependencies
  bool hasCircularDependency(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check for mismatched collective operations
  bool hasMismatchedCollectives(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze blocking vs non-blocking operation mixing
  bool hasBlockingNonBlockingMix(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check for improper synchronization patterns
  bool hasImproperSynchronization(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze request handle lifecycle for deadlock potential
  bool analyzeRequestLifecycle(const CallSite& Site, const MPICallMetadata& Metadata);

private:
  /// Detect send-receive cycles that could cause deadlock
  bool detectSendRecvCycle(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check for barrier synchronization issues
  bool hasBarrierSyncIssues(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze communicator usage for deadlock potential
  bool analyzeCommunicatorDeadlock(Value* Comm);
  
  /// Check for rank-dependent control flow that could cause deadlock
  bool hasRankDependentDeadlock(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze tag usage for potential deadlock
  bool analyzeTagDeadlock(Value* Tag, const CallSite& Site);
  
  /// Check for wildcard receive deadlock potential
  bool hasWildcardReceiveDeadlock(const CallSite& Site, const MPICallMetadata& Metadata);
  
  Function* CurrentFunction = nullptr;
  DominatorTree* DT = nullptr;
  
  // Cache for analysis results
  DenseMap<Instruction*, bool> DeadlockRiskCache;
  DenseMap<Value*, bool> CommunicatorSafetyCache;
};

/// Constant Analyzer for compile-time constant detection
class ConstantAnalyzer {
public:
  ConstantAnalyzer();
  ~ConstantAnalyzer();
  
  /// Check if a value is a compile-time constant
  bool isCompileTimeConstant(Value* V);
  
  /// Get constant value if available
  Constant* getConstantValue(Value* V);
  
  /// Check if buffer size is compile-time constant
  bool isConstantBufferSize(Value* Count, Value* Datatype = nullptr);
  
  /// Check if communicator is compile-time constant
  bool isConstantCommunicator(Value* Comm);
  
  /// Analyze all parameters for constant values
  bool analyzeParameters(const MPICallMetadata& Metadata);
  
  /// Check if MPI operation has constant behavior
  bool hasConstantBehavior(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Detect compile-time constant MPI datatypes
  bool isConstantDatatype(Value* Datatype);
  
  /// Detect compile-time constant tags and ranks
  bool isConstantTag(Value* Tag);
  bool isConstantRank(Value* Rank);

private:
  /// Recursively check for constant expressions
  bool isConstantExpression(Value* V);
  
  /// Check for effectively constant values (loop invariant, etc.)
  bool isEffectivelyConstant(Value* V);
  
  /// Analyze GEP instructions for constant offsets
  bool isConstantGEP(GetElementPtrInst* GEP);
  
  /// Cache for constant analysis results
  DenseMap<Value*, bool> ConstantCache;
  DenseMap<Value*, Constant*> ValueCache;
};

/// Static Analyzer for MPI Operations
///
/// Performs compile-time analysis to optimize instrumentation overhead
/// while maintaining correctness guarantees.
class StaticAnalyzer {
public:
  StaticAnalyzer();
  ~StaticAnalyzer();
  
  /// Initialize analyzer with function context
  void initialize(Function& F, DominatorTree* DT = nullptr, AAResults* AA = nullptr);
  
  /// Perform comprehensive analysis of a call site
  AnalysisResult analyzeCallSite(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check if an MPI operation is provably safe and can skip instrumentation
  bool isProvablySafe(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check if operation has compile-time constants
  bool hasCompileTimeConstants(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze potential deadlock conditions
  bool couldCauseDeadlock(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze potential data race conditions
  bool hasDataRaceRisk(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Determine if instrumentation can be optimized for this call
  bool canOptimizeInstrumentation(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Get recommended optimization level
  OptimizationLevel getRecommendedOptimizationLevel(const CallSite& Site, 
                                                    const MPICallMetadata& Metadata);

  /// Enhanced safety analysis methods for Task 10.2
  
  /// Comprehensive safety pattern analysis
  bool analyzeSafetyPatterns(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze MPI operation ordering for safety
  bool analyzeOperationOrdering(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check for safe communicator usage patterns
  bool analyzeCommunicatorSafety(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze buffer safety (bounds, alignment, etc.)
  bool analyzeBufferSafety(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Enhanced data race analysis for non-blocking and one-sided operations
  bool analyzeNonBlockingDataRace(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze one-sided communication data race potential
  bool analyzeOneSidedDataRace(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check for request handle data races
  bool analyzeRequestHandleRace(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze window synchronization for data races
  bool analyzeWindowSynchronization(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check for memory consistency issues in MPI operations
  bool analyzeMemoryConsistency(const CallSite& Site, const MPICallMetadata& Metadata);

private:
  /// Analyze compile-time constants (legacy method)
  bool analyzeConstants(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Perform data flow analysis (legacy method)
  bool performDataFlowAnalysis(const CallSite& Site);
  
  /// Analyze MPI function type for safety characteristics
  bool analyzeFunctionTypeSafety(MPIFunctionType Type);
  
  /// Check for known safe patterns
  bool matchesSafePattern(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze collective operation safety
  bool analyzeCollectiveSafety(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Analyze point-to-point operation safety
  bool analyzePointToPointSafety(const CallSite& Site, const MPICallMetadata& Metadata);
  
  std::unique_ptr<DataFlowAnalyzer> DFAnalyzer;
  std::unique_ptr<ConstantAnalyzer> ConstAnalyzer;
  std::unique_ptr<DeadlockAnalyzer> DeadlockAnalyzer_;
  Function* CurrentFunction = nullptr;
};

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_STATICANALYZER_H