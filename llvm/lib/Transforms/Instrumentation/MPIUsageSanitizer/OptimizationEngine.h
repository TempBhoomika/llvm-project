//===- OptimizationEngine.h - MPI Optimization Decision Engine -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the OptimizationEngine class which makes intelligent
// decisions about which MPI operations to instrument based on static analysis
// results and optimization levels.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_OPTIMIZATIONENGINE_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_OPTIMIZATIONENGINE_H

#include "MPICallDetector.h"
#include "MetadataExtractor.h"
#include "StaticAnalyzer.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include <memory>
#include <set>

namespace llvm {

/// Instrumentation level for different optimization modes
enum class InstrumentationLevel {
  None,        // No instrumentation
  Minimal,     // Only critical error-prone operations
  Selective,   // Based on static analysis results
  Standard,    // Default instrumentation level
  Full,        // All detected MPI operations
  Debug        // Maximum instrumentation for debugging
};

/// Optimization decision result for a call site
struct OptimizationDecision {
  bool ShouldInstrument;              // Whether to instrument this call
  InstrumentationLevel Level;         // Recommended instrumentation level
  bool EnablePreHooks;                // Enable pre-call hooks
  bool EnablePostHooks;               // Enable post-call hooks
  bool EnablePerformanceHooks;        // Enable performance monitoring hooks
  bool EnableErrorChecking;           // Enable error checking hooks
  bool EnableDeadlockDetection;       // Enable deadlock detection hooks
  bool EnableDataRaceDetection;       // Enable data race detection hooks
  
  // Optimization rationale
  std::string Reason;                 // Human-readable reason for decision
  double PerformanceImpact;           // Estimated performance impact (0.0-1.0)
  double SafetyBenefit;               // Estimated safety benefit (0.0-1.0)
  
  OptimizationDecision()
    : ShouldInstrument(true), Level(InstrumentationLevel::Standard),
      EnablePreHooks(true), EnablePostHooks(true), EnablePerformanceHooks(false),
      EnableErrorChecking(true), EnableDeadlockDetection(true), 
      EnableDataRaceDetection(true), PerformanceImpact(0.5), SafetyBenefit(0.8) {}
};

/// Configuration for optimization engine behavior
struct OptimizationConfiguration {
  InstrumentationLevel GlobalLevel = InstrumentationLevel::Standard;
  bool EnableOptimizations = true;
  bool EnablePerformanceMonitoring = false;
  bool AggressiveOptimization = false;
  bool PreserveDebugging = true;
  
  // Function type-specific settings
  std::set<MPIFunctionType> EnabledTypes;
  std::set<MPIFunctionType> DisabledTypes;
  
  // Performance thresholds
  double MaxPerformanceImpact = 0.1;     // Maximum acceptable performance impact
  double MinSafetyBenefit = 0.3;         // Minimum required safety benefit
  
  // Optimization weights
  double SafetyWeight = 0.7;             // Weight for safety considerations
  double PerformanceWeight = 0.3;        // Weight for performance considerations
  
  OptimizationConfiguration() {
    // Enable all function types by default
    EnabledTypes = {
      MPIFunctionType::PointToPoint,
      MPIFunctionType::Collective,
      MPIFunctionType::Communicator,
      MPIFunctionType::Datatype,
      MPIFunctionType::Request,
      MPIFunctionType::Info,
      MPIFunctionType::Window,
      MPIFunctionType::File,
      MPIFunctionType::Topology,
      MPIFunctionType::Environment,
      MPIFunctionType::Error
    };
  }
};

/// Statistics for optimization decisions
struct OptimizationStatistics {
  unsigned TotalCallSites = 0;
  unsigned InstrumentedCallSites = 0;
  unsigned OptimizedCallSites = 0;
  unsigned SkippedCallSites = 0;
  
  // By function type
  std::map<MPIFunctionType, unsigned> CallsByType;
  std::map<MPIFunctionType, unsigned> InstrumentedByType;
  std::map<MPIFunctionType, unsigned> OptimizedByType;
  
  // By optimization level
  std::map<OptimizationLevel, unsigned> CallsByLevel;
  
  // Performance metrics
  double EstimatedOverheadReduction = 0.0;
  double EstimatedSafetyCoverage = 0.0;
  
  void reset() {
    TotalCallSites = 0;
    InstrumentedCallSites = 0;
    OptimizedCallSites = 0;
    SkippedCallSites = 0;
    CallsByType.clear();
    InstrumentedByType.clear();
    OptimizedByType.clear();
    CallsByLevel.clear();
    EstimatedOverheadReduction = 0.0;
    EstimatedSafetyCoverage = 0.0;
  }
};

/// Optimization Engine for MPI Instrumentation
///
/// Makes intelligent decisions about which MPI operations to instrument
/// based on static analysis results, optimization levels, and configuration.
class OptimizationEngine {
public:
  OptimizationEngine();
  ~OptimizationEngine();
  
  /// Initialize the optimization engine with configuration
  void initialize(const OptimizationConfiguration& Config);
  
  /// Set the static analyzer for optimization decisions
  void setStaticAnalyzer(std::shared_ptr<StaticAnalyzer> Analyzer);
  
  /// Make optimization decision for a call site
  OptimizationDecision makeDecision(const CallSite& Site, 
                                   const MPICallMetadata& Metadata,
                                   const AnalysisResult& Analysis);
  
  /// Batch optimization for multiple call sites
  std::vector<OptimizationDecision> optimizeCallSites(
    const std::vector<CallSite>& Sites,
    const std::vector<MPICallMetadata>& Metadata,
    const std::vector<AnalysisResult>& Analyses);
  
  /// Check if a call site should be instrumented
  bool shouldInstrument(const CallSite& Site, 
                       const MPICallMetadata& Metadata,
                       const AnalysisResult& Analysis);
  
  /// Get recommended instrumentation level for a call site
  InstrumentationLevel getInstrumentationLevel(const CallSite& Site,
                                              const MPICallMetadata& Metadata,
                                              const AnalysisResult& Analysis);
  
  /// Estimate performance impact of instrumenting a call site
  double estimatePerformanceImpact(const CallSite& Site,
                                  const MPICallMetadata& Metadata,
                                  const AnalysisResult& Analysis);
  
  /// Estimate safety benefit of instrumenting a call site
  double estimateSafetyBenefit(const CallSite& Site,
                              const MPICallMetadata& Metadata,
                              const AnalysisResult& Analysis);
  
  /// Update configuration at runtime
  void updateConfiguration(const OptimizationConfiguration& Config);
  
  /// Get current optimization statistics
  const OptimizationStatistics& getStatistics() const;
  
  /// Reset optimization statistics
  void resetStatistics();
  
  /// Generate optimization report
  std::string generateOptimizationReport() const;

private:
  /// Analyze function type for optimization decisions
  OptimizationDecision analyzeFunctionType(const CallSite& Site,
                                          const MPICallMetadata& Metadata,
                                          const AnalysisResult& Analysis);
  
  /// Apply safety-based optimization rules
  OptimizationDecision applySafetyOptimization(const CallSite& Site,
                                              const MPICallMetadata& Metadata,
                                              const AnalysisResult& Analysis);
  
  /// Apply performance-based optimization rules
  OptimizationDecision applyPerformanceOptimization(const CallSite& Site,
                                                    const MPICallMetadata& Metadata,
                                                    const AnalysisResult& Analysis);
  
  /// Apply configuration-based optimization rules
  OptimizationDecision applyConfigurationRules(const CallSite& Site,
                                               const MPICallMetadata& Metadata,
                                               const AnalysisResult& Analysis);
  
  /// Combine multiple optimization decisions
  OptimizationDecision combineDecisions(const std::vector<OptimizationDecision>& Decisions);
  
  /// Calculate optimization score for a call site
  double calculateOptimizationScore(const CallSite& Site,
                                   const MPICallMetadata& Metadata,
                                   const AnalysisResult& Analysis);
  
  /// Check if function type is enabled for instrumentation
  bool isFunctionTypeEnabled(MPIFunctionType Type) const;
  
  /// Get base instrumentation level for function type
  InstrumentationLevel getBaseLevelForType(MPIFunctionType Type) const;
  
  /// Apply optimization level adjustments
  InstrumentationLevel adjustLevelForOptimization(InstrumentationLevel BaseLevel,
                                                 const AnalysisResult& Analysis) const;
  
  /// Update statistics for a decision
  void updateStatistics(const CallSite& Site,
                       const MPICallMetadata& Metadata,
                       const OptimizationDecision& Decision);
  
  /// Validate optimization decision
  bool validateDecision(const OptimizationDecision& Decision,
                       const CallSite& Site,
                       const MPICallMetadata& Metadata) const;
  
  OptimizationConfiguration Config;
  std::shared_ptr<StaticAnalyzer> Analyzer;
  OptimizationStatistics Stats;
  
  // Caching for optimization decisions
  DenseMap<Instruction*, OptimizationDecision> DecisionCache;
  
  // Performance impact models
  DenseMap<MPIFunctionType, double> BasePerformanceImpact;
  DenseMap<MPIFunctionType, double> BaseSafetyBenefit;
};

/// Optimization Engine Factory
///
/// Creates and configures optimization engines for different use cases.
class OptimizationEngineFactory {
public:
  /// Create optimization engine for development mode
  static std::unique_ptr<OptimizationEngine> createDevelopmentEngine();
  
  /// Create optimization engine for production mode
  static std::unique_ptr<OptimizationEngine> createProductionEngine();
  
  /// Create optimization engine for debugging mode
  static std::unique_ptr<OptimizationEngine> createDebuggingEngine();
  
  /// Create optimization engine with custom configuration
  static std::unique_ptr<OptimizationEngine> createCustomEngine(
    const OptimizationConfiguration& Config);
  
private:
  /// Get default development configuration
  static OptimizationConfiguration getDevelopmentConfig();
  
  /// Get default production configuration
  static OptimizationConfiguration getProductionConfig();
  
  /// Get default debugging configuration
  static OptimizationConfiguration getDebuggingConfig();
};

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_OPTIMIZATIONENGINE_H