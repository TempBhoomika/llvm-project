//===- ConfigurationManager.h - MPI Sanitizer Configuration ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the ConfigurationManager class which provides flexible
// configuration options for instrumentation policies and optimization levels.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_CONFIGURATIONMANAGER_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_CONFIGURATIONMANAGER_H

#include "llvm/Transforms/Instrumentation/MPIUsageSanitizer.h"
#include "MPICallDetector.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallVector.h"
#include <set>
#include <string>
#include <memory>

namespace llvm {

struct CallSite;
struct AnalysisResult;

/// Instrumentation Policy for different MPI operation categories
struct InstrumentationPolicy {
  bool EnablePreHooks = true;
  bool EnablePostHooks = true;
  bool EnablePerformanceHooks = false;
  bool EnableErrorChecking = true;
  bool EnableDeadlockDetection = true;
  bool EnableDataRaceDetection = true;
  int OptimizationLevel = 0; // 0=none, 1=basic, 2=aggressive
};

/// Complete pass configuration structure
struct PassConfiguration {
  /// Global instrumentation level
  MPIUsageSanitizerOptions::InstrumentationLevel Level = 
      MPIUsageSanitizerOptions::InstrumentationLevel::Full;
  
  /// Set of enabled MPI function types
  std::set<MPIFunctionType> EnabledTypes;
  
  /// Per-type instrumentation policies
  StringMap<InstrumentationPolicy> TypePolicies;
  
  /// Per-function instrumentation policies
  StringMap<InstrumentationPolicy> FunctionPolicies;
  
  /// Global optimization settings
  bool EnableOptimizations = true;
  bool EnablePerformanceMonitoring = false;
  bool EnableDeadlockDetection = true;
  bool EnableDataRaceDetection = true;
  
  /// Configuration file path
  std::string ConfigFile;
  
  /// Verbose output and statistics
  bool Verbose = false;
  bool PrintStatistics = false;
  
  /// Default constructor initializes all MPI types as enabled
  PassConfiguration();
};

/// Command line option parser for MPI sanitizer
class CommandLineParser {
public:
  CommandLineParser();
  
  /// Parse command line options and populate configuration
  void parseOptions(PassConfiguration& Config);
  
  /// Get parsed instrumentation level
  MPIUsageSanitizerOptions::InstrumentationLevel getInstrumentationLevel() const;
  
  /// Check if specific MPI function types are enabled via command line
  bool isTypeEnabled(MPIFunctionType Type) const;

private:
  void initializeOptions();
  
  /// Convert string to MPIFunctionType
  static MPIFunctionType stringToFunctionType(StringRef TypeStr);
};

/// Configuration file parser for complex instrumentation policies
class ConfigFileParser {
public:
  ConfigFileParser() = default;
  
  /// Parse configuration file and populate configuration
  bool parseFile(StringRef FilePath, PassConfiguration& Config);
  
  /// Validate configuration file format
  bool validateFile(StringRef FilePath);

private:
  /// Parse instrumentation policy section
  bool parseInstrumentationPolicy(StringRef Section, InstrumentationPolicy& Policy);
  
  /// Parse MPI function type list
  std::set<MPIFunctionType> parseFunctionTypes(StringRef TypeList);
  
  /// Convert string to MPIFunctionType
  MPIFunctionType stringToFunctionType(StringRef TypeStr);
  
  /// Parse boolean value from string
  bool parseBool(StringRef Value, bool DefaultValue = false);
  
  /// Parse integer value from string
  int parseInt(StringRef Value, int DefaultValue = 0);
};

/// Configuration Manager for MPI Sanitizer
///
/// Provides flexible configuration options for instrumentation policies
/// and optimization levels. Integrates command line options with configuration
/// file parsing for complex instrumentation policies.
class ConfigurationManager {
public:
  ConfigurationManager(const MPIUsageSanitizerOptions& Options);
  ~ConfigurationManager();
  
  /// Initialize configuration from command line options and config files
  bool initialize();
  
  /// Check if a specific MPI call site should be instrumented
  bool shouldInstrument(const CallSite& Site) const;
  
  /// Enhanced decision logic for call site instrumentation with comprehensive policy evaluation
  bool shouldInstrument(const CallSite& Site, const struct AnalysisResult* Analysis) const;
  
  /// Check if a specific MPI function should be instrumented
  bool shouldInstrument(StringRef FunctionName) const;
  
  /// Check if a specific MPI function type should be instrumented
  bool shouldInstrument(MPIFunctionType Type) const;
  
  /// Check if a specific MPI operation category should be instrumented
  bool shouldInstrumentCategory(MPIFunctionType Type) const;
  
  /// Apply instrumentation mode filtering (lightweight vs full)
  bool applyInstrumentationModeFilter(const CallSite& Site, MPIFunctionType Type) const;
  
  /// Evaluate fine-grained policy controls for a call site
  bool evaluatePolicyControls(const CallSite& Site, const InstrumentationPolicy& Policy) const;
  
  /// Get instrumentation policy for a specific function
  InstrumentationPolicy getInstrumentationPolicy(StringRef FunctionName) const;
  
  /// Get instrumentation policy for a specific function type
  InstrumentationPolicy getInstrumentationPolicy(MPIFunctionType Type) const;
  
  /// Get the instrumentation level for a specific function type
  int getInstrumentationLevel(StringRef FunctionType) const;
  
  /// Load configuration from file
  bool loadFromFile(StringRef ConfigFile);
  
  /// Parse command line options
  void parseCommandLineOptions();
  
  /// Get the current pass configuration
  const PassConfiguration& getConfiguration() const { return Config; }
  
  /// Get the original options
  const MPIUsageSanitizerOptions& getOptions() const { return Options; }
  
  /// Check if verbose output is enabled
  bool isVerbose() const { return Config.Verbose; }
  
  /// Check if statistics should be printed
  bool shouldPrintStatistics() const { return Config.PrintStatistics; }
  
  /// Enable/disable specific MPI operation categories
  void enableMPIOperationCategory(MPIFunctionType Type);
  void disableMPIOperationCategory(MPIFunctionType Type);
  
  /// Set instrumentation mode (lightweight vs full)
  void setInstrumentationMode(MPIUsageSanitizerOptions::InstrumentationLevel Level);
  
  /// Configure fine-grained policy for specific function type
  void setFunctionTypePolicy(MPIFunctionType Type, const InstrumentationPolicy& Policy);
  
  /// Configure fine-grained policy for specific function
  void setFunctionPolicy(StringRef FunctionName, const InstrumentationPolicy& Policy);
  
  /// Get list of enabled MPI operation categories
  std::set<MPIFunctionType> getEnabledCategories() const { return Config.EnabledTypes; }
  
  /// Get list of disabled MPI operation categories
  std::set<MPIFunctionType> getDisabledCategories() const;
  
  /// Check if lightweight mode is active
  bool isLightweightMode() const {
    return Config.Level == MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight;
  }
  
  /// Check if full mode is active
  bool isFullMode() const {
    return Config.Level == MPIUsageSanitizerOptions::InstrumentationLevel::Full;
  }
  
  /// Check if performance mode is active
  bool isPerformanceMode() const {
    return Config.Level == MPIUsageSanitizerOptions::InstrumentationLevel::Performance;
  }

private:
  /// Original options from pass constructor
  MPIUsageSanitizerOptions Options;
  
  /// Complete pass configuration
  PassConfiguration Config;
  
  /// Command line parser
  std::unique_ptr<CommandLineParser> CLParser;
  
  /// Configuration file parser
  std::unique_ptr<ConfigFileParser> FileParser;
  
  /// Initialization state
  bool Initialized = false;
  
  /// Classify MPI function name to type
  MPIFunctionType classifyFunction(StringRef FunctionName) const;
  
  /// Get default instrumentation policy for a function type
  InstrumentationPolicy getDefaultPolicy(MPIFunctionType Type) const;
  
  /// Merge command line options with configuration file settings
  void mergeConfigurations();
};

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_CONFIGURATIONMANAGER_H