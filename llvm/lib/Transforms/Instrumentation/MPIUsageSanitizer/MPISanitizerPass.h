//===- MPISanitizerPass.h - MPI Usage Sanitizer Pass ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the main MPI Usage Sanitizer LLVM Pass that orchestrates
// the instrumentation of MPI programs for runtime error detection and
// performance monitoring.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPISANITIZERPASS_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPISANITIZERPASS_H

#include "ConfigurationManager.h"
#include "MPICallDetector.h"
#include "MetadataExtractor.h"
#include "HookInserter.h"
#include "StaticAnalyzer.h"
#include "OptimizationEngine.h"
#include "ErrorHandler.h"
#include "RuntimeInterfaceValidator.h"
#include "PerformanceProfiler.h"
#include "PassOptimizer.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include <memory>

namespace llvm {

class Module;
class Function;

/// MPI Usage Sanitizer Pass
///
/// Main LLVM transformation pass that instruments MPI programs for runtime
/// error detection and performance monitoring. Integrates all components
/// of the MPI sanitizer system.
class MPISanitizerPass : public PassInfoMixin<MPISanitizerPass> {
public:
  MPISanitizerPass();
  explicit MPISanitizerPass(const PassConfiguration& Config);
  
  /// Run the pass on a module
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM);
  
  /// Get pass name for debugging
  static StringRef name() { return "MPISanitizerPass"; }
  
  /// Set configuration for the pass
  void setConfiguration(const PassConfiguration& Config);
  
  /// Get current configuration
  const PassConfiguration& getConfiguration() const;

private:
  /// Initialize all pass components
  void initializeComponents();
  
  /// Run MPI call detection phase
  bool runCallDetection(Module& M);
  
  /// Run metadata extraction phase
  bool runMetadataExtraction(Module& M);
  
  /// Run static analysis phase
  bool runStaticAnalysis(Module& M);
  
  /// Run optimization phase
  bool runOptimization(Module& M);
  
  /// Run instrumentation phase
  bool runInstrumentation(Module& M);
  
  /// Validate runtime interface
  bool validateRuntimeInterface(Module& M);
  
  /// Generate instrumentation report
  void generateReport(Module& M);

private:
  /// Pass configuration
  PassConfiguration Config;
  
  /// Pass components
  std::unique_ptr<ConfigurationManager> ConfigManager;
  std::unique_ptr<MPICallDetector> CallDetector;
  std::unique_ptr<MetadataExtractor> MetaExtractor;
  std::unique_ptr<HookInserter> HInserter;
  std::unique_ptr<StaticAnalyzer> SAnalyzer;
  std::unique_ptr<OptimizationEngine> OptEngine;
  std::unique_ptr<ErrorHandler> ErrHandler;
  std::unique_ptr<RuntimeInterfaceValidator> RuntimeValidator;
  
  /// Performance profiling and optimization
  std::unique_ptr<PerformanceProfiler> Profiler;
  std::unique_ptr<PassOptimizer> Optimizer;
  PassPerformanceProfile PerformanceProfile;
  
  /// Detected MPI calls
  std::vector<CallSite> DetectedCalls;
  
  /// Extracted metadata
  std::vector<MPICallMetadata> ExtractedMetadata;
  
  /// Analysis results
  std::vector<AnalysisResult> AnalysisResults;
  
  /// Optimization decisions
  std::vector<OptimizationDecision> OptimizationDecisions;
  
  /// Pass statistics
  struct PassStatistics {
    uint32_t MPICallsDetected = 0;
    uint32_t MPICallsInstrumented = 0;
    uint32_t HooksInserted = 0;
    uint32_t OptimizationsApplied = 0;
    uint32_t ErrorsDetected = 0;
    uint64_t ExecutionTimeUs = 0;
  } Statistics;
};

/// Legacy pass wrapper for old pass manager
class MPISanitizerLegacyPass : public ModulePass {
public:
  static char ID;
  
  MPISanitizerLegacyPass();
  explicit MPISanitizerLegacyPass(const PassConfiguration& Config);
  
  bool runOnModule(Module &M) override;
  
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  
  StringRef getPassName() const override {
    return "MPI Usage Sanitizer";
  }

private:
  MPISanitizerPass Impl;
};

/// Pass registration functions
ModulePass *createMPISanitizerLegacyPass();
ModulePass *createMPISanitizerLegacyPass(const PassConfiguration& Config);

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPISANITIZERPASS_H