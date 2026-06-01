//===- MPISanitizerPass.cpp - MPI Usage Sanitizer Pass --------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the main MPI Usage Sanitizer LLVM Pass that orchestrates
// the instrumentation of MPI programs for runtime error detection and
// performance monitoring.
//
//===----------------------------------------------------------------------===//

#include "MPISanitizerPass.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Timer.h"
#include "PerformanceProfiler.h"
#include "PassOptimizer.h"
#include <chrono>

#define DEBUG_TYPE "mpi-sanitizer"

namespace llvm {

//===----------------------------------------------------------------------===//
// MPISanitizerPass Implementation
//===----------------------------------------------------------------------===//

MPISanitizerPass::MPISanitizerPass() {
  initializeComponents();
}

MPISanitizerPass::MPISanitizerPass(const PassConfiguration& Config) : Config(Config) {
  initializeComponents();
}

PreservedAnalyses MPISanitizerPass::run(Module &M, ModuleAnalysisManager &AM) {
  auto StartTime = std::chrono::high_resolution_clock::now();
  
  LLVM_DEBUG(dbgs() << "Running MPI Sanitizer Pass on module: " << M.getName() << "\n");
  
  // Initialize profiling if enabled
  if (Config.EnableProfiling) {
    Profiler = std::make_unique<PerformanceProfiler>();
    Profiler->startPassProfiling(M);
  }
  
  // Initialize configuration if not already done
  if (ConfigManager) {
    ConfigManager->initialize(Config);
  }
  
  bool ModuleChanged = false;
  
  try {
    // Phase 1: MPI Call Detection
    {
      PROFILE_PHASE(*Profiler, "MPI Call Detection");
      if (runCallDetection(M)) {
        LLVM_DEBUG(dbgs() << "Detected " << DetectedCalls.size() << " MPI calls\n");
        Statistics.MPICallsDetected = DetectedCalls.size();
        if (Profiler) Profiler->recordOperation("MPI Call Detection", DetectedCalls.size());
      }
    }
    
    // Phase 2: Metadata Extraction
    {
      PROFILE_PHASE(*Profiler, "Metadata Extraction");
      if (!DetectedCalls.empty() && runMetadataExtraction(M)) {
        LLVM_DEBUG(dbgs() << "Extracted metadata for " << ExtractedMetadata.size() << " calls\n");
        if (Profiler) Profiler->recordOperation("Metadata Extraction", ExtractedMetadata.size());
      }
    }
    
    // Phase 3: Static Analysis
    {
      PROFILE_PHASE(*Profiler, "Static Analysis");
      if (!ExtractedMetadata.empty() && runStaticAnalysis(M)) {
        LLVM_DEBUG(dbgs() << "Completed static analysis for " << AnalysisResults.size() << " calls\n");
        if (Profiler) Profiler->recordOperation("Static Analysis", AnalysisResults.size());
      }
    }
    
    // Phase 4: Optimization
    {
      PROFILE_PHASE(*Profiler, "Optimization");
      if (!AnalysisResults.empty() && runOptimization(M)) {
        LLVM_DEBUG(dbgs() << "Generated " << OptimizationDecisions.size() << " optimization decisions\n");
        if (Profiler) Profiler->recordOperation("Optimization", OptimizationDecisions.size());
      }
    }
    
    // Phase 5: Instrumentation
    {
      PROFILE_PHASE(*Profiler, "Instrumentation");
      if (!OptimizationDecisions.empty() && runInstrumentation(M)) {
        LLVM_DEBUG(dbgs() << "Applied instrumentation to module\n");
        ModuleChanged = true;
        if (Profiler) Profiler->recordOperation("Instrumentation", Statistics.HooksInserted);
      }
    }
    
    // Phase 6: Runtime Interface Validation
    {
      PROFILE_PHASE(*Profiler, "Runtime Validation");
      if (ModuleChanged && !validateRuntimeInterface(M)) {
        LLVM_DEBUG(dbgs() << "Runtime interface validation failed\n");
      }
    }
    
    // Generate performance profile and apply optimizations for future runs
    if (Config.EnableProfiling && Profiler) {
      PerformanceProfile = Profiler->endPassProfiling();
      
      if (Config.EnableOptimization) {
        // Create optimizer and apply optimizations
        OptimizationConfig OptConfig = OptimizedPassComponentFactory::createProfileGuidedOptimizationConfig(PerformanceProfile);
        Optimizer = OptimizedPassComponentFactory::createPassOptimizer(OptConfig);
        Optimizer->initialize(PerformanceProfile);
        
        // Apply module-level optimizations
        if (Optimizer->optimizeModule(M)) {
          ModuleChanged = true;
          LLVM_DEBUG(dbgs() << "Applied performance optimizations to module\n");
        }
      }
    }
    
    // Generate report if requested
    if (Config.GenerateReport) {
      generateReport(M);
    }
    
  } catch (const std::exception& E) {
    if (ErrHandler) {
      ErrHandler->reportError(ErrorCategory::InternalError, ErrorLevel::Error,
                               "Exception in MPI Sanitizer Pass: " + std::string(E.what()),
                               DebugLoc());
    }
  }
  
  auto EndTime = std::chrono::high_resolution_clock::now();
  Statistics.ExecutionTimeUs = std::chrono::duration_cast<std::chrono::microseconds>(EndTime - StartTime).count();
  
  LLVM_DEBUG(dbgs() << "MPI Sanitizer Pass completed in " << Statistics.ExecutionTimeUs << " μs\n");
  
  return ModuleChanged ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

void MPISanitizerPass::setConfiguration(const PassConfiguration& NewConfig) {
  Config = NewConfig;
  if (ConfigManager) {
    ConfigManager->initialize(Config);
  }
}

const PassConfiguration& MPISanitizerPass::getConfiguration() const {
  return Config;
}

void MPISanitizerPass::initializeComponents() {
  ConfigManager = std::make_unique<ConfigurationManager>();
  CallDetector = std::make_unique<MPICallDetector>();
  MetaExtractor = std::make_unique<MetadataExtractor>();
  HInserter = std::make_unique<HookInserter>();
  SAnalyzer = std::make_unique<StaticAnalyzer>();
  OptEngine = std::make_unique<OptimizationEngine>();
  ErrHandler = std::make_unique<ErrorHandler>();
  RuntimeValidator = std::make_unique<RuntimeInterfaceValidator>();
}

bool MPISanitizerPass::runCallDetection(Module& M) {
  if (!CallDetector) return false;
  
  try {
    DetectedCalls = CallDetector->detectMPICalls(M);
    return true;
  } catch (const std::exception& E) {
    if (ErrHandler) {
      ErrHandler->reportError(ErrorCategory::CallDetection, ErrorLevel::Error,
                               "Call detection failed: " + std::string(E.what()),
                               DebugLoc());
    }
    return false;
  }
}

bool MPISanitizerPass::runMetadataExtraction(Module& M) {
  if (!MetaExtractor || DetectedCalls.empty()) return false;
  
  try {
    ExtractedMetadata.clear();
    ExtractedMetadata.reserve(DetectedCalls.size());
    
    for (const auto& Call : DetectedCalls) {
      if (Call.CallInst) {
        MPICallMetadata Metadata = MetaExtractor->extractMetadata(*Call.CallInst);
        ExtractedMetadata.push_back(Metadata);
      }
    }
    
    return !ExtractedMetadata.empty();
  } catch (const std::exception& E) {
    if (ErrHandler) {
      ErrHandler->reportError(ErrorCategory::MetadataExtraction, ErrorLevel::Error,
                               "Metadata extraction failed: " + std::string(E.what()),
                               DebugLoc());
    }
    return false;
  }
}

bool MPISanitizerPass::runStaticAnalysis(Module& M) {
  if (!SAnalyzer || ExtractedMetadata.empty()) return false;
  
  try {
    AnalysisResults.clear();
    AnalysisResults.reserve(ExtractedMetadata.size());
    
    for (size_t i = 0; i < DetectedCalls.size() && i < ExtractedMetadata.size(); ++i) {
      AnalysisResult Result = SAnalyzer->analyzeCall(DetectedCalls[i], ExtractedMetadata[i]);
      AnalysisResults.push_back(Result);
    }
    
    return !AnalysisResults.empty();
  } catch (const std::exception& E) {
    if (ErrHandler) {
      ErrHandler->reportError(ErrorCategory::StaticAnalysis, ErrorLevel::Error,
                               "Static analysis failed: " + std::string(E.what()),
                               DebugLoc());
    }
    return false;
  }
}

bool MPISanitizerPass::runOptimization(Module& M) {
  if (!OptEngine || AnalysisResults.empty()) return false;
  
  try {
    OptimizationDecisions.clear();
    OptimizationDecisions.reserve(AnalysisResults.size());
    
    for (size_t i = 0; i < DetectedCalls.size() && i < ExtractedMetadata.size() && i < AnalysisResults.size(); ++i) {
      OptimizationDecision Decision = OptEngine->makeDecision(DetectedCalls[i], 
                                                                      ExtractedMetadata[i], 
                                                                      AnalysisResults[i]);
      OptimizationDecisions.push_back(Decision);
    }
    
    return !OptimizationDecisions.empty();
  } catch (const std::exception& E) {
    if (ErrHandler) {
      ErrHandler->reportError(ErrorCategory::Optimization, ErrorLevel::Error,
                               "Optimization failed: " + std::string(E.what()),
                               DebugLoc());
    }
    return false;
  }
}

bool MPISanitizerPass::runInstrumentation(Module& M) {
  if (!HInserter || OptimizationDecisions.empty()) return false;
  
  try {
    uint32_t HooksInserted = 0;
    uint32_t CallsInstrumented = 0;
    
    for (size_t i = 0; i < DetectedCalls.size() && i < ExtractedMetadata.size() && i < OptimizationDecisions.size(); ++i) {
      if (OptimizationDecisions[i].ShouldInstrument) {
        bool Success = HInserter->insertHooks(DetectedCalls[i], ExtractedMetadata[i], OptimizationDecisions[i]);
        if (Success) {
          CallsInstrumented++;
          // Count hooks based on decision
          if (OptimizationDecisions[i].EnablePreHooks) HooksInserted++;
          if (OptimizationDecisions[i].EnablePostHooks) HooksInserted++;
          if (OptimizationDecisions[i].EnablePerformanceHooks) HooksInserted++;
        }
      }
    }
    
    Statistics.MPICallsInstrumented = CallsInstrumented;
    Statistics.HooksInserted = HooksInserted;
    
    return CallsInstrumented > 0;
  } catch (const std::exception& E) {
    if (ErrHandler) {
      ErrHandler->reportError(ErrorCategory::Instrumentation, ErrorLevel::Error,
                               "Instrumentation failed: " + std::string(E.what()),
                               DebugLoc());
    }
    return false;
  }
}

bool MPISanitizerPass::validateRuntimeInterface(Module& M) {
  if (!RuntimeValidator) return true; // Skip validation if no validator
  
  try {
    return RuntimeValidator->validateInterface(M);
  } catch (const std::exception& E) {
    if (ErrHandler) {
      ErrHandler->reportError(ErrorCategory::RuntimeValidation, ErrorLevel::Warning,
                               "Runtime interface validation failed: " + std::string(E.what()),
                               DebugLoc());
    }
    return false;
  }
}

void MPISanitizerPass::generateReport(Module& M) {
  if (!Config.ReportFile.empty()) {
    std::error_code EC;
    raw_fd_ostream ReportFile(Config.ReportFile, EC);
    
    if (!EC) {
      ReportFile << "=== MPI Sanitizer Pass Report ===\n";
      ReportFile << "Module: " << M.getName() << "\n";
      ReportFile << "MPI Calls Detected: " << Statistics.MPICallsDetected << "\n";
      ReportFile << "MPI Calls Instrumented: " << Statistics.MPICallsInstrumented << "\n";
      ReportFile << "Hooks Inserted: " << Statistics.HooksInserted << "\n";
      ReportFile << "Execution Time: " << Statistics.ExecutionTimeUs << " μs\n";
      
      // Add performance profiling report if available
      if (Config.EnableProfiling && Profiler) {
        ReportFile << "\n=== Performance Profile ===\n";
        ReportFile << Profiler->generateDetailedReport(PerformanceProfile);
        
        auto Recommendations = Profiler->generateOptimizationRecommendations(PerformanceProfile);
        if (!Recommendations.empty()) {
          ReportFile << "\n=== Optimization Recommendations ===\n";
          for (const auto& Rec : Recommendations) {
            ReportFile << "- " << Rec << "\n";
          }
        }
      }
      
      // Add optimization report if available
      if (Config.EnableOptimization && Optimizer) {
        ReportFile << "\n=== Optimization Report ===\n";
        ReportFile << Optimizer->generateOptimizationReport();
      }
      
      ReportFile << "================================\n";
    }
  }
}

//===----------------------------------------------------------------------===//
// MPISanitizerLegacyPass Implementation
//===----------------------------------------------------------------------===//

char MPISanitizerLegacyPass::ID = 0;

MPISanitizerLegacyPass::MPISanitizerLegacyPass() : ModulePass(ID) {}

MPISanitizerLegacyPass::MPISanitizerLegacyPass(const PassConfiguration& Config) 
    : ModulePass(ID), Impl(Config) {}

bool MPISanitizerLegacyPass::runOnModule(Module &M) {
  ModuleAnalysisManager DummyMAM;
  PreservedAnalyses PA = Impl.run(M, DummyMAM);
  return !PA.areAllPreserved();
}

void MPISanitizerLegacyPass::getAnalysisUsage(AnalysisUsage &AU) const {
  // The pass may modify the module
  AU.setPreservesCFG();
}

//===----------------------------------------------------------------------===//
// Pass Registration
//===----------------------------------------------------------------------===//

ModulePass *createMPISanitizerLegacyPass() {
  return new MPISanitizerLegacyPass();
}

ModulePass *createMPISanitizerLegacyPass(const PassConfiguration& Config) {
  return new MPISanitizerLegacyPass(Config);
}

// Register the pass
INITIALIZE_PASS(MPISanitizerLegacyPass, "mpi-sanitizer",
                "MPI Usage Sanitizer", false, false)

} // namespace llvm