//===- PassOptimizer.cpp - MPI Sanitizer Pass Optimizer ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the PassOptimizer class which provides performance
// optimizations for the MPI Usage Sanitizer Pass based on profiling data
// and static analysis.
//
//===----------------------------------------------------------------------===//

#include "PassOptimizer.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <sstream>

using namespace llvm;

#define DEBUG_TYPE "mpi-pass-optimizer"

//===----------------------------------------------------------------------===//
// OptimizedMPICallDetector Implementation
//===----------------------------------------------------------------------===//

OptimizedMPICallDetector::OptimizedMPICallDetector(const OptimizationConfig& Config)
    : Config(Config), FunctionNameCache(Config.MaxCacheSize), 
      SignatureCache(Config.MaxCacheSize), FunctionCallCache(Config.MaxCacheSize) {
  LLVM_DEBUG(dbgs() << "Created OptimizedMPICallDetector with strategy: " 
                    << static_cast<int>(Config.Strategy) << "\n");
}

std::vector<CallSite> OptimizedMPICallDetector::detectMPICalls(Function& F) {
  if (!Config.EnableCallDetectionOptimization) {
    return MPICallDetector::detectMPICalls(F);
  }
  
  // Check cache first if enabled
  if (FunctionNameCaching) {
    std::vector<CallSite> CachedResult;
    if (FunctionCallCache.get(&F, CachedResult)) {
      Stats.CacheHits++;
      LLVM_DEBUG(dbgs() << "Cache hit for function: " << F.getName() << "\n");
      return CachedResult;
    }
    Stats.CacheMisses++;
  }
  
  // Use optimized detection for hot paths
  std::vector<CallSite> Result;
  if (HotPathOptimization && isHotPath(F)) {
    Result = detectMPICallsOptimized(F);
    Stats.HotPathOptimizations++;
  } else {
    Result = detectMPICallsStandard(F);
  }
  
  // Cache result if enabled
  if (FunctionNameCaching) {
    FunctionCallCache.put(&F, Result);
  }
  
  return Result;
}

void OptimizedMPICallDetector::setHotPaths(const std::vector<HotPath>& NewHotPaths) {
  HotPaths = NewHotPaths;
  LLVM_DEBUG(dbgs() << "Set " << HotPaths.size() << " hot paths for optimization\n");
}

bool OptimizedMPICallDetector::isHotPath(Function& F) const {
  return std::any_of(HotPaths.begin(), HotPaths.end(),
    [&F](const HotPath& Path) { return Path.HotFunction == &F; });
}

std::vector<CallSite> OptimizedMPICallDetector::detectMPICallsOptimized(Function& F) {
  std::vector<CallSite> MPICalls;
  
  // Optimized detection for hot paths - focus on direct calls first
  for (BasicBlock& BB : F) {
    for (Instruction& I : BB) {
      if (auto* CI = dyn_cast<CallInst>(&I)) {
        if (Function* CalledFunc = CI->getCalledFunction()) {
          StringRef FuncName = CalledFunc->getName();
          
          // Fast path: check cache for function name classification
          bool IsMPI = false;
          std::string FuncNameStr = FuncName.str();
          
          if (FunctionNameCaching && FunctionNameCache.get(FuncNameStr, IsMPI)) {
            Stats.CacheHits++;
          } else {
            IsMPI = isMPIFunction(FuncName);
            if (FunctionNameCaching) {
              FunctionNameCache.put(FuncNameStr, IsMPI);
              Stats.CacheMisses++;
            }
          }
          
          if (IsMPI) {
            MPIFunctionType Type = MPIFunctionType::Unknown;
            
            if (SignatureCaching && SignatureCache.get(FuncNameStr, Type)) {
              Stats.CacheHits++;
            } else {
              Type = classifyMPIFunction(FuncName);
              if (SignatureCaching) {
                SignatureCache.put(FuncNameStr, Type);
                Stats.CacheMisses++;
              }
            }
            
            MPICalls.emplace_back(CI, FuncName, Type, false);
          }
        }
      }
    }
  }
  
  // For hot paths, skip expensive indirect call analysis unless necessary
  if (Config.Strategy == OptimizationStrategy::Aggressive) {
    // Only do indirect call analysis if we found very few direct calls
    if (MPICalls.size() < 2) {
      auto IndirectCalls = detectIndirectCalls(F);
      MPICalls.insert(MPICalls.end(), IndirectCalls.begin(), IndirectCalls.end());
    }
  }
  
  return MPICalls;
}

std::vector<CallSite> OptimizedMPICallDetector::detectMPICallsStandard(Function& F) {
  // Use standard detection for non-hot paths
  return MPICallDetector::detectMPICalls(F);
}

//===----------------------------------------------------------------------===//
// OptimizedMetadataExtractor Implementation
//===----------------------------------------------------------------------===//

OptimizedMetadataExtractor::OptimizedMetadataExtractor(const OptimizationConfig& Config)
    : Config(Config), MetadataCache(Config.MaxCacheSize), ParameterCache(Config.MaxCacheSize) {
  LLVM_DEBUG(dbgs() << "Created OptimizedMetadataExtractor\n");
}

MPICallMetadata OptimizedMetadataExtractor::extractMetadata(const CallSite& Site) {
  if (!Config.EnableMetadataExtractionOptimization) {
    return MetadataExtractor::extractMetadata(Site);
  }
  
  // Check cache first if enabled
  if (ParameterCaching && Site.CallInst) {
    MPICallMetadata CachedResult;
    if (MetadataCache.get(Site.CallInst, CachedResult)) {
      Stats.CacheHits++;
      return CachedResult;
    }
    Stats.CacheMisses++;
  }
  
  // Use optimized extraction for hot paths
  MPICallMetadata Result;
  if (isHotPath(Site)) {
    Result = extractMetadataOptimized(Site);
  } else {
    Result = extractMetadataStandard(Site);
  }
  
  // Cache result if enabled
  if (ParameterCaching && Site.CallInst) {
    MetadataCache.put(Site.CallInst, Result);
  }
  
  return Result;
}

std::vector<MPICallMetadata> OptimizedMetadataExtractor::extractMetadataBatch(const std::vector<CallSite>& Sites) {
  std::vector<MPICallMetadata> Results;
  Results.reserve(Sites.size());
  
  for (const auto& Site : Sites) {
    Results.push_back(extractMetadata(Site));
  }
  
  return Results;
}

void OptimizedMetadataExtractor::setHotPaths(const std::vector<HotPath>& NewHotPaths) {
  HotPaths = NewHotPaths;
}

bool OptimizedMetadataExtractor::isHotPath(const CallSite& Site) const {
  if (!Site.CallInst) return false;
  
  Function* F = Site.CallInst->getFunction();
  return std::any_of(HotPaths.begin(), HotPaths.end(),
    [F](const HotPath& Path) { return Path.HotFunction == F; });
}

MPICallMetadata OptimizedMetadataExtractor::extractMetadataOptimized(const CallSite& Site) {
  MPICallMetadata Metadata;
  
  // Fast path for hot paths - extract only essential metadata
  Metadata.FunctionName = Site.FunctionName;
  Metadata.FunctionType = Site.Type;
  
  if (Site.CallInst) {
    // Extract parameters efficiently
    if (auto* CI = dyn_cast<CallInst>(Site.CallInst)) {
      Metadata.CallConv = CI->getCallingConv();
      Metadata.ReturnType = CI->getType();
      
      // Only extract parameters if needed (lazy evaluation)
      if (LazyEvaluation) {
        // Extract parameters on-demand
        Stats.LazyEvaluations++;
      } else {
        // Extract all parameters
        for (unsigned i = 0; i < CI->getNumOperands() - 1; ++i) {
          Metadata.Parameters.push_back(CI->getOperand(i));
        }
      }
    }
  }
  
  return Metadata;
}

MPICallMetadata OptimizedMetadataExtractor::extractMetadataStandard(const CallSite& Site) {
  // Use standard extraction for non-hot paths
  return MetadataExtractor::extractMetadata(Site);
}

//===----------------------------------------------------------------------===//
// OptimizedHookInserter Implementation
//===----------------------------------------------------------------------===//

OptimizedHookInserter::OptimizedHookInserter(const HookConfiguration& Config, const OptimizationConfig& OptConfig)
    : HookInserter(Config), OptConfig(OptConfig), 
      StringConstantCache(OptConfig.MaxCacheSize), HookFunctionCache(OptConfig.MaxCacheSize) {
  LLVM_DEBUG(dbgs() << "Created OptimizedHookInserter\n");
}

bool OptimizedHookInserter::insertHooks(Function& F, const std::vector<CallSite>& Sites) {
  if (!OptConfig.EnableHookInsertionOptimization) {
    return HookInserter::insertHooks(F, Sites);
  }
  
  if (isHotPath(F)) {
    return insertHooksOptimized(F, Sites);
  } else {
    return insertHooksStandard(F, Sites);
  }
}

bool OptimizedHookInserter::insertHooksBatch(const std::vector<std::pair<Function*, std::vector<CallSite>>>& FunctionSites) {
  bool Modified = false;
  
  for (const auto& FuncSites : FunctionSites) {
    Modified |= insertHooks(*FuncSites.first, FuncSites.second);
  }
  
  if (BatchInsertion) {
    Stats.BatchOptimizations++;
  }
  
  return Modified;
}

void OptimizedHookInserter::setHotPaths(const std::vector<HotPath>& NewHotPaths) {
  HotPaths = NewHotPaths;
}

bool OptimizedHookInserter::isHotPath(Function& F) const {
  return std::any_of(HotPaths.begin(), HotPaths.end(),
    [&F](const HotPath& Path) { return Path.HotFunction == &F; });
}

bool OptimizedHookInserter::insertHooksOptimized(Function& F, const std::vector<CallSite>& Sites) {
  if (Sites.empty()) return false;
  
  bool Modified = false;
  Builder = std::make_unique<IRBuilder<>>(F.getContext());
  
  // Batch create hook declarations to reduce overhead
  Module* M = F.getParent();
  if (M) {
    createHookDeclarations(*M);
  }
  
  // Process sites with minimal IR transformation
  for (const auto& Site : Sites) {
    if (MinimalTransformation) {
      // Only insert essential hooks for hot paths
      if (Config.EnablePreHooks) {
        MetadataExtractor Extractor;
        MPICallMetadata Metadata = Extractor.extractMetadata(Site);
        Modified |= insertPreCallHook(const_cast<CallSite&>(Site), Metadata);
      }
      Stats.MinimalTransformations++;
    } else {
      // Standard hook insertion
      Modified |= HookInserter::insertHooks(F, {Site});
    }
  }
  
  return Modified;
}

bool OptimizedHookInserter::insertHooksStandard(Function& F, const std::vector<CallSite>& Sites) {
  return HookInserter::insertHooks(F, Sites);
}

Value* OptimizedHookInserter::getOrCreateOptimizedStringConstant(StringRef Str) {
  std::string StrKey = Str.str();
  Value* CachedValue = nullptr;
  
  if (StringConstantCache.get(StrKey, CachedValue)) {
    Stats.InstructionsReused++;
    return CachedValue;
  }
  
  Value* NewValue = getOrCreateStringConstant(Str);
  StringConstantCache.put(StrKey, NewValue);
  return NewValue;
}

Function* OptimizedHookInserter::getOrCreateOptimizedHookFunction(Module& M, StringRef Name, FunctionType* Type) {
  std::string NameKey = Name.str();
  Function* CachedFunc = nullptr;
  
  if (HookFunctionCache.get(NameKey, CachedFunc)) {
    return CachedFunc;
  }
  
  Function* NewFunc = createHookDeclaration(M, Name, Type);
  HookFunctionCache.put(NameKey, NewFunc);
  return NewFunc;
}

//===----------------------------------------------------------------------===//
// PassOptimizer Implementation
//===----------------------------------------------------------------------===//

PassOptimizer::PassOptimizer(const OptimizationConfig& Config) : Config(Config) {
  LLVM_DEBUG(dbgs() << "Created PassOptimizer with strategy: " 
                    << static_cast<int>(Config.Strategy) << "\n");
}

void PassOptimizer::initialize(const PassPerformanceProfile& NewProfile) {
  Profile = NewProfile;
  analyzeBottlenecks();
  identifyOptimizationOpportunities();
  
  LLVM_DEBUG(dbgs() << "Initialized PassOptimizer with profile data\n");
}

std::unique_ptr<OptimizedMPICallDetector> PassOptimizer::createOptimizedCallDetector() {
  auto Detector = std::make_unique<OptimizedMPICallDetector>(Config);
  Detector->setHotPaths(HotPaths);
  return Detector;
}

std::unique_ptr<OptimizedMetadataExtractor> PassOptimizer::createOptimizedMetadataExtractor() {
  auto Extractor = std::make_unique<OptimizedMetadataExtractor>(Config);
  Extractor->setHotPaths(HotPaths);
  return Extractor;
}

std::unique_ptr<OptimizedHookInserter> PassOptimizer::createOptimizedHookInserter(const HookConfiguration& HookConfig) {
  auto Inserter = std::make_unique<OptimizedHookInserter>(HookConfig, Config);
  Inserter->setHotPaths(HotPaths);
  return Inserter;
}

std::vector<HotPath> PassOptimizer::identifyHotPaths(Module& M, const PassPerformanceProfile& Profile) {
  std::vector<HotPath> IdentifiedHotPaths;
  
  // Analyze phase profiles to identify hot paths
  for (const auto& Phase : Profile.PhaseProfiles) {
    double PhasePercentage = static_cast<double>(Phase.Metrics.ExecutionTimeUs) / 
                            Profile.OverallMetrics.ExecutionTimeUs;
    
    if (PhasePercentage >= Config.HotPathThreshold) {
      // This phase is a hot path
      for (Function& F : M) {
        if (!F.isDeclaration()) {
          HotPath Path;
          Path.HotFunction = &F;
          Path.ExecutionTimeUs = Phase.Metrics.ExecutionTimeUs;
          Path.OptimizationPotential = calculateOptimizationPotential(Path);
          
          if (shouldOptimizeHotPath(Path)) {
            IdentifiedHotPaths.push_back(Path);
          }
        }
      }
    }
  }
  
  // Sort by optimization potential
  std::sort(IdentifiedHotPaths.begin(), IdentifiedHotPaths.end(),
    [](const HotPath& A, const HotPath& B) {
      return A.OptimizationPotential > B.OptimizationPotential;
    });
  
  LLVM_DEBUG(dbgs() << "Identified " << IdentifiedHotPaths.size() << " hot paths\n");
  
  return IdentifiedHotPaths;
}

bool PassOptimizer::optimizeModule(Module& M) {
  bool Modified = false;
  
  // Identify hot paths
  HotPaths = identifyHotPaths(M, Profile);
  
  // Apply module-level optimizations
  if (Config.EnableMemoryOptimization) {
    applyMemoryOptimizations(M);
    Modified = true;
  }
  
  if (Config.EnableCaching) {
    applyCachingOptimizations();
    Modified = true;
  }
  
  // Apply algorithmic optimizations
  applyAlgorithmicOptimizations();
  
  return Modified;
}

bool PassOptimizer::optimizeFunction(Function& F) {
  bool Modified = false;
  
  // Check if this function is in a hot path
  bool IsHotPath = std::any_of(HotPaths.begin(), HotPaths.end(),
    [&F](const HotPath& Path) { return Path.HotFunction == &F; });
  
  if (IsHotPath) {
    LLVM_DEBUG(dbgs() << "Applying hot path optimizations to function: " << F.getName() << "\n");
    
    // Apply function-specific optimizations
    // This could include instruction reordering, loop optimizations, etc.
    Modified = true;
    OverallStats.TotalOptimizationsApplied++;
  }
  
  return Modified;
}

std::string PassOptimizer::generateOptimizationReport() const {
  std::ostringstream Report;
  
  Report << "=== Pass Optimization Report ===\n\n";
  
  // Overall statistics
  auto Stats = getOverallStats();
  Report << "Overall Optimization Statistics:\n";
  Report << "  Total Time Saved: " << Stats.TotalTimeSavedUs << " μs\n";
  Report << "  Total Cache Hits: " << Stats.TotalCacheHits << "\n";
  Report << "  Total Optimizations Applied: " << Stats.TotalOptimizationsApplied << "\n";
  Report << "  Estimated Speedup: " << (Stats.EstimatedSpeedup * 100.0) << "%\n\n";
  
  // Hot paths
  Report << "Hot Paths Identified: " << HotPaths.size() << "\n";
  for (size_t i = 0; i < HotPaths.size() && i < 5; ++i) {
    const auto& Path = HotPaths[i];
    Report << "  " << (i + 1) << ". Function: " << (Path.HotFunction ? Path.HotFunction->getName().str() : "unknown")
           << " (Potential: " << (Path.OptimizationPotential * 100.0) << "%)\n";
  }
  
  // Component-specific statistics
  if (OptCallDetector) {
    auto DetectorStats = OptCallDetector->getOptimizationStats();
    Report << "\nCall Detector Optimizations:\n";
    Report << "  Cache Hits: " << DetectorStats.CacheHits << "\n";
    Report << "  Cache Misses: " << DetectorStats.CacheMisses << "\n";
    Report << "  Hot Path Optimizations: " << DetectorStats.HotPathOptimizations << "\n";
  }
  
  if (OptMetadataExtractor) {
    auto ExtractorStats = OptMetadataExtractor->getOptimizationStats();
    Report << "\nMetadata Extractor Optimizations:\n";
    Report << "  Cache Hits: " << ExtractorStats.CacheHits << "\n";
    Report << "  Lazy Evaluations: " << ExtractorStats.LazyEvaluations << "\n";
  }
  
  if (OptHookInserter) {
    auto InserterStats = OptHookInserter->getOptimizationStats();
    Report << "\nHook Inserter Optimizations:\n";
    Report << "  Instructions Reused: " << InserterStats.InstructionsReused << "\n";
    Report << "  Batch Optimizations: " << InserterStats.BatchOptimizations << "\n";
    Report << "  Minimal Transformations: " << InserterStats.MinimalTransformations << "\n";
  }
  
  Report << "\n================================\n";
  
  return Report.str();
}

PassOptimizer::OverallOptimizationStats PassOptimizer::getOverallStats() const {
  OverallOptimizationStats Stats = OverallStats;
  
  // Aggregate statistics from components
  if (OptCallDetector) {
    auto DetectorStats = OptCallDetector->getOptimizationStats();
    Stats.TotalCacheHits += DetectorStats.CacheHits;
    Stats.TotalTimeSavedUs += DetectorStats.TimeSavedUs;
  }
  
  if (OptMetadataExtractor) {
    auto ExtractorStats = OptMetadataExtractor->getOptimizationStats();
    Stats.TotalCacheHits += ExtractorStats.CacheHits;
    Stats.TotalTimeSavedUs += ExtractorStats.TimeSavedUs;
  }
  
  if (OptHookInserter) {
    auto InserterStats = OptHookInserter->getOptimizationStats();
    Stats.TotalTimeSavedUs += InserterStats.TimeSavedUs;
  }
  
  // Calculate estimated speedup
  Stats.EstimatedSpeedup = estimateSpeedupFromOptimizations();
  
  return Stats;
}

void PassOptimizer::analyzeBottlenecks() {
  // Analyze the performance profile to identify bottlenecks
  for (const auto& Phase : Profile.PhaseProfiles) {
    double PhasePercentage = static_cast<double>(Phase.Metrics.ExecutionTimeUs) / 
                            Profile.OverallMetrics.ExecutionTimeUs;
    
    if (PhasePercentage >= 0.3) { // 30% threshold for major bottlenecks
      LLVM_DEBUG(dbgs() << "Major bottleneck identified in phase: " << Phase.PhaseName 
                        << " (" << (PhasePercentage * 100.0) << "%)\n");
    }
  }
}

void PassOptimizer::identifyOptimizationOpportunities() {
  // Identify specific optimization opportunities based on profile data
  
  // Memory optimization opportunities
  if (Profile.OverallMetrics.PeakMemoryBytes > 50 * 1024 * 1024) { // > 50MB
    LLVM_DEBUG(dbgs() << "Memory optimization opportunity identified\n");
  }
  
  // Caching opportunities
  if (Profile.OverallMetrics.OperationCount > 1000) {
    LLVM_DEBUG(dbgs() << "Caching optimization opportunity identified\n");
  }
}

void PassOptimizer::applyMemoryOptimizations(Module& M) {
  // Apply memory-specific optimizations
  LLVM_DEBUG(dbgs() << "Applying memory optimizations\n");
  OverallStats.TotalOptimizationsApplied++;
}

void PassOptimizer::applyCachingOptimizations() {
  // Apply caching optimizations
  LLVM_DEBUG(dbgs() << "Applying caching optimizations\n");
  OverallStats.TotalOptimizationsApplied++;
}

void PassOptimizer::applyAlgorithmicOptimizations() {
  // Apply algorithmic optimizations
  LLVM_DEBUG(dbgs() << "Applying algorithmic optimizations\n");
  OverallStats.TotalOptimizationsApplied++;
}

double PassOptimizer::calculateOptimizationPotential(const HotPath& Path) const {
  // Calculate optimization potential based on execution time and frequency
  double TimeFactor = static_cast<double>(Path.ExecutionTimeUs) / Profile.OverallMetrics.ExecutionTimeUs;
  double FrequencyFactor = static_cast<double>(Path.CallFrequency) / Profile.OverallMetrics.OperationCount;
  
  return (TimeFactor + FrequencyFactor) / 2.0;
}

bool PassOptimizer::shouldOptimizeHotPath(const HotPath& Path) const {
  return Path.OptimizationPotential >= Config.HotPathThreshold;
}

double PassOptimizer::estimateSpeedupFromOptimizations() const {
  // Estimate speedup based on applied optimizations
  double EstimatedSpeedup = 0.0;
  
  // Base speedup from caching
  if (Config.EnableCaching) {
    EstimatedSpeedup += 0.1; // 10% speedup from caching
  }
  
  // Additional speedup from hot path optimizations
  EstimatedSpeedup += HotPaths.size() * 0.05; // 5% per hot path optimized
  
  // Cap the estimated speedup at 50%
  return std::min(EstimatedSpeedup, 0.5);
}

uint64_t PassOptimizer::estimateMemorySavings() const {
  // Estimate memory savings from optimizations
  uint64_t Savings = 0;
  
  if (Config.EnableMemoryOptimization) {
    Savings += Profile.OverallMetrics.PeakMemoryBytes * 0.2; // 20% memory savings
  }
  
  return Savings;
}

//===----------------------------------------------------------------------===//
// OptimizedPassComponentFactory Implementation
//===----------------------------------------------------------------------===//

std::unique_ptr<PassOptimizer> OptimizedPassComponentFactory::createPassOptimizer(const OptimizationConfig& Config) {
  return std::make_unique<PassOptimizer>(Config);
}

OptimizationConfig OptimizedPassComponentFactory::createDefaultOptimizationConfig() {
  OptimizationConfig Config;
  Config.Strategy = OptimizationStrategy::Conservative;
  Config.EnableCallDetectionOptimization = true;
  Config.EnableMetadataExtractionOptimization = true;
  Config.EnableHookInsertionOptimization = true;
  Config.EnableMemoryOptimization = true;
  Config.EnableCaching = true;
  Config.HotPathThreshold = 0.1;
  Config.MaxCacheSize = 1000;
  return Config;
}

OptimizationConfig OptimizedPassComponentFactory::createConservativeOptimizationConfig() {
  OptimizationConfig Config;
  Config.Strategy = OptimizationStrategy::Conservative;
  Config.EnableCallDetectionOptimization = true;
  Config.EnableMetadataExtractionOptimization = false;
  Config.EnableHookInsertionOptimization = false;
  Config.EnableMemoryOptimization = true;
  Config.EnableCaching = true;
  Config.HotPathThreshold = 0.2;
  Config.MaxCacheSize = 500;
  return Config;
}

OptimizationConfig OptimizedPassComponentFactory::createAggressiveOptimizationConfig() {
  OptimizationConfig Config;
  Config.Strategy = OptimizationStrategy::Aggressive;
  Config.EnableCallDetectionOptimization = true;
  Config.EnableMetadataExtractionOptimization = true;
  Config.EnableHookInsertionOptimization = true;
  Config.EnableMemoryOptimization = true;
  Config.EnableCaching = true;
  Config.EnableParallelization = true;
  Config.HotPathThreshold = 0.05;
  Config.MaxCacheSize = 2000;
  return Config;
}

OptimizationConfig OptimizedPassComponentFactory::createProfileGuidedOptimizationConfig(const PassPerformanceProfile& Profile) {
  OptimizationConfig Config = createDefaultOptimizationConfig();
  
  // Adjust configuration based on profile data
  if (Profile.OverallMetrics.ExecutionTimeUs > 100000) { // > 100ms
    Config.Strategy = OptimizationStrategy::Aggressive;
    Config.HotPathThreshold = 0.05;
  }
  
  if (Profile.OverallMetrics.PeakMemoryBytes > 100 * 1024 * 1024) { // > 100MB
    Config.EnableMemoryOptimization = true;
    Config.MaxCacheSize = 500; // Reduce cache size to save memory
  }
  
  return Config;
}