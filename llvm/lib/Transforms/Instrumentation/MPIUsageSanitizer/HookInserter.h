//===- HookInserter.h - MPI Hook Insertion Framework ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the HookInserter class which inserts runtime hook
// functions before and after MPI calls while preserving program semantics.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_HOOKINSERTER_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_HOOKINSERTER_H

#include "MPICallDetector.h"
#include "MetadataExtractor.h"
#include "RuntimeInterface.h"
#include "OptimizationEngine.h"
#include "ConfigurationManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include <memory>

namespace llvm {

class Module;
class Function;

  /// Configuration for hook insertion behavior
struct HookConfiguration {
  bool EnablePreHooks = true;
  bool EnablePostHooks = true;
  bool EnablePerformanceHooks = false;
  bool EnableCommunicationVolumeHooks = false;
  bool EnableCommunicationPatternHooks = false;
  bool EnableCollectiveTimingHooks = false;
  bool EnableSynchronizationHooks = false;
  bool EnableSelectiveInstrumentation = true;
  bool PreserveDebugInfo = true;
  InstrumentationLevel Level = InstrumentationLevel::Full;
  
  // Performance monitoring configuration
  double PerformanceOverheadThreshold = 0.05; // 5% overhead limit
  bool MonitorPointToPointOps = true;
  bool MonitorCollectiveOps = true;
  bool MonitorCommunicatorOps = false;
  bool MonitorDatatypeOps = false;
};

/// Hook Insertion Framework
///
/// Inserts runtime hook functions before and after MPI calls while
/// preserving program semantics and original function behavior.
class HookInserter {
public:
  HookInserter(const HookConfiguration& Config = {});
  HookInserter(const HookConfiguration& Config, std::shared_ptr<ConfigurationManager> ConfigMgr);
  ~HookInserter();
  
  /// Insert hooks for all MPI calls in a function
  bool insertHooks(Function& F, const std::vector<CallSite>& Sites);
  
  /// Create declarations for all required hook functions in the module
  void createHookDeclarations(Module& M);
  
  /// Insert pre-call hook before an MPI function call
  bool insertPreCallHook(CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Insert post-call hook after an MPI function call
  bool insertPostCallHook(CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Insert performance monitoring hooks around MPI calls
  bool insertPerformanceHooks(CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Insert communication volume monitoring hooks
  bool insertCommunicationVolumeHooks(CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Insert communication pattern analysis hooks
  bool insertCommunicationPatternHooks(CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Insert selective performance instrumentation based on optimization decisions
  bool insertSelectivePerformanceHooks(CallSite& Site, const MPICallMetadata& Metadata, 
                                      const struct OptimizationDecision& Decision);
  
  /// Insert timing hooks for MPI collective operations
  bool insertCollectiveTimingHooks(CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Insert synchronization point monitoring hooks
  bool insertSynchronizationHooks(CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Set the current module context
  void setModule(Module* M) { CurrentModule = M; }
  
  /// Get the current configuration
  const HookConfiguration& getConfiguration() const { return Config; }
  
  /// Update configuration
  void setConfiguration(const HookConfiguration& NewConfig) { Config = NewConfig; }
  
  /// Set configuration manager for policy-driven decisions
  void setConfigurationManager(std::shared_ptr<class ConfigurationManager> ConfigMgr);
  
  /// Check if a call site should be instrumented based on policy
  bool shouldInstrumentCallSite(const CallSite& Site) const;
  
  /// Insert hooks with policy-driven selective instrumentation
  bool insertHooksWithPolicy(Function& F, const std::vector<CallSite>& Sites);

private:
  /// Create function declaration for a hook function
  Function* createHookDeclaration(Module& M, StringRef Name, 
                                  FunctionType* Type);
  
  /// Generate parameters for hook function calls
  std::vector<Value*> generateHookParameters(const CallSite& Site,
                                             const MPICallMetadata& Metadata,
                                             bool IsPreHook);
  
  /// Generate enhanced parameters for pre-call hooks with comprehensive metadata
  std::vector<Value*> generateEnhancedPreCallParameters(const CallSite& Site,
                                                        const MPICallMetadata& Metadata);
  
  /// Generate enhanced parameters for post-call hooks with return value preservation
  std::vector<Value*> generateEnhancedPostCallParameters(const CallSite& Site,
                                                         const MPICallMetadata& Metadata);
  
  /// Create parameter array for pre-call hooks
  Value* createParameterArray(const MPICallMetadata& Metadata);
  
  /// Create enhanced parameter array with type information and role metadata
  Value* createEnhancedParameterArray(const MPICallMetadata& Metadata);
  
  /// Preserve original call's return value and side effects
  Value* preserveReturnValue(CallSite& Site, Value* HookCall);
  
  /// Preserve and extract return value with proper type handling
  Value* preserveAndExtractReturnValue(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Extract MPI-specific error code from return value
  Value* extractMPIErrorCode(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Get or create string constant for function names
  Value* getOrCreateStringConstant(StringRef Str);
  
  /// Extract source location information from debug metadata
  Value* extractSourceLocation(const CallSite& Site);
  
  /// Extract enhanced source location with function context
  Value* extractEnhancedSourceLocation(const CallSite& Site);
  
  /// Get MPI operation type string for performance monitoring
  StringRef getMPIOperationType(MPIFunctionType Type);
  
  /// Validate hook function signatures against runtime interface
  bool validateHookSignatures(Module& M);
  
  /// Handle exception safety for invoke instructions
  bool handleExceptionSafety(CallSite& Site);
  
  /// Validate calling convention compatibility
  bool validateCallingConvention(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Determine if performance monitoring should be applied to this call site
  bool shouldApplyPerformanceMonitoring(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Calculate communication volume for buffer-based operations
  Value* calculateCommunicationVolume(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Extract communication pattern information
  Value* extractCommunicationPattern(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Create performance monitoring configuration value
  Value* createPerformanceConfig(const CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Insert timing begin/end pair around MPI operation
  std::pair<CallInst*, CallInst*> insertTimingPair(CallSite& Site, const MPICallMetadata& Metadata);
  
  /// Check if operation is collective and requires synchronization monitoring
  bool isCollectiveOperation(const MPICallMetadata& Metadata);
  
  /// Check if operation involves communication buffers
  bool hasCommunicationBuffers(const MPICallMetadata& Metadata);
  
  HookConfiguration Config;
  std::unique_ptr<RuntimeInterface> RuntimeIntf;
  std::unique_ptr<IRBuilder<>> Builder;
  Module* CurrentModule = nullptr;
  
  // Configuration manager for policy-driven instrumentation decisions
  std::shared_ptr<class ConfigurationManager> ConfigMgr;
  
  // Cache for created string constants
  std::map<std::string, GlobalVariable*> StringConstants;
  
  // Cache for hook function declarations
  std::map<std::string, Function*> HookDeclarations;
};

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_HOOKINSERTER_H