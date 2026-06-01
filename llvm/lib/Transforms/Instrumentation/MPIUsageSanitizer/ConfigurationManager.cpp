//===- ConfigurationManager.cpp - MPI Sanitizer Configuration -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the ConfigurationManager class which provides flexible
// configuration options for instrumentation policies and optimization levels.
//
//===----------------------------------------------------------------------===//

#include "ConfigurationManager.h"
#include "MPICallDetector.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/Twine.h"
#include <fstream>
#include <sstream>

using namespace llvm;

#define DEBUG_TYPE "mpi-config-manager"

//===----------------------------------------------------------------------===//
// Command Line Options
//===----------------------------------------------------------------------===//

// These are declared extern in the main pass file, we reference them here
extern cl::opt<bool> ClEnableOptimizations;
extern cl::opt<bool> ClEnablePerformanceMonitoring;
extern cl::opt<bool> ClEnableDeadlockDetection;
extern cl::opt<bool> ClEnableDataRaceDetection;
extern cl::opt<std::string> ClInstrumentationLevel;
extern cl::opt<std::string> ClConfigFile;
extern cl::opt<bool> ClVerbose;
extern cl::opt<bool> ClStatistics;

// Additional command line options for fine-grained control
static cl::list<std::string> ClEnabledTypes(
    "mpi-sanitizer-enable-types",
    cl::desc("Comma-separated list of MPI function types to instrument "
             "(pointtopoint,collective,communicator,datatype,request,info,window,file,topology,environment)"),
    cl::Hidden, cl::CommaSeparated);

static cl::list<std::string> ClDisabledTypes(
    "mpi-sanitizer-disable-types", 
    cl::desc("Comma-separated list of MPI function types to skip instrumentation"),
    cl::Hidden, cl::CommaSeparated);

static cl::list<std::string> ClEnabledFunctions(
    "mpi-sanitizer-enable-functions",
    cl::desc("Comma-separated list of specific MPI functions to instrument"),
    cl::Hidden, cl::CommaSeparated);

static cl::list<std::string> ClDisabledFunctions(
    "mpi-sanitizer-disable-functions",
    cl::desc("Comma-separated list of specific MPI functions to skip"),
    cl::Hidden, cl::CommaSeparated);

//===----------------------------------------------------------------------===//
// PassConfiguration Implementation
//===----------------------------------------------------------------------===//

PassConfiguration::PassConfiguration() {
  // Initialize all MPI function types as enabled by default
  EnabledTypes.insert(MPIFunctionType::PointToPoint);
  EnabledTypes.insert(MPIFunctionType::Collective);
  EnabledTypes.insert(MPIFunctionType::Communicator);
  EnabledTypes.insert(MPIFunctionType::Datatype);
  EnabledTypes.insert(MPIFunctionType::Request);
  EnabledTypes.insert(MPIFunctionType::Info);
  EnabledTypes.insert(MPIFunctionType::Window);
  EnabledTypes.insert(MPIFunctionType::File);
  EnabledTypes.insert(MPIFunctionType::Topology);
  EnabledTypes.insert(MPIFunctionType::Environment);
}

//===----------------------------------------------------------------------===//
// CommandLineParser Implementation
//===----------------------------------------------------------------------===//

CommandLineParser::CommandLineParser() {
  initializeOptions();
}

void CommandLineParser::initializeOptions() {
  // Options are already initialized as static variables above
}

void CommandLineParser::parseOptions(PassConfiguration& Config) {
  // Parse instrumentation level
  if (ClInstrumentationLevel == "full") {
    Config.Level = MPIUsageSanitizerOptions::InstrumentationLevel::Full;
  } else if (ClInstrumentationLevel == "lightweight") {
    Config.Level = MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight;
  } else if (ClInstrumentationLevel == "performance") {
    Config.Level = MPIUsageSanitizerOptions::InstrumentationLevel::Performance;
  }
  
  // Parse global options
  Config.EnableOptimizations = ClEnableOptimizations;
  Config.EnablePerformanceMonitoring = ClEnablePerformanceMonitoring;
  Config.EnableDeadlockDetection = ClEnableDeadlockDetection;
  Config.EnableDataRaceDetection = ClEnableDataRaceDetection;
  Config.ConfigFile = ClConfigFile;
  Config.Verbose = ClVerbose;
  Config.PrintStatistics = ClStatistics;
  
  // Parse enabled/disabled types
  if (!ClEnabledTypes.empty()) {
    Config.EnabledTypes.clear(); // Clear defaults if explicit list provided
    for (const std::string& TypeStr : ClEnabledTypes) {
      MPIFunctionType Type = stringToFunctionType(TypeStr);
      if (Type != MPIFunctionType::Unknown) {
        Config.EnabledTypes.insert(Type);
      }
    }
  }
  
  // Remove disabled types
  for (const std::string& TypeStr : ClDisabledTypes) {
    MPIFunctionType Type = stringToFunctionType(TypeStr);
    if (Type != MPIFunctionType::Unknown) {
      Config.EnabledTypes.erase(Type);
    }
  }
  
  // Set up per-function policies for enabled/disabled functions
  InstrumentationPolicy EnabledPolicy;
  EnabledPolicy.EnablePreHooks = true;
  EnabledPolicy.EnablePostHooks = true;
  EnabledPolicy.EnableErrorChecking = true;
  
  InstrumentationPolicy DisabledPolicy;
  DisabledPolicy.EnablePreHooks = false;
  DisabledPolicy.EnablePostHooks = false;
  DisabledPolicy.EnableErrorChecking = false;
  
  for (const std::string& FuncName : ClEnabledFunctions) {
    Config.FunctionPolicies[FuncName] = EnabledPolicy;
  }
  
  for (const std::string& FuncName : ClDisabledFunctions) {
    Config.FunctionPolicies[FuncName] = DisabledPolicy;
  }
}

MPIUsageSanitizerOptions::InstrumentationLevel 
CommandLineParser::getInstrumentationLevel() const {
  if (ClInstrumentationLevel == "full") {
    return MPIUsageSanitizerOptions::InstrumentationLevel::Full;
  } else if (ClInstrumentationLevel == "lightweight") {
    return MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight;
  } else if (ClInstrumentationLevel == "performance") {
    return MPIUsageSanitizerOptions::InstrumentationLevel::Performance;
  }
  return MPIUsageSanitizerOptions::InstrumentationLevel::Full;
}

bool CommandLineParser::isTypeEnabled(MPIFunctionType Type) const {
  // Check if type is explicitly disabled
  for (const std::string& TypeStr : ClDisabledTypes) {
    if (stringToFunctionType(TypeStr) == Type) {
      return false;
    }
  }
  
  // If enabled types are specified, check if this type is in the list
  if (!ClEnabledTypes.empty()) {
    for (const std::string& TypeStr : ClEnabledTypes) {
      if (stringToFunctionType(TypeStr) == Type) {
        return true;
      }
    }
    return false; // Not in explicit enabled list
  }
  
  return true; // Default to enabled
}

MPIFunctionType CommandLineParser::stringToFunctionType(StringRef TypeStr) {
  std::string Lower = TypeStr.lower();
  if (Lower == "pointtopoint" || Lower == "p2p") return MPIFunctionType::PointToPoint;
  if (Lower == "collective" || Lower == "coll") return MPIFunctionType::Collective;
  if (Lower == "communicator" || Lower == "comm") return MPIFunctionType::Communicator;
  if (Lower == "datatype" || Lower == "type") return MPIFunctionType::Datatype;
  if (Lower == "request" || Lower == "req") return MPIFunctionType::Request;
  if (Lower == "info") return MPIFunctionType::Info;
  if (Lower == "window" || Lower == "win") return MPIFunctionType::Window;
  if (Lower == "file" || Lower == "io") return MPIFunctionType::File;
  if (Lower == "topology" || Lower == "topo") return MPIFunctionType::Topology;
  if (Lower == "environment" || Lower == "env") return MPIFunctionType::Environment;
  return MPIFunctionType::Unknown;
}

//===----------------------------------------------------------------------===//
// ConfigFileParser Implementation
//===----------------------------------------------------------------------===//

bool ConfigFileParser::parseFile(StringRef FilePath, PassConfiguration& Config) {
  // Check if file exists
  if (!sys::fs::exists(FilePath)) {
    LLVM_DEBUG(dbgs() << "Config file does not exist: " << FilePath << "\n");
    return false;
  }
  
  // Read file contents
  auto FileOrError = MemoryBuffer::getFile(FilePath);
  if (std::error_code EC = FileOrError.getError()) {
    LLVM_DEBUG(dbgs() << "Failed to read config file: " << EC.message() << "\n");
    return false;
  }
  
  std::unique_ptr<MemoryBuffer> Buffer = std::move(FileOrError.get());
  
  // Parse configuration file line by line
  StringRef CurrentSection;
  InstrumentationPolicy CurrentPolicy;
  
  for (line_iterator I(*Buffer, /*SkipBlanks=*/true), E; I != E; ++I) {
    StringRef Line = I->trim();
    
    // Skip comments
    if (Line.starts_with("#") || Line.starts_with("//")) {
      continue;
    }
    
    // Handle section headers [section_name]
    if (Line.starts_with("[") && Line.ends_with("]")) {
      // Save previous section if it was a policy section
      if (!CurrentSection.empty() && CurrentSection.starts_with("policy.")) {
        StringRef PolicyName = CurrentSection.drop_front(7); // Remove "policy."
        if (PolicyName.starts_with("type.")) {
          StringRef TypeName = PolicyName.drop_front(5);
          MPIFunctionType Type = stringToFunctionType(TypeName);
          if (Type != MPIFunctionType::Unknown) {
            Config.TypePolicies[TypeName] = CurrentPolicy;
          }
        } else if (PolicyName.starts_with("function.")) {
          StringRef FuncName = PolicyName.drop_front(9);
          Config.FunctionPolicies[FuncName] = CurrentPolicy;
        }
      }
      
      CurrentSection = Line.slice(1, Line.size() - 1);
      CurrentPolicy = InstrumentationPolicy(); // Reset to defaults
      continue;
    }
    
    // Handle key=value pairs
    size_t EqPos = Line.find('=');
    if (EqPos == StringRef::npos) {
      continue; // Skip malformed lines
    }
    
    StringRef Key = Line.slice(0, EqPos).trim();
    StringRef Value = Line.slice(EqPos + 1, StringRef::npos).trim();
    
    // Parse global configuration
    if (CurrentSection == "global") {
      if (Key == "instrumentation_level") {
        if (Value == "full") {
          Config.Level = MPIUsageSanitizerOptions::InstrumentationLevel::Full;
        } else if (Value == "lightweight") {
          Config.Level = MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight;
        } else if (Value == "performance") {
          Config.Level = MPIUsageSanitizerOptions::InstrumentationLevel::Performance;
        }
      } else if (Key == "enable_optimizations") {
        Config.EnableOptimizations = parseBool(Value, true);
      } else if (Key == "enable_performance_monitoring") {
        Config.EnablePerformanceMonitoring = parseBool(Value, false);
      } else if (Key == "enable_deadlock_detection") {
        Config.EnableDeadlockDetection = parseBool(Value, true);
      } else if (Key == "enable_datarace_detection") {
        Config.EnableDataRaceDetection = parseBool(Value, true);
      } else if (Key == "verbose") {
        Config.Verbose = parseBool(Value, false);
      } else if (Key == "print_statistics") {
        Config.PrintStatistics = parseBool(Value, false);
      } else if (Key == "enabled_types") {
        Config.EnabledTypes = parseFunctionTypes(Value);
      }
    }
    // Parse policy sections
    else if (CurrentSection.starts_with("policy.")) {
      if (Key == "enable_pre_hooks") {
        CurrentPolicy.EnablePreHooks = parseBool(Value, true);
      } else if (Key == "enable_post_hooks") {
        CurrentPolicy.EnablePostHooks = parseBool(Value, true);
      } else if (Key == "enable_performance_hooks") {
        CurrentPolicy.EnablePerformanceHooks = parseBool(Value, false);
      } else if (Key == "enable_error_checking") {
        CurrentPolicy.EnableErrorChecking = parseBool(Value, true);
      } else if (Key == "enable_deadlock_detection") {
        CurrentPolicy.EnableDeadlockDetection = parseBool(Value, true);
      } else if (Key == "enable_datarace_detection") {
        CurrentPolicy.EnableDataRaceDetection = parseBool(Value, true);
      } else if (Key == "optimization_level") {
        CurrentPolicy.OptimizationLevel = parseInt(Value, 0);
      }
    }
  }
  
  // Save final section if it was a policy section
  if (!CurrentSection.empty() && CurrentSection.starts_with("policy.")) {
    StringRef PolicyName = CurrentSection.drop_front(7);
    if (PolicyName.starts_with("type.")) {
      StringRef TypeName = PolicyName.drop_front(5);
      MPIFunctionType Type = stringToFunctionType(TypeName);
      if (Type != MPIFunctionType::Unknown) {
        Config.TypePolicies[TypeName] = CurrentPolicy;
      }
    } else if (PolicyName.starts_with("function.")) {
      StringRef FuncName = PolicyName.drop_front(9);
      Config.FunctionPolicies[FuncName] = CurrentPolicy;
    }
  }
  
  return true;
}

bool ConfigFileParser::validateFile(StringRef FilePath) {
  return sys::fs::exists(FilePath) && sys::fs::is_regular_file(FilePath);
}

std::set<MPIFunctionType> ConfigFileParser::parseFunctionTypes(StringRef TypeList) {
  std::set<MPIFunctionType> Types;
  SmallVector<StringRef, 8> TypeNames;
  TypeList.split(TypeNames, ',');
  
  for (StringRef TypeName : TypeNames) {
    MPIFunctionType Type = stringToFunctionType(TypeName.trim());
    if (Type != MPIFunctionType::Unknown) {
      Types.insert(Type);
    }
  }
  
  return Types;
}

MPIFunctionType ConfigFileParser::stringToFunctionType(StringRef TypeStr) {
  std::string Lower = TypeStr.lower();
  if (Lower == "pointtopoint" || Lower == "p2p") return MPIFunctionType::PointToPoint;
  if (Lower == "collective" || Lower == "coll") return MPIFunctionType::Collective;
  if (Lower == "communicator" || Lower == "comm") return MPIFunctionType::Communicator;
  if (Lower == "datatype" || Lower == "type") return MPIFunctionType::Datatype;
  if (Lower == "request" || Lower == "req") return MPIFunctionType::Request;
  if (Lower == "info") return MPIFunctionType::Info;
  if (Lower == "window" || Lower == "win") return MPIFunctionType::Window;
  if (Lower == "file" || Lower == "io") return MPIFunctionType::File;
  if (Lower == "topology" || Lower == "topo") return MPIFunctionType::Topology;
  if (Lower == "environment" || Lower == "env") return MPIFunctionType::Environment;
  return MPIFunctionType::Unknown;
}

bool ConfigFileParser::parseBool(StringRef Value, bool DefaultValue) {
  std::string Lower = Value.lower();
  if (Lower == "true" || Lower == "yes" || Lower == "1" || Lower == "on") {
    return true;
  } else if (Lower == "false" || Lower == "no" || Lower == "0" || Lower == "off") {
    return false;
  }
  return DefaultValue;
}

int ConfigFileParser::parseInt(StringRef Value, int DefaultValue) {
  int Result;
  if (Value.getAsInteger(10, Result)) {
    return DefaultValue;
  }
  return Result;
}

//===----------------------------------------------------------------------===//
// ConfigurationManager Implementation
//===----------------------------------------------------------------------===//

ConfigurationManager::ConfigurationManager(const MPIUsageSanitizerOptions& Opts) 
    : Options(Opts), CLParser(std::make_unique<CommandLineParser>()),
      FileParser(std::make_unique<ConfigFileParser>()) {
  LLVM_DEBUG(dbgs() << "Initializing MPI Configuration Manager\n");
}

ConfigurationManager::~ConfigurationManager() = default;

bool ConfigurationManager::initialize() {
  LLVM_DEBUG(dbgs() << "Initializing configuration with level: " 
                    << (int)Options.Level << "\n");
  
  // Start with default configuration
  Config = PassConfiguration();
  
  // Parse command line options first
  parseCommandLineOptions();
  
  // Load configuration from file if specified (overrides command line)
  if (!Config.ConfigFile.empty()) {
    if (!loadFromFile(Config.ConfigFile)) {
      LLVM_DEBUG(dbgs() << "Failed to load config file: " << Config.ConfigFile << "\n");
      return false;
    }
  }
  
  // Merge configurations
  mergeConfigurations();
  
  Initialized = true;
  return true;
}

void ConfigurationManager::parseCommandLineOptions() {
  CLParser->parseOptions(Config);
}

bool ConfigurationManager::loadFromFile(StringRef ConfigFile) {
  if (!FileParser->validateFile(ConfigFile)) {
    LLVM_DEBUG(dbgs() << "Invalid config file: " << ConfigFile << "\n");
    return false;
  }
  
  return FileParser->parseFile(ConfigFile, Config);
}

void ConfigurationManager::mergeConfigurations() {
  // Apply original options as overrides
  Config.Level = Options.Level;
  Config.EnableOptimizations = Options.EnableOptimizations;
  Config.EnablePerformanceMonitoring = Options.EnablePerformanceMonitoring;
  Config.EnableDeadlockDetection = Options.EnableDeadlockDetection;
  Config.EnableDataRaceDetection = Options.EnableDataRaceDetection;
  
  if (!Options.ConfigFile.empty()) {
    Config.ConfigFile = Options.ConfigFile;
  }
}

bool ConfigurationManager::shouldInstrument(const CallSite& Site) const {
  return shouldInstrument(Site, nullptr);
}

bool ConfigurationManager::shouldInstrument(const CallSite& Site, const AnalysisResult* Analysis) const {
  if (!Initialized) {
    return true; // Default to instrumenting everything
  }
  
  // First check if the function type is enabled at all
  if (!shouldInstrumentCategory(Site.Type)) {
    return false;
  }
  
  // Apply instrumentation mode filtering (lightweight vs full)
  if (!applyInstrumentationModeFilter(Site, Site.Type)) {
    return false;
  }
  
  // Check function-specific policy first
  auto FuncIt = Config.FunctionPolicies.find(Site.FunctionName);
  if (FuncIt != Config.FunctionPolicies.end()) {
    const InstrumentationPolicy& Policy = FuncIt->second;
    return evaluatePolicyControls(Site, Policy);
  }
  
  // Check type-based policy
  InstrumentationPolicy Policy = getInstrumentationPolicy(Site.Type);
  return evaluatePolicyControls(Site, Policy);
}

bool ConfigurationManager::shouldInstrument(StringRef FunctionName) const {
  if (!Initialized) {
    return true; // Default to instrumenting everything
  }
  
  // Check function-specific policy first
  auto FuncIt = Config.FunctionPolicies.find(FunctionName);
  if (FuncIt != Config.FunctionPolicies.end()) {
    const InstrumentationPolicy& Policy = FuncIt->second;
    return Policy.EnablePreHooks || Policy.EnablePostHooks || Policy.EnableErrorChecking;
  }
  
  // Check type-based policy
  MPIFunctionType Type = classifyFunction(FunctionName);
  return shouldInstrument(Type);
}

bool ConfigurationManager::shouldInstrument(MPIFunctionType Type) const {
  if (!Initialized) {
    return true;
  }
  
  // Check if type is enabled
  if (Config.EnabledTypes.find(Type) == Config.EnabledTypes.end()) {
    return false;
  }
  
  // Apply instrumentation level filtering
  switch (Config.Level) {
    case MPIUsageSanitizerOptions::InstrumentationLevel::Full:
      return true; // Instrument everything that's enabled
      
    case MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight:
      // Only instrument error-prone operations
      return Type == MPIFunctionType::PointToPoint || 
             Type == MPIFunctionType::Request ||
             Type == MPIFunctionType::Collective;
             
    case MPIUsageSanitizerOptions::InstrumentationLevel::Performance:
      // Only instrument for performance monitoring
      return Config.EnablePerformanceMonitoring && 
             (Type == MPIFunctionType::Collective || Type == MPIFunctionType::PointToPoint);
  }
  
  return true;
}

bool ConfigurationManager::shouldInstrumentCategory(MPIFunctionType Type) const {
  if (!Initialized) {
    return true;
  }
  
  // Check if category is explicitly enabled
  return Config.EnabledTypes.find(Type) != Config.EnabledTypes.end();
}

bool ConfigurationManager::applyInstrumentationModeFilter(const CallSite& Site, MPIFunctionType Type) const {
  if (!Initialized) {
    return true;
  }
  
  switch (Config.Level) {
    case MPIUsageSanitizerOptions::InstrumentationLevel::Full:
      // Full mode: instrument all enabled operations
      return true;
      
    case MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight:
      // Lightweight mode: only critical error-prone operations
      switch (Type) {
        case MPIFunctionType::PointToPoint:
          // All point-to-point operations are error-prone
          return true;
        case MPIFunctionType::Request:
          // Request operations are critical for correctness
          return true;
        case MPIFunctionType::Collective:
          // Only synchronizing collective operations
          return Site.FunctionName.contains("Barrier") ||
                 Site.FunctionName.contains("Allreduce") ||
                 Site.FunctionName.contains("Bcast");
        case MPIFunctionType::Environment:
          // Only Init/Finalize operations
          return Site.FunctionName == "MPI_Init" ||
                 Site.FunctionName == "MPI_Finalize" ||
                 Site.FunctionName == "MPI_Init_thread";
        default:
          return false;
      }
      
    case MPIUsageSanitizerOptions::InstrumentationLevel::Performance:
      // Performance mode: only operations that benefit from performance monitoring
      switch (Type) {
        case MPIFunctionType::Collective:
          return Config.EnablePerformanceMonitoring;
        case MPIFunctionType::PointToPoint:
          // Only blocking point-to-point operations for performance monitoring
          return Config.EnablePerformanceMonitoring && 
                 !Site.FunctionName.starts_with("MPI_I"); // Non-immediate operations
        default:
          return false;
      }
  }
  
  return true;
}

bool ConfigurationManager::evaluatePolicyControls(const CallSite& Site, const InstrumentationPolicy& Policy) const {
  // A call site should be instrumented if any hook type is enabled
  bool ShouldInstrument = Policy.EnablePreHooks || 
                         Policy.EnablePostHooks || 
                         Policy.EnableErrorChecking;
  
  // Apply additional policy-specific filters
  if (ShouldInstrument) {
    // Check if performance hooks are required but disabled
    if (Config.EnablePerformanceMonitoring && !Policy.EnablePerformanceHooks) {
      // Still instrument for basic error checking even if performance hooks are disabled
      ShouldInstrument = Policy.EnableErrorChecking;
    }
    
    // Check optimization level constraints
    if (Policy.OptimizationLevel >= 2) {
      // High optimization level - be more selective
      switch (Site.Type) {
        case MPIFunctionType::Environment:
        case MPIFunctionType::Info:
        case MPIFunctionType::Datatype:
          // These are typically safe operations, skip in high optimization
          ShouldInstrument = false;
          break;
        default:
          break;
      }
    }
  }
  
  return ShouldInstrument;
}

InstrumentationPolicy ConfigurationManager::getInstrumentationPolicy(StringRef FunctionName) const {
  // Check function-specific policy first
  auto FuncIt = Config.FunctionPolicies.find(FunctionName);
  if (FuncIt != Config.FunctionPolicies.end()) {
    return FuncIt->second;
  }
  
  // Fall back to type-based policy
  MPIFunctionType Type = classifyFunction(FunctionName);
  return getInstrumentationPolicy(Type);
}

InstrumentationPolicy ConfigurationManager::getInstrumentationPolicy(MPIFunctionType Type) const {
  // Look for type-specific policy
  std::string TypeName;
  switch (Type) {
    case MPIFunctionType::PointToPoint: TypeName = "pointtopoint"; break;
    case MPIFunctionType::Collective: TypeName = "collective"; break;
    case MPIFunctionType::Communicator: TypeName = "communicator"; break;
    case MPIFunctionType::Datatype: TypeName = "datatype"; break;
    case MPIFunctionType::Request: TypeName = "request"; break;
    case MPIFunctionType::Info: TypeName = "info"; break;
    case MPIFunctionType::Window: TypeName = "window"; break;
    case MPIFunctionType::File: TypeName = "file"; break;
    case MPIFunctionType::Topology: TypeName = "topology"; break;
    case MPIFunctionType::Environment: TypeName = "environment"; break;
    default: break;
  }
  
  auto TypeIt = Config.TypePolicies.find(TypeName);
  if (TypeIt != Config.TypePolicies.end()) {
    return TypeIt->second;
  }
  
  // Return default policy for this type
  return getDefaultPolicy(Type);
}

int ConfigurationManager::getInstrumentationLevel(StringRef FunctionType) const {
  // Convert function type string to enum and get policy
  MPIFunctionType Type = classifyFunction(FunctionType);
  InstrumentationPolicy Policy = getInstrumentationPolicy(Type);
  
  // Calculate level based on policy settings
  int Level = 0;
  if (Policy.EnablePreHooks) Level += 1;
  if (Policy.EnablePostHooks) Level += 1;
  if (Policy.EnableErrorChecking) Level += 1;
  if (Policy.EnablePerformanceHooks) Level += 1;
  
  return std::max(Level, Policy.OptimizationLevel);
}

MPIFunctionType ConfigurationManager::classifyFunction(StringRef FunctionName) const {
  // Point-to-point operations
  if (FunctionName.starts_with("MPI_Send") || FunctionName.starts_with("MPI_Recv") ||
      FunctionName.starts_with("MPI_Isend") || FunctionName.starts_with("MPI_Irecv") ||
      FunctionName.starts_with("MPI_Sendrecv") || FunctionName.starts_with("MPI_Probe") ||
      FunctionName.starts_with("MPI_Iprobe")) {
    return MPIFunctionType::PointToPoint;
  }
  
  // Collective operations
  if (FunctionName.starts_with("MPI_Bcast") || FunctionName.starts_with("MPI_Reduce") ||
      FunctionName.starts_with("MPI_Allreduce") || FunctionName.starts_with("MPI_Gather") ||
      FunctionName.starts_with("MPI_Scatter") || FunctionName.starts_with("MPI_Allgather") ||
      FunctionName.starts_with("MPI_Alltoall") || FunctionName.starts_with("MPI_Barrier")) {
    return MPIFunctionType::Collective;
  }
  
  // Request operations
  if (FunctionName.starts_with("MPI_Wait") || FunctionName.starts_with("MPI_Test") ||
      FunctionName.starts_with("MPI_Request")) {
    return MPIFunctionType::Request;
  }
  
  // Communicator operations
  if (FunctionName.starts_with("MPI_Comm")) {
    return MPIFunctionType::Communicator;
  }
  
  // Datatype operations
  if (FunctionName.starts_with("MPI_Type")) {
    return MPIFunctionType::Datatype;
  }
  
  // Info operations
  if (FunctionName.starts_with("MPI_Info")) {
    return MPIFunctionType::Info;
  }
  
  // Window operations
  if (FunctionName.starts_with("MPI_Win")) {
    return MPIFunctionType::Window;
  }
  
  // File operations
  if (FunctionName.starts_with("MPI_File")) {
    return MPIFunctionType::File;
  }
  
  // Topology operations
  if (FunctionName.starts_with("MPI_Cart") || FunctionName.starts_with("MPI_Graph") ||
      FunctionName.starts_with("MPI_Topo")) {
    return MPIFunctionType::Topology;
  }
  
  // Environment operations
  if (FunctionName == "MPI_Init" || FunctionName == "MPI_Finalize" ||
      FunctionName == "MPI_Init_thread" || FunctionName == "MPI_Initialized" ||
      FunctionName == "MPI_Finalized") {
    return MPIFunctionType::Environment;
  }
  
  return MPIFunctionType::PointToPoint; // Default fallback
}

InstrumentationPolicy ConfigurationManager::getDefaultPolicy(MPIFunctionType Type) const {
  InstrumentationPolicy Policy;
  
  switch (Type) {
    case MPIFunctionType::PointToPoint:
    case MPIFunctionType::Request:
      // High instrumentation for error-prone operations
      Policy.EnablePreHooks = true;
      Policy.EnablePostHooks = true;
      Policy.EnableErrorChecking = true;
      Policy.EnableDeadlockDetection = true;
      Policy.EnableDataRaceDetection = true;
      Policy.OptimizationLevel = 1;
      break;
      
    case MPIFunctionType::Collective:
      // Medium instrumentation for collective operations
      Policy.EnablePreHooks = true;
      Policy.EnablePostHooks = true;
      Policy.EnableErrorChecking = true;
      Policy.EnablePerformanceHooks = Config.EnablePerformanceMonitoring;
      Policy.EnableDeadlockDetection = true;
      Policy.OptimizationLevel = 1;
      break;
      
    case MPIFunctionType::Environment:
      // Light instrumentation for environment operations
      Policy.EnablePreHooks = true;
      Policy.EnablePostHooks = false;
      Policy.EnableErrorChecking = true;
      Policy.OptimizationLevel = 0;
      break;
      
    default:
      // Standard instrumentation for other operations
      Policy.EnablePreHooks = true;
      Policy.EnablePostHooks = true;
      Policy.EnableErrorChecking = true;
      Policy.OptimizationLevel = 0;
      break;
  }
  
  return Policy;
}

void ConfigurationManager::enableMPIOperationCategory(MPIFunctionType Type) {
  Config.EnabledTypes.insert(Type);
  LLVM_DEBUG(dbgs() << "Enabled MPI operation category: " << (int)Type << "\n");
}

void ConfigurationManager::disableMPIOperationCategory(MPIFunctionType Type) {
  Config.EnabledTypes.erase(Type);
  LLVM_DEBUG(dbgs() << "Disabled MPI operation category: " << (int)Type << "\n");
}

void ConfigurationManager::setInstrumentationMode(MPIUsageSanitizerOptions::InstrumentationLevel Level) {
  Config.Level = Level;
  LLVM_DEBUG(dbgs() << "Set instrumentation mode to: " << (int)Level << "\n");
}

void ConfigurationManager::setFunctionTypePolicy(MPIFunctionType Type, const InstrumentationPolicy& Policy) {
  std::string TypeName;
  switch (Type) {
    case MPIFunctionType::PointToPoint: TypeName = "pointtopoint"; break;
    case MPIFunctionType::Collective: TypeName = "collective"; break;
    case MPIFunctionType::Communicator: TypeName = "communicator"; break;
    case MPIFunctionType::Datatype: TypeName = "datatype"; break;
    case MPIFunctionType::Request: TypeName = "request"; break;
    case MPIFunctionType::Info: TypeName = "info"; break;
    case MPIFunctionType::Window: TypeName = "window"; break;
    case MPIFunctionType::File: TypeName = "file"; break;
    case MPIFunctionType::Topology: TypeName = "topology"; break;
    case MPIFunctionType::Environment: TypeName = "environment"; break;
    default: TypeName = "unknown"; break;
  }
  
  Config.TypePolicies[TypeName] = Policy;
  LLVM_DEBUG(dbgs() << "Set policy for function type: " << TypeName << "\n");
}

void ConfigurationManager::setFunctionPolicy(StringRef FunctionName, const InstrumentationPolicy& Policy) {
  Config.FunctionPolicies[FunctionName] = Policy;
  LLVM_DEBUG(dbgs() << "Set policy for function: " << FunctionName << "\n");
}

std::set<MPIFunctionType> ConfigurationManager::getDisabledCategories() const {
  std::set<MPIFunctionType> AllTypes = {
    MPIFunctionType::PointToPoint,
    MPIFunctionType::Collective,
    MPIFunctionType::Communicator,
    MPIFunctionType::Datatype,
    MPIFunctionType::Request,
    MPIFunctionType::Info,
    MPIFunctionType::Window,
    MPIFunctionType::File,
    MPIFunctionType::Topology,
    MPIFunctionType::Environment
  };
  
  std::set<MPIFunctionType> DisabledTypes;
  for (MPIFunctionType Type : AllTypes) {
    if (Config.EnabledTypes.find(Type) == Config.EnabledTypes.end()) {
      DisabledTypes.insert(Type);
    }
  }
  
  return DisabledTypes;
}