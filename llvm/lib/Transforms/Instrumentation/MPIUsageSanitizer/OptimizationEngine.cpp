//===- OptimizationEngine.cpp - MPI Optimization Decision Engine --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the OptimizationEngine class which makes intelligent
// decisions about which MPI operations to instrument based on static analysis
// results and optimization levels.
//
//===----------------------------------------------------------------------===//

#include "OptimizationEngine.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <sstream>

using namespace llvm;

#define DEBUG_TYPE "mpi-optimization-engine"

//===----------------------------------------------------------------------===//
// OptimizationEngine Implementation
//===----------------------------------------------------------------------===//

OptimizationEngine::OptimizationEngine() {
  LLVM_DEBUG(dbgs() << "Initializing MPI Optimization Engine\n");
  
  // Initialize base performance impact models
  BasePerformanceImpact[MPIFunctionType::Environment] = 0.01;      // Very low impact
  BasePerformanceImpact[MPIFunctionType::Info] = 0.02;            // Low impact
  BasePerformanceImpact[MPIFunctionType::Error] = 0.02;           // Low impact
  BasePerformanceImpact[MPIFunctionType::Communicator] = 0.05;    // Low-medium impact
  BasePerformanceImpact[MPIFunctionType::Datatype] = 0.05;        // Low-medium impact
  BasePerformanceImpact[MPIFunctionType::PointToPoint] = 0.15;    // Medium impact
  BasePerformanceImpact[MPIFunctionType::Collective] = 0.20;      // Medium-high impact
  BasePerformanceImpact[MPIFunctionType::Request] = 0.10;         // Medium impact
  BasePerformanceImpact[MPIFunctionType::Window] = 0.25;          // High impact
  BasePerformanceImpact[MPIFunctionType::File] = 0.30;            // High impact
  BasePerformanceImpact[MPIFunctionType::Topology] = 0.08;        // Low-medium impact
  
  // Initialize base safety benefit models
  BaseSafetyBenefit[MPIFunctionType::Environment] = 0.3;          // Low benefit
  BaseSafetyBenefit[MPIFunctionType::Info] = 0.2;                 // Low benefit
  BaseSafetyBenefit[MPIFunctionType::Error] = 0.4;                // Medium benefit
  BaseSafetyBenefit[MPIFunctionType::Communicator] = 0.6;         // Medium-high benefit
  BaseSafetyBenefit[MPIFunctionType::Datatype] = 0.5;             // Medium benefit
  BaseSafetyBenefit[MPIFunctionType::PointToPoint] = 0.9;         // Very high benefit
  BaseSafetyBenefit[MPIFunctionType::Collective] = 0.8;           // High benefit
  BaseSafetyBenefit[MPIFunctionType::Request] = 0.7;              // High benefit
  BaseSafetyBenefit[MPIFunctionType::Window] = 0.95;              // Very high benefit
  BaseSafetyBenefit[MPIFunctionType::File] = 0.7;                 // High benefit
  BaseSafetyBenefit[MPIFunctionType::Topology] = 0.4;             // Medium benefit
}

OptimizationEngine::~OptimizationEngine() = default;

void OptimizationEngine::initialize(const OptimizationConfiguration& Config) {
  this->Config = Config;
  DecisionCache.clear();
  Stats.reset();
  
  LLVM_DEBUG(dbgs() << "OptimizationEngine initialized with level: " 
                    << static_cast<int>(Config.GlobalLevel) << "\n");
}

void OptimizationEngine::setStaticAnalyzer(std::shared_ptr<StaticAnalyzer> Analyzer) {
  this->Analyzer = Analyzer;
}

OptimizationDecision OptimizationEngine::makeDecision(const CallSite& Site,
                                                      const MPICallMetadata& Metadata,
                                                      const AnalysisResult& Analysis) {
  // Check cache first
  if (Site.CallInst) {
    auto It = DecisionCache.find(Site.CallInst);
    if (It != DecisionCache.end()) {
      return It->second;
    }
  }
  
  LLVM_DEBUG(dbgs() << "Making optimization decision for: " << Site.FunctionName << "\n");
  
  // Collect optimization decisions from different strategies
  std::vector<OptimizationDecision> Decisions;
  
  // 1. Function type-based decision
  Decisions.push_back(analyzeFunctionType(Site, Metadata, Analysis));
  
  // 2. Safety-based optimization
  Decisions.push_back(applySafetyOptimization(Site, Metadata, Analysis));
  
  // 3. Performance-based optimization
  Decisions.push_back(applyPerformanceOptimization(Site, Metadata, Analysis));
  
  // 4. Configuration-based rules
  Decisions.push_back(applyConfigurationRules(Site, Metadata, Analysis));
  
  // Combine all decisions
  OptimizationDecision FinalDecision = combineDecisions(Decisions);
  
  // Validate the decision
  if (!validateDecision(FinalDecision, Site, Metadata)) {
    // Fall back to safe default
    FinalDecision = OptimizationDecision();
    FinalDecision.Reason = "Validation failed, using safe default";
  }
  
  // Update statistics
  updateStatistics(Site, Metadata, FinalDecision);
  
  // Cache the decision
  if (Site.CallInst) {
    DecisionCache[Site.CallInst] = FinalDecision;
  }
  
  return FinalDecision;
}

std::vector<OptimizationDecision> OptimizationEngine::optimizeCallSites(
    const std::vector<CallSite>& Sites,
    const std::vector<MPICallMetadata>& Metadata,
    const std::vector<AnalysisResult>& Analyses) {
  
  std::vector<OptimizationDecision> Decisions;
  Decisions.reserve(Sites.size());
  
  for (size_t i = 0; i < Sites.size(); ++i) {
    Decisions.push_back(makeDecision(Sites[i], Metadata[i], Analyses[i]));
  }
  
  return Decisions;
}

bool OptimizationEngine::shouldInstrument(const CallSite& Site,
                                         const MPICallMetadata& Metadata,
                                         const AnalysisResult& Analysis) {
  OptimizationDecision Decision = makeDecision(Site, Metadata, Analysis);
  return Decision.ShouldInstrument;
}

InstrumentationLevel OptimizationEngine::getInstrumentationLevel(const CallSite& Site,
                                                                const MPICallMetadata& Metadata,
                                                                const AnalysisResult& Analysis) {
  OptimizationDecision Decision = makeDecision(Site, Metadata, Analysis);
  return Decision.Level;
}

double OptimizationEngine::estimatePerformanceImpact(const CallSite& Site,
                                                     const MPICallMetadata& Metadata,
                                                     const AnalysisResult& Analysis) {
  double BaseImpact = BasePerformanceImpact[Site.Type];
  
  // Adjust based on analysis results
  if (Analysis.HasConstantParameters) {
    BaseImpact *= 0.7; // Constant parameters reduce overhead
  }
  
  if (Analysis.IsInLoop) {
    BaseImpact *= 2.0; // Loop context increases impact
  }
  
  if (Analysis.HasComplexControlFlow) {
    BaseImpact *= 1.3; // Complex control flow increases impact
  }
  
  // Adjust based on optimization level
  switch (Analysis.RecommendedLevel) {
    case OptimizationLevel::Maximum:
      BaseImpact *= 0.1;
      break;
    case OptimizationLevel::Aggressive:
      BaseImpact *= 0.3;
      break;
    case OptimizationLevel::Moderate:
      BaseImpact *= 0.6;
      break;
    case OptimizationLevel::Minimal:
      BaseImpact *= 0.9;
      break;
    default:
      break;
  }
  
  return std::min(1.0, BaseImpact);
}

double OptimizationEngine::estimateSafetyBenefit(const CallSite& Site,
                                                const MPICallMetadata& Metadata,
                                                const AnalysisResult& Analysis) {
  double BaseBenefit = BaseSafetyBenefit[Site.Type];
  
  // Adjust based on analysis results
  if (Analysis.IsSafe) {
    BaseBenefit *= 0.3; // Safe operations have lower benefit
  }
  
  if (Analysis.CouldCauseDeadlock) {
    BaseBenefit *= 1.5; // Deadlock risk increases benefit
  }
  
  if (Analysis.CouldCauseDataRace) {
    BaseBenefit *= 1.4; // Data race risk increases benefit
  }
  
  if (Analysis.IsInLoop) {
    BaseBenefit *= 1.2; // Loop context increases benefit
  }
  
  return std::min(1.0, BaseBenefit);
}

void OptimizationEngine::updateConfiguration(const OptimizationConfiguration& Config) {
  this->Config = Config;
  DecisionCache.clear(); // Clear cache when configuration changes
}

const OptimizationStatistics& OptimizationEngine::getStatistics() const {
  return Stats;
}

void OptimizationEngine::resetStatistics() {
  Stats.reset();
}

std::string OptimizationEngine::generateOptimizationReport() const {
  std::ostringstream Report;
  
  Report << "=== MPI Optimization Engine Report ===\n";
  Report << "Total Call Sites: " << Stats.TotalCallSites << "\n";
  Report << "Instrumented: " << Stats.InstrumentedCallSites 
         << " (" << (Stats.TotalCallSites > 0 ? 
                    (100.0 * Stats.InstrumentedCallSites / Stats.TotalCallSites) : 0.0)
         << "%)\n";
  Report << "Optimized: " << Stats.OptimizedCallSites 
         << " (" << (Stats.TotalCallSites > 0 ? 
                    (100.0 * Stats.OptimizedCallSites / Stats.TotalCallSites) : 0.0)
         << "%)\n";
  Report << "Skipped: " << Stats.SkippedCallSites 
         << " (" << (Stats.TotalCallSites > 0 ? 
                    (100.0 * Stats.SkippedCallSites / Stats.TotalCallSites) : 0.0)
         << "%)\n";
  
  Report << "\nBy Function Type:\n";
  for (const auto& Entry : Stats.CallsByType) {
    MPIFunctionType Type = Entry.first;
    unsigned Total = Entry.second;
    unsigned Instrumented = Stats.InstrumentedByType.count(Type) ? 
                           Stats.InstrumentedByType.at(Type) : 0;
    unsigned Optimized = Stats.OptimizedByType.count(Type) ? 
                        Stats.OptimizedByType.at(Type) : 0;
    
    Report << "  Type " << static_cast<int>(Type) << ": " 
           << Total << " total, " << Instrumented << " instrumented, " 
           << Optimized << " optimized\n";
  }
  
  Report << "\nPerformance Metrics:\n";
  Report << "Estimated Overhead Reduction: " 
         << (Stats.EstimatedOverheadReduction * 100.0) << "%\n";
  Report << "Estimated Safety Coverage: " 
         << (Stats.EstimatedSafetyCoverage * 100.0) << "%\n";
  
  return Report.str();
}

OptimizationDecision OptimizationEngine::analyzeFunctionType(const CallSite& Site,
                                                            const MPICallMetadata& Metadata,
                                                            const AnalysisResult& Analysis) {
  OptimizationDecision Decision;
  
  // Check if function type is enabled
  if (!isFunctionTypeEnabled(Site.Type)) {
    Decision.ShouldInstrument = false;
    Decision.Level = InstrumentationLevel::None;
    Decision.Reason = "Function type disabled in configuration";
    return Decision;
  }
  
  // Get base level for this function type
  InstrumentationLevel BaseLevel = getBaseLevelForType(Site.Type);
  
  // Adjust level based on optimization analysis
  Decision.Level = adjustLevelForOptimization(BaseLevel, Analysis);
  
  // Set hook preferences based on function type
  switch (Site.Type) {
    case MPIFunctionType::Environment:
      Decision.EnablePreHooks = true;
      Decision.EnablePostHooks = false;
      Decision.EnablePerformanceHooks = false;
      Decision.EnableErrorChecking = true;
      Decision.EnableDeadlockDetection = false;
      Decision.EnableDataRaceDetection = false;
      break;
      
    case MPIFunctionType::PointToPoint:
      Decision.EnablePreHooks = true;
      Decision.EnablePostHooks = true;
      Decision.EnablePerformanceHooks = Config.EnablePerformanceMonitoring;
      Decision.EnableErrorChecking = true;
      Decision.EnableDeadlockDetection = true;
      Decision.EnableDataRaceDetection = true;
      break;
      
    case MPIFunctionType::Collective:
      Decision.EnablePreHooks = true;
      Decision.EnablePostHooks = true;
      Decision.EnablePerformanceHooks = Config.EnablePerformanceMonitoring;
      Decision.EnableErrorChecking = true;
      Decision.EnableDeadlockDetection = true;
      Decision.EnableDataRaceDetection = false;
      break;
      
    case MPIFunctionType::Window:
      Decision.EnablePreHooks = true;
      Decision.EnablePostHooks = true;
      Decision.EnablePerformanceHooks = Config.EnablePerformanceMonitoring;
      Decision.EnableErrorChecking = true;
      Decision.EnableDeadlockDetection = false;
      Decision.EnableDataRaceDetection = true;
      break;
      
    default:
      // Default settings for other types
      Decision.EnablePreHooks = true;
      Decision.EnablePostHooks = true;
      Decision.EnablePerformanceHooks = false;
      Decision.EnableErrorChecking = true;
      Decision.EnableDeadlockDetection = false;
      Decision.EnableDataRaceDetection = false;
      break;
  }
  
  Decision.Reason = "Function type-based decision";
  return Decision;
}

OptimizationDecision OptimizationEngine::applySafetyOptimization(const CallSite& Site,
                                                                const MPICallMetadata& Metadata,
                                                                const AnalysisResult& Analysis) {
  OptimizationDecision Decision;
  
  // If operation is provably safe, reduce instrumentation
  if (Analysis.IsSafe) {
    Decision.Level = InstrumentationLevel::Minimal;
    Decision.EnableErrorChecking = false;
    Decision.EnableDeadlockDetection = false;
    Decision.EnableDataRaceDetection = false;
    Decision.Reason = "Operation is provably safe";
  }
  // If operation has high risk, increase instrumentation
  else if (Analysis.CouldCauseDeadlock || Analysis.CouldCauseDataRace) {
    Decision.Level = InstrumentationLevel::Full;
    Decision.EnableErrorChecking = true;
    Decision.EnableDeadlockDetection = Analysis.CouldCauseDeadlock;
    Decision.EnableDataRaceDetection = Analysis.CouldCauseDataRace;
    Decision.Reason = "High-risk operation detected";
  }
  // Standard safety instrumentation
  else {
    Decision.Level = InstrumentationLevel::Standard;
    Decision.EnableErrorChecking = true;
    Decision.EnableDeadlockDetection = true;
    Decision.EnableDataRaceDetection = true;
    Decision.Reason = "Standard safety instrumentation";
  }
  
  return Decision;
}

OptimizationDecision OptimizationEngine::applyPerformanceOptimization(const CallSite& Site,
                                                                      const MPICallMetadata& Metadata,
                                                                      const AnalysisResult& Analysis) {
  OptimizationDecision Decision;
  
  double PerformanceImpact = estimatePerformanceImpact(Site, Metadata, Analysis);
  double SafetyBenefit = estimateSafetyBenefit(Site, Metadata, Analysis);
  
  // Calculate optimization score
  double Score = calculateOptimizationScore(Site, Metadata, Analysis);
  
  // Make decision based on performance vs. safety trade-off
  if (PerformanceImpact > Config.MaxPerformanceImpact && 
      SafetyBenefit < Config.MinSafetyBenefit) {
    Decision.ShouldInstrument = false;
    Decision.Level = InstrumentationLevel::None;
    Decision.Reason = "Performance impact too high, safety benefit too low";
  }
  else if (Score > 0.8) {
    Decision.Level = InstrumentationLevel::Full;
    Decision.Reason = "High optimization score";
  }
  else if (Score > 0.6) {
    Decision.Level = InstrumentationLevel::Standard;
    Decision.Reason = "Medium optimization score";
  }
  else if (Score > 0.4) {
    Decision.Level = InstrumentationLevel::Selective;
    Decision.Reason = "Low-medium optimization score";
  }
  else {
    Decision.Level = InstrumentationLevel::Minimal;
    Decision.Reason = "Low optimization score";
  }
  
  Decision.PerformanceImpact = PerformanceImpact;
  Decision.SafetyBenefit = SafetyBenefit;
  
  return Decision;
}

OptimizationDecision OptimizationEngine::applyConfigurationRules(const CallSite& Site,
                                                                 const MPICallMetadata& Metadata,
                                                                 const AnalysisResult& Analysis) {
  OptimizationDecision Decision;
  
  // Apply global configuration level
  Decision.Level = Config.GlobalLevel;
  
  // Check if optimizations are enabled
  if (!Config.EnableOptimizations) {
    Decision.Level = InstrumentationLevel::Full;
    Decision.Reason = "Optimizations disabled in configuration";
    return Decision;
  }
  
  // Apply aggressive optimization if enabled
  if (Config.AggressiveOptimization && Analysis.IsSafe) {
    Decision.ShouldInstrument = false;
    Decision.Level = InstrumentationLevel::None;
    Decision.Reason = "Aggressive optimization for safe operation";
    return Decision;
  }
  
  // Preserve debugging information if requested
  if (Config.PreserveDebugging) {
    Decision.EnablePreHooks = true;
    Decision.EnablePostHooks = true;
  }
  
  // Apply performance monitoring settings
  Decision.EnablePerformanceHooks = Config.EnablePerformanceMonitoring;
  
  Decision.Reason = "Configuration-based rules applied";
  return Decision;
}

OptimizationDecision OptimizationEngine::combineDecisions(const std::vector<OptimizationDecision>& Decisions) {
  if (Decisions.empty()) {
    return OptimizationDecision();
  }
  
  OptimizationDecision Combined = Decisions[0];
  
  // Combine instrumentation decisions (conservative approach)
  for (size_t i = 1; i < Decisions.size(); ++i) {
    const auto& Decision = Decisions[i];
    
    // If any decision says don't instrument, don't instrument
    if (!Decision.ShouldInstrument) {
      Combined.ShouldInstrument = false;
    }
    
    // Use the most conservative (highest) instrumentation level
    if (Decision.Level > Combined.Level) {
      Combined.Level = Decision.Level;
    }
    
    // Enable hooks if any decision enables them
    Combined.EnablePreHooks = Combined.EnablePreHooks || Decision.EnablePreHooks;
    Combined.EnablePostHooks = Combined.EnablePostHooks || Decision.EnablePostHooks;
    Combined.EnablePerformanceHooks = Combined.EnablePerformanceHooks || Decision.EnablePerformanceHooks;
    Combined.EnableErrorChecking = Combined.EnableErrorChecking || Decision.EnableErrorChecking;
    Combined.EnableDeadlockDetection = Combined.EnableDeadlockDetection || Decision.EnableDeadlockDetection;
    Combined.EnableDataRaceDetection = Combined.EnableDataRaceDetection || Decision.EnableDataRaceDetection;
    
    // Use maximum performance impact and safety benefit
    Combined.PerformanceImpact = std::max(Combined.PerformanceImpact, Decision.PerformanceImpact);
    Combined.SafetyBenefit = std::max(Combined.SafetyBenefit, Decision.SafetyBenefit);
  }
  
  // Combine reasons
  std::ostringstream ReasonStream;
  for (size_t i = 0; i < Decisions.size(); ++i) {
    if (i > 0) ReasonStream << "; ";
    ReasonStream << Decisions[i].Reason;
  }
  Combined.Reason = ReasonStream.str();
  
  return Combined;
}

double OptimizationEngine::calculateOptimizationScore(const CallSite& Site,
                                                     const MPICallMetadata& Metadata,
                                                     const AnalysisResult& Analysis) {
  double PerformanceImpact = estimatePerformanceImpact(Site, Metadata, Analysis);
  double SafetyBenefit = estimateSafetyBenefit(Site, Metadata, Analysis);
  
  // Weighted score calculation
  double Score = (Config.SafetyWeight * SafetyBenefit) - 
                 (Config.PerformanceWeight * PerformanceImpact);
  
  // Normalize to 0-1 range
  return std::max(0.0, std::min(1.0, Score));
}

bool OptimizationEngine::isFunctionTypeEnabled(MPIFunctionType Type) const {
  // Check if explicitly disabled
  if (Config.DisabledTypes.count(Type) > 0) {
    return false;
  }
  
  // Check if explicitly enabled or enabled by default
  return Config.EnabledTypes.count(Type) > 0;
}

InstrumentationLevel OptimizationEngine::getBaseLevelForType(MPIFunctionType Type) const {
  switch (Type) {
    case MPIFunctionType::Environment:
      return InstrumentationLevel::Minimal;
    case MPIFunctionType::Info:
    case MPIFunctionType::Error:
      return InstrumentationLevel::Selective;
    case MPIFunctionType::PointToPoint:
    case MPIFunctionType::Collective:
    case MPIFunctionType::Window:
      return InstrumentationLevel::Standard;
    default:
      return InstrumentationLevel::Standard;
  }
}

InstrumentationLevel OptimizationEngine::adjustLevelForOptimization(InstrumentationLevel BaseLevel,
                                                                   const AnalysisResult& Analysis) const {
  // Adjust based on static analysis results
  if (Analysis.IsSafe && Config.EnableOptimizations) {
    // Reduce instrumentation for safe operations
    switch (BaseLevel) {
      case InstrumentationLevel::Full:
        return InstrumentationLevel::Standard;
      case InstrumentationLevel::Standard:
        return InstrumentationLevel::Selective;
      case InstrumentationLevel::Selective:
        return InstrumentationLevel::Minimal;
      default:
        return BaseLevel;
    }
  }
  
  if ((Analysis.CouldCauseDeadlock || Analysis.CouldCauseDataRace) && 
      BaseLevel < InstrumentationLevel::Standard) {
    // Increase instrumentation for risky operations
    return InstrumentationLevel::Standard;
  }
  
  return BaseLevel;
}

void OptimizationEngine::updateStatistics(const CallSite& Site,
                                         const MPICallMetadata& Metadata,
                                         const OptimizationDecision& Decision) {
  Stats.TotalCallSites++;
  Stats.CallsByType[Site.Type]++;
  Stats.CallsByLevel[Decision.Level]++;
  
  if (Decision.ShouldInstrument) {
    Stats.InstrumentedCallSites++;
    Stats.InstrumentedByType[Site.Type]++;
  } else {
    Stats.SkippedCallSites++;
  }
  
  if (Decision.Level < InstrumentationLevel::Standard) {
    Stats.OptimizedCallSites++;
    Stats.OptimizedByType[Site.Type]++;
  }
  
  // Update performance metrics
  Stats.EstimatedOverheadReduction += (1.0 - Decision.PerformanceImpact) / Stats.TotalCallSites;
  Stats.EstimatedSafetyCoverage += Decision.SafetyBenefit / Stats.TotalCallSites;
}

bool OptimizationEngine::validateDecision(const OptimizationDecision& Decision,
                                         const CallSite& Site,
                                         const MPICallMetadata& Metadata) const {
  // Basic validation checks
  if (Decision.Level == InstrumentationLevel::None && Decision.ShouldInstrument) {
    return false; // Inconsistent decision
  }
  
  if (!Decision.ShouldInstrument && 
      (Decision.EnablePreHooks || Decision.EnablePostHooks)) {
    return false; // Inconsistent hook settings
  }
  
  if (Decision.PerformanceImpact < 0.0 || Decision.PerformanceImpact > 1.0) {
    return false; // Invalid performance impact
  }
  
  if (Decision.SafetyBenefit < 0.0 || Decision.SafetyBenefit > 1.0) {
    return false; // Invalid safety benefit
  }
  
  return true;
}

//===----------------------------------------------------------------------===//
// OptimizationEngineFactory Implementation
//===----------------------------------------------------------------------===//

std::unique_ptr<OptimizationEngine> OptimizationEngineFactory::createDevelopmentEngine() {
  auto Engine = std::make_unique<OptimizationEngine>();
  Engine->initialize(getDevelopmentConfig());
  return Engine;
}

std::unique_ptr<OptimizationEngine> OptimizationEngineFactory::createProductionEngine() {
  auto Engine = std::make_unique<OptimizationEngine>();
  Engine->initialize(getProductionConfig());
  return Engine;
}

std::unique_ptr<OptimizationEngine> OptimizationEngineFactory::createDebuggingEngine() {
  auto Engine = std::make_unique<OptimizationEngine>();
  Engine->initialize(getDebuggingConfig());
  return Engine;
}

std::unique_ptr<OptimizationEngine> OptimizationEngineFactory::createCustomEngine(
    const OptimizationConfiguration& Config) {
  auto Engine = std::make_unique<OptimizationEngine>();
  Engine->initialize(Config);
  return Engine;
}

OptimizationConfiguration OptimizationEngineFactory::getDevelopmentConfig() {
  OptimizationConfiguration Config;
  Config.GlobalLevel = InstrumentationLevel::Standard;
  Config.EnableOptimizations = true;
  Config.EnablePerformanceMonitoring = false;
  Config.AggressiveOptimization = false;
  Config.PreserveDebugging = true;
  Config.MaxPerformanceImpact = 0.15;
  Config.MinSafetyBenefit = 0.2;
  Config.SafetyWeight = 0.8;
  Config.PerformanceWeight = 0.2;
  return Config;
}

OptimizationConfiguration OptimizationEngineFactory::getProductionConfig() {
  OptimizationConfiguration Config;
  Config.GlobalLevel = InstrumentationLevel::Selective;
  Config.EnableOptimizations = true;
  Config.EnablePerformanceMonitoring = false;
  Config.AggressiveOptimization = true;
  Config.PreserveDebugging = false;
  Config.MaxPerformanceImpact = 0.05;
  Config.MinSafetyBenefit = 0.4;
  Config.SafetyWeight = 0.6;
  Config.PerformanceWeight = 0.4;
  return Config;
}

OptimizationConfiguration OptimizationEngineFactory::getDebuggingConfig() {
  OptimizationConfiguration Config;
  Config.GlobalLevel = InstrumentationLevel::Full;
  Config.EnableOptimizations = false;
  Config.EnablePerformanceMonitoring = true;
  Config.AggressiveOptimization = false;
  Config.PreserveDebugging = true;
  Config.MaxPerformanceImpact = 1.0;
  Config.MinSafetyBenefit = 0.0;
  Config.SafetyWeight = 1.0;
  Config.PerformanceWeight = 0.0;
  return Config;
}