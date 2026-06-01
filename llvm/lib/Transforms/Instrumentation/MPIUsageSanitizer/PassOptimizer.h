//===- PassOptimizer.h - MPI Sanitizer Pass Optimizer ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the PassOptimizer class which implements performance
// optimizations for the MPI Usage Sanitizer Pass based on profiling data
// and static analysis.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_PASSOPTIMIZER_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_PASSOPTIMIZER_H

#include "PerformanceProfiler.h"
#include "MPICallDetector.h"
#include "MetadataExtractor.h"
#include "HookInserter.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/IR/Module.h"
#include <memory>
#include <vector>

namespace llvm {

class Function;
class BasicBlock;
class Instruction;

/// Optimization strategy for different pass components
enum class OptimizationStrategy {
  None,           ///< No optimization
  Conservative,   ///< Safe optimizations only
  Aggressive,     ///< More aggressive optimizations
  Experimental    ///< Experimental optimizations
};

/// Hot path information for optimization targeting
struct HotPath {
  Function* HotFunction = nullptr;
  BasicBlock* HotBasicBlock = nullptr;
  std::vector<Instruction*> HotInstructions;
  uint64_t ExecutionTimeUs = 0;
  uint32_t CallFrequency = 0;
  double OptimizationPotential = 0.0;
};

/// Optimization configuration
struct OptimizationConfig {
  OptimizationStrategy Strategy = OptimizationStrategy::Conservative;
  bool EnableCallDetectionOptimization = true;
  bool EnableMetadataExtractionOptimization = true;
  bool EnableHookInsertionOptimization = true;
  bool EnableMemoryOptimization = true;
  bool EnableCaching = true;
  bool EnableParallelization = false;
  double HotPathThreshold = 0.1; ///< 10% of total execution time
  uint32_t MaxCacheSize = 1000;
};

/// Cache for expensive computations
template<typename Key, typename Value>
class OptimizationCache {
public:
  OptimizationCache(uint32_t MaxSize) : MaxSize(MaxSize) {}
  
  bool get(const Key& K, Value& V) const {
    auto It = Cache.find(K);
    if (It != Cache.end()) {
      V = It->second;
      return true;
    }
    return false;
  }
  
  void put(const Key& K, const Value& V) {
    if (Cache.size() >= MaxSize) {
      // Simple LRU: remove first element
      Cache.erase(Cache.begin());
    }
    Cache[K] = V;
  }
  
  void clear() { Cache.clear(); }
  size_t size() const { return Cache.size(); }

private:
  DenseMap<Key, Value> Cache;
  uint32_t MaxSize;
};

/// Optimized MPI call detector with caching and hot path optimization
class OptimizedMPICallDetector : public MPICallDetector {
public:
  OptimizedMPICallDetector(const OptimizationConfig& Config);
  ~OptimizedMPICallDetector() = default;
  
  /// Optimized MPI call detection with caching
  std::vector<CallSite> detectMPICalls(Function& F) override;
  
  /// Set hot paths for optimization targeting
  void setHotPaths(const std::vector<HotPath>& HotPaths);
  
  /// Enable/disable specific optimizations
  void enableFunctionNameCaching(bool Enable) { FunctionNameCaching = Enable; }
  void enableSignatureCaching(bool Enable) { SignatureCaching = Enable; }
  void enableHotPathOptimization(bool Enable) { HotPathOptimization = Enable; }
  
  /// Get optimization statistics
  struct OptimizationStats {
    uint32_t CacheHits = 0;
    uint32_t CacheMisses = 0;
    uint32_t HotPathOptimizations = 0;
    uint64_t TimeSavedUs = 0;
  };
  
  OptimizationStats getOptimizationStats() const { return Stats; }
  void resetOptimizationStats() { Stats = OptimizationStats(); }

private:
  OptimizationConfig Config;
  std::vector<HotPath> HotPaths;
  
  /// Optimization flags
  bool FunctionNameCaching = true;
  bool SignatureCaching = true;
  bool HotPathOptimization = true;
  
  /// Caches for expensive operations
  OptimizationCache<std::string, bool> FunctionNameCache;
  OptimizationCache<std::string, MPIFunctionType> SignatureCache;
  OptimizationCache<Function*, std::vector<CallSite>> FunctionCallCache;
  
  /// Optimization statistics
  mutable OptimizationStats Stats;
  
  /// Optimized helper methods
  bool isHotPath(Function& F) const;
  std::vector<CallSite> detectMPICallsOptimized(Function& F);
  std::vector<CallSite> detectMPICallsStandard(Function& F);
};

/// Optimized metadata extractor with reduced overhead
class OptimizedMetadataExtractor : public MetadataExtractor {
public:
  OptimizedMetadataExtractor(const OptimizationConfig& Config);
  ~OptimizedMetadataExtractor() = default;
  
  /// Optimized metadata extraction
  MPICallMetadata extractMetadata(const CallSite& Site) override;
  
  /// Batch metadata extraction for multiple call sites
  std::vector<MPICallMetadata> extractMetadataBatch(const std::vector<CallSite>& Sites);
  
  /// Set hot paths for optimization targeting
  void setHotPaths(const std::vector<HotPath>& HotPaths);
  
  /// Enable/disable specific optimizations
  void enableParameterCaching(bool Enable) { ParameterCaching = Enable; }
  void enableLazyEvaluation(bool Enable) { LazyEvaluation = Enable; }
  
  /// Get optimization statistics
  struct OptimizationStats {
    uint32_t CacheHits = 0;
    uint32_t CacheMisses = 0;
    uint32_t LazyEvaluations = 0;
    uint64_t TimeSavedUs = 0;
  };
  
  OptimizationStats getOptimizationStats() const { return Stats; }

private:
  OptimizationConfig Config;
  std::vector<HotPath> HotPaths;
  
  /// Optimization flags
  bool ParameterCaching = true;
  bool LazyEvaluation = true;
  
  /// Caches for expensive operations
  OptimizationCache<Instruction*, MPICallMetadata> MetadataCache;
  OptimizationCache<Value*, ParameterInfo> ParameterCache;
  
  /// Optimization statistics
  mutable OptimizationStats Stats;
  
  /// Optimized helper methods
  bool isHotPath(const CallSite& Site) const;
  MPICallMetadata extractMetadataOptimized(const CallSite& Site);
  MPICallMetadata extractMetadataStandard(const CallSite& Site);
};

/// Optimized hook inserter with minimal IR transformation overhead
class OptimizedHookInserter : public HookInserter {
public:
  OptimizedHookInserter(const HookConfiguration& Config, const OptimizationConfig& OptConfig);
  ~OptimizedHookInserter() = default;
  
  /// Optimized hook insertion
  bool insertHooks(Function& F, const std::vector<CallSite>& Sites) override;
  
  /// Batch hook insertion for multiple functions
  bool insertHooksBatch(const std::vector<std::pair<Function*, std::vector<CallSite>>>& FunctionSites);
  
  /// Set hot paths for optimization targeting
  void setHotPaths(const std::vector<HotPath>& HotPaths);
  
  /// Enable/disable specific optimizations
  void enableInstructionReuse(bool Enable) { InstructionReuse = Enable; }
  void enableBatchInsertion(bool Enable) { BatchInsertion = Enable; }
  void enableMinimalTransformation(bool Enable) { MinimalTransformation = Enable; }
  
  /// Get optimization statistics
  struct OptimizationStats {
    uint32_t InstructionsReused = 0;
    uint32_t BatchOptimizations = 0;
    uint32_t MinimalTransformations = 0;
    uint64_t TimeSavedUs = 0;
  };
  
  OptimizationStats getOptimizationStats() const { return Stats; }

private:
  OptimizationConfig OptConfig;
  std::vector<HotPath> HotPaths;
  
  /// Optimization flags
  bool InstructionReuse = true;
  bool BatchInsertion = true;
  bool MinimalTransformation = true;
  
  /// Caches for reusable instructions and values
  OptimizationCache<std::string, Value*> StringConstantCache;
  OptimizationCache<std::string, Function*> HookFunctionCache;
  DenseMap<BasicBlock*, std::vector<Instruction*>> ReusableInstructions;
  
  /// Optimization statistics
  mutable OptimizationStats Stats;
  
  /// Optimized helper methods
  bool isHotPath(Function& F) const;
  bool insertHooksOptimized(Function& F, const std::vector<CallSite>& Sites);
  bool insertHooksStandard(Function& F, const std::vector<CallSite>& Sites);
  Value* getOrCreateOptimizedStringConstant(StringRef Str);
  Function* getOrCreateOptimizedHookFunction(Module& M, StringRef Name, FunctionType* Type);
};

/// Main pass optimizer that coordinates all optimization strategies
class PassOptimizer {
public:
  PassOptimizer(const OptimizationConfig& Config);
  ~PassOptimizer() = default;
  
  /// Initialize optimizer with profiling data
  void initialize(const PassPerformanceProfile& Profile);
  
  /// Create optimized pass components
  std::unique_ptr<OptimizedMPICallDetector> createOptimizedCallDetector();
  std::unique_ptr<OptimizedMetadataExtractor> createOptimizedMetadataExtractor();
  std::unique_ptr<OptimizedHookInserter> createOptimizedHookInserter(const HookConfiguration& HookConfig);
  
  /// Analyze and identify hot paths from profiling data
  std::vector<HotPath> identifyHotPaths(Module& M, const PassPerformanceProfile& Profile);
  
  /// Apply module-level optimizations
  bool optimizeModule(Module& M);
  
  /// Apply function-level optimizations
  bool optimizeFunction(Function& F);
  
  /// Generate optimization report
  std::string generateOptimizationReport() const;
  
  /// Get overall optimization statistics
  struct OverallOptimizationStats {
    uint64_t TotalTimeSavedUs = 0;
    uint32_t TotalCacheHits = 0;
    uint32_t TotalOptimizationsApplied = 0;
    double EstimatedSpeedup = 0.0;
  };
  
  OverallOptimizationStats getOverallStats() const;

private:
  OptimizationConfig Config;
  PassPerformanceProfile Profile;
  std::vector<HotPath> HotPaths;
  
  /// Optimized components
  std::unique_ptr<OptimizedMPICallDetector> OptCallDetector;
  std::unique_ptr<OptimizedMetadataExtractor> OptMetadataExtractor;
  std::unique_ptr<OptimizedHookInserter> OptHookInserter;
  
  /// Optimization statistics
  mutable OverallOptimizationStats OverallStats;
  
  /// Helper methods
  void analyzeBottlenecks();
  void identifyOptimizationOpportunities();
  void applyMemoryOptimizations(Module& M);
  void applyCachingOptimizations();
  void applyAlgorithmicOptimizations();
  
  /// Hot path analysis
  double calculateOptimizationPotential(const HotPath& Path) const;
  bool shouldOptimizeHotPath(const HotPath& Path) const;
  
  /// Performance prediction
  double estimateSpeedupFromOptimizations() const;
  uint64_t estimateMemorySavings() const;
};

/// Factory for creating optimized pass components
class OptimizedPassComponentFactory {
public:
  static std::unique_ptr<PassOptimizer> createPassOptimizer(const OptimizationConfig& Config);
  
  static OptimizationConfig createDefaultOptimizationConfig();
  static OptimizationConfig createConservativeOptimizationConfig();
  static OptimizationConfig createAggressiveOptimizationConfig();
  
  /// Create optimization config from profiling data
  static OptimizationConfig createProfileGuidedOptimizationConfig(const PassPerformanceProfile& Profile);
};

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_PASSOPTIMIZER_H