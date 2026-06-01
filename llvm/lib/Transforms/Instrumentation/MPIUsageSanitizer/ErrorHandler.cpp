//===- ErrorHandler.cpp - MPI Sanitizer Error Handling ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the ErrorHandler class which provides comprehensive
// error handling and diagnostic integration for the MPI Usage Sanitizer.
//
//===----------------------------------------------------------------------===//

#include "ErrorHandler.h"
#include "MPICallDetector.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Timer.h"
#include "llvm/ADT/Twine.h"
#include <chrono>
#include <set>

using namespace llvm;

#define DEBUG_TYPE "mpi-error-handler"

//===----------------------------------------------------------------------===//
// MPISanitizerDiagnosticInfo Implementation
//===----------------------------------------------------------------------===//

MPISanitizerDiagnosticInfo::MPISanitizerDiagnosticInfo(const MPIErrorInfo& Error)
    : DiagnosticInfo(DK_FirstPluginKind, 
                     Error.Level == ErrorLevel::Fatal ? DS_Error :
                     Error.Level == ErrorLevel::Error ? DS_Error :
                     Error.Level == ErrorLevel::Warning ? DS_Warning : DS_Note),
      Error(Error) {}

void MPISanitizerDiagnosticInfo::print(DiagnosticPrinter &DP) const {
  DP << "MPI Sanitizer ";
  
  // Print error level
  switch (Error.Level) {
    case ErrorLevel::Info:
      DP << "info: ";
      break;
    case ErrorLevel::Warning:
      DP << "warning: ";
      break;
    case ErrorLevel::Error:
      DP << "error: ";
      break;
    case ErrorLevel::Fatal:
      DP << "fatal error: ";
      break;
  }
  
  // Print error category
  DP << "[" << ErrorHandler::getErrorCategoryName(Error.Category) << "] ";
  
  // Print main message
  DP << Error.Message;
  
  // Print source location if available
  if (Error.Location) {
    DP << " at " << Error.Location->getFilename() << ":" 
       << Error.Location->getLine();
    if (Error.Location->getColumn() > 0) {
      DP << ":" << Error.Location->getColumn();
    }
  }
  
  // Print function context if available
  if (Error.Func) {
    DP << " in function '" << Error.Func->getName() << "'";
  }
  
  // Print additional context
  for (const auto& Ctx : Error.Context) {
    DP << " [" << Ctx.first() << ": " << Ctx.second << "]";
  }
}

//===----------------------------------------------------------------------===//
// ErrorHandler Implementation
//===----------------------------------------------------------------------===//

ErrorHandler::ErrorHandler(LLVMContext& Context) : Context(Context) {
  LLVM_DEBUG(dbgs() << "Initializing MPI Sanitizer Error Handler\n");
  
  // Set default error thresholds
  ErrorThresholds["PassInfrastructure"] = 5;
  ErrorThresholds["CallDetection"] = 50;
  ErrorThresholds["MetadataExtraction"] = 100;
  ErrorThresholds["HookInsertion"] = 20;
  ErrorThresholds["StaticAnalysis"] = 30;
  ErrorThresholds["Configuration"] = 10;
  ErrorThresholds["RuntimeInterface"] = 5;
  ErrorThresholds["UnsupportedPattern"] = 200;
}

ErrorHandler::~ErrorHandler() {
  if (CollectStatistics && Statistics.getTotalCount() > 0) {
    LLVM_DEBUG(dbgs() << "MPI Sanitizer Error Handler: " 
                      << Statistics.getTotalCount() << " total errors recorded\n");
  }
}

void ErrorHandler: reportError(StringRef Message){
    return;
}
void ErrorHandler::reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, Category, Message);
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

void ErrorHandler::reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message,
                               const DebugLoc& Location) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, Category, Message);
  Error.Location = Location;
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

void ErrorHandler::reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message,
                               const Instruction* Inst) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, Category, Message);
  Error.Inst = Inst;
  
  if (Inst) {
    Error.Location = extractDebugLocation(Inst);
    Error.Func = Inst->getFunction();
  }
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

void ErrorHandler::reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message,
                               const Function* Func) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, Category, Message);
  Error.Func = Func;
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

void ErrorHandler::reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message,
                               const Instruction* Inst, const StringMap<std::string>& Context) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, Category, Message);
  Error.Inst = Inst;
  Error.Context = Context;
  
  if (Inst) {
    Error.Location = extractDebugLocation(Inst);
    Error.Func = Inst->getFunction();
  }
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

void ErrorHandler::reportMPIError(ErrorLevel Level, StringRef Message, const CallSite& Site) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, ErrorCategory::CallDetection, Message);
  
  // Add MPI-specific context
  Error.Context["mpi_function"] = Site.FunctionName.str();
  Error.Context["mpi_type"] = std::to_string(static_cast<int>(Site.Type));
  
  if (Site.Inst) {
    Error.Inst = Site.Inst;
    Error.Location = extractDebugLocation(Site.Inst);
    Error.Func = Site.Inst->getFunction();
  }
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

void ErrorHandler::reportConfigError(ErrorLevel Level, StringRef Message, StringRef ConfigKey) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, ErrorCategory::Configuration, Message);
  
  if (!ConfigKey.empty()) {
    Error.Context["config_key"] = ConfigKey.str();
  }
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

void ErrorHandler::reportHookError(ErrorLevel Level, StringRef Message, const Instruction* Inst,
                                   StringRef HookName) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, ErrorCategory::HookInsertion, Message);
  Error.Inst = Inst;
  
  if (!HookName.empty()) {
    Error.Context["hook_name"] = HookName.str();
  }
  
  if (Inst) {
    Error.Location = extractDebugLocation(Inst);
    Error.Func = Inst->getFunction();
  }
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

void ErrorHandler::reportAnalysisError(ErrorLevel Level, StringRef Message, const Function* Func,
                                       StringRef AnalysisType) {
  MPIErrorInfo Error = createMPIErrorInfo(Level, ErrorCategory::StaticAnalysis, Message);
  Error.Func = Func;
  
  if (!AnalysisType.empty()) {
    Error.Context["analysis_type"] = AnalysisType.str();
  }
  
  if (CollectStatistics) {
    updateStatistics(Error);
  }
  
  if (!isErrorLimitReached()) {
    Errors.push_back(std::move(Error));
  }
  
  reportToDiagnosticEngine(Errors.back());
}

bool ErrorHandler::shouldContinueAfterError(ErrorLevel Level) const {
  switch (Level) {
    case ErrorLevel::Info:
      return true;
    case ErrorLevel::Warning:
      return ContinueOnWarnings;
    case ErrorLevel::Error:
      return ContinueOnErrors;
    case ErrorLevel::Fatal:
      return false;
  }
  return false;
}

bool ErrorHandler::shouldContinueAfterError(const MPIErrorInfo& Error) const {
  RecoveryContext Context = createRecoveryContext(Error, "default");
  return shouldContinueAfterError(Error, Context);
}

RecoveryStrategy ErrorHandler::determineRecoveryStrategy(const MPIErrorInfo& Error, const RecoveryContext& Context) const {
  // Check error level first
  if (Error.Level == ErrorLevel::Fatal) {
    return RecoveryStrategy::Stop;
  }
  
  // Check if error threshold exceeded for this category
  if (isErrorThresholdExceeded(Error.Category)) {
    LLVM_DEBUG(dbgs() << "Error threshold exceeded for category: " 
                      << getErrorCategoryName(Error.Category) << "\n");
    return RecoveryStrategy::Stop;
  }
  
  // Category-specific recovery strategies
  switch (Error.Category) {
    case ErrorCategory::PassInfrastructure:
      // Infrastructure errors are usually fatal
      return (Error.Level == ErrorLevel::Error) ? RecoveryStrategy::Stop : RecoveryStrategy::ContinueReduced;
      
    case ErrorCategory::CallDetection:
      // Can skip problematic calls and continue
      return RecoveryStrategy::SkipAndContinue;
      
    case ErrorCategory::MetadataExtraction:
      // Can continue with reduced metadata
      return RecoveryStrategy::ContinueReduced;
      
    case ErrorCategory::HookInsertion:
      // Can skip instrumentation for failed calls
      return RecoveryStrategy::SkipAndContinue;
      
    case ErrorCategory::StaticAnalysis:
      // Can continue without optimization
      return RecoveryStrategy::ContinueFallback;
      
    case ErrorCategory::Configuration:
      // Can use defaults or fallback configuration
      return RecoveryStrategy::ContinueFallback;
      
    case ErrorCategory::RuntimeInterface:
      // Interface errors may be recoverable with alternative approaches
      return hasAlternativeApproaches(Error.Category) ? RecoveryStrategy::RetryAlternative : RecoveryStrategy::Stop;
      
    case ErrorCategory::UnsupportedPattern:
      // Always skip unsupported patterns
      return RecoveryStrategy::SkipAndContinue;
  }
  
  // Default strategy based on error level
  switch (Error.Level) {
    case ErrorLevel::Info:
    case ErrorLevel::Warning:
      return RecoveryStrategy::SkipAndContinue;
    case ErrorLevel::Error:
      return ContinueOnErrors ? RecoveryStrategy::ContinueReduced : RecoveryStrategy::Stop;
    case ErrorLevel::Fatal:
      return RecoveryStrategy::Stop;
  }
  
  return RecoveryStrategy::Stop;
}

bool ErrorHandler::executeRecoveryStrategy(RecoveryStrategy Strategy, const RecoveryContext& Context) {
  recordRecoveryAttempt(Strategy, true); // Assume success initially
  
  switch (Strategy) {
    case RecoveryStrategy::Stop:
      LLVM_DEBUG(dbgs() << "Recovery strategy: Stop processing\n");
      return false;
      
    case RecoveryStrategy::ContinueReduced:
      LLVM_DEBUG(dbgs() << "Recovery strategy: Continue with reduced functionality\n");
      // Implementation would disable certain features
      return true;
      
    case RecoveryStrategy::ContinueFallback:
      LLVM_DEBUG(dbgs() << "Recovery strategy: Continue with fallback behavior\n");
      // Implementation would use fallback approaches
      return true;
      
    case RecoveryStrategy::SkipAndContinue:
      LLVM_DEBUG(dbgs() << "Recovery strategy: Skip current operation and continue\n");
      // Implementation would skip the problematic operation
      return true;
      
    case RecoveryStrategy::RetryAlternative:
      LLVM_DEBUG(dbgs() << "Recovery strategy: Retry with alternative approach\n");
      // Implementation would try alternative methods
      return true;
  }
  
  return false;
}

bool ErrorHandler::shouldContinueAfterError(const MPIErrorInfo& Error, const RecoveryContext& Context) const {
  // Determine and execute recovery strategy
  RecoveryStrategy Strategy = determineRecoveryStrategy(Error, Context);
  
  // For const method, we can't execute the strategy, just determine if we should continue
  switch (Strategy) {
    case RecoveryStrategy::Stop:
      return false;
    case RecoveryStrategy::ContinueReduced:
    case RecoveryStrategy::ContinueFallback:
    case RecoveryStrategy::SkipAndContinue:
    case RecoveryStrategy::RetryAlternative:
      return true;
  }
  
  return false;
}

bool ErrorHandler::handleUnsupportedPattern(const MPIErrorInfo& Error, StringRef PatternDescription) {
  if (!GracefulDegradationMode) {
    return false;
  }
  
  // Record the unsupported pattern
  UnsupportedPatterns.insert(PatternDescription.str());
  
  // Report as info level if graceful degradation is enabled
  MPIErrorInfo DegradationInfo(ErrorLevel::Info, ErrorCategory::UnsupportedPattern,
                           "Gracefully skipping unsupported pattern: " + PatternDescription.str());
  
  if (CollectStatistics) {
    updateStatistics(DegradationInfo);
  }
  
  LLVM_DEBUG(dbgs() << "Gracefully handling unsupported pattern: " << PatternDescription << "\n");
  
  return true; // Continue processing
}

void ErrorHandler::collectErrorStatistics(const MPIErrorInfo& Error) {
  updateStatistics(Error);
  
  // Additional statistics collection for recovery analysis
  StringRef CategoryName = getErrorCategoryName(Error.Category);
  
  // Track error patterns for recovery strategy optimization
  if (Error.Level == ErrorLevel::Error || Error.Level == ErrorLevel::Fatal) {
    // This could be used to adjust recovery strategies dynamically
    LLVM_DEBUG(dbgs() << "Collecting error statistics for category: " << CategoryName << "\n");
  }
}

SmallVector<StringRef, 4> ErrorHandler::generateRecoveryRecommendations(const MPIErrorInfo& Error) const {
  SmallVector<StringRef, 4> Recommendations;
  
  switch (Error.Category) {
    case ErrorCategory::PassInfrastructure:
      Recommendations.push_back("Check LLVM version compatibility");
      Recommendations.push_back("Verify pass registration");
      break;
      
    case ErrorCategory::CallDetection:
      Recommendations.push_back("Check MPI function signatures");
      Recommendations.push_back("Verify function name mangling");
      Recommendations.push_back("Enable verbose call detection");
      break;
      
    case ErrorCategory::MetadataExtraction:
      Recommendations.push_back("Simplify parameter analysis");
      Recommendations.push_back("Use conservative type assumptions");
      break;
      
    case ErrorCategory::HookInsertion:
      Recommendations.push_back("Check runtime library compatibility");
      Recommendations.push_back("Verify hook function signatures");
      Recommendations.push_back("Use alternative instrumentation approach");
      break;
      
    case ErrorCategory::StaticAnalysis:
      Recommendations.push_back("Disable advanced optimizations");
      Recommendations.push_back("Use conservative analysis assumptions");
      break;
      
    case ErrorCategory::Configuration:
      Recommendations.push_back("Check configuration file format");
      Recommendations.push_back("Use default configuration values");
      Recommendations.push_back("Validate command line options");
      break;
      
    case ErrorCategory::RuntimeInterface:
      Recommendations.push_back("Update runtime library version");
      Recommendations.push_back("Check ABI compatibility");
      break;
      
    case ErrorCategory::UnsupportedPattern:
      Recommendations.push_back("Enable graceful degradation mode");
      Recommendations.push_back("Skip unsupported MPI patterns");
      break;
  }
  
  return Recommendations;
}

bool ErrorHandler::isErrorThresholdExceeded(ErrorCategory Category) const {
  StringRef CategoryName = getErrorCategoryName(Category);
  
  auto ThresholdIt = ErrorThresholds.find(CategoryName);
  if (ThresholdIt == ErrorThresholds.end()) {
    return false; // No threshold set
  }
  
  auto CountIt = Statistics.CategoryCounts.find(CategoryName);
  if (CountIt == Statistics.CategoryCounts.end()) {
    return false; // No errors in this category yet
  }
  
  return CountIt->second >= ThresholdIt->second;
}

void ErrorHandler::setErrorThreshold(ErrorCategory Category, uint32_t Threshold) {
  StringRef CategoryName = getErrorCategoryName(Category);
  ErrorThresholds[CategoryName] = Threshold;
  
  LLVM_DEBUG(dbgs() << "Set error threshold for " << CategoryName 
                    << " to " << Threshold << "\n");
}

void ErrorHandler::printStatistics(raw_ostream& OS) const {
  OS << "MPI Sanitizer Error Statistics:\n";
  OS << "  Total errors: " << Statistics.getTotalCount() << "\n";
  OS << "  Info: " << Statistics.InfoCount << "\n";
  OS << "  Warnings: " << Statistics.WarningCount << "\n";
  OS << "  Errors: " << Statistics.ErrorCount << "\n";
  OS << "  Fatal: " << Statistics.FatalCount << "\n";
  
  if (!Statistics.CategoryCounts.empty()) {
    OS << "\nBy Category:\n";
    for (const auto& Cat : Statistics.CategoryCounts) {
      OS << "  " << Cat.first() << ": " << Cat.second;
      
      // Show threshold status
      auto ThresholdIt = ErrorThresholds.find(Cat.first());
      if (ThresholdIt != ErrorThresholds.end()) {
        OS << " (threshold: " << ThresholdIt->second;
        if (Cat.second >= ThresholdIt->second) {
          OS << " - EXCEEDED";
        }
        OS << ")";
      }
      OS << "\n";
    }
  }
  
  // Print recovery statistics
  if (!RecoveryAttempts.empty()) {
    OS << "\nRecovery Statistics:\n";
    for (const auto& Recovery : RecoveryAttempts) {
      uint32_t Attempts = Recovery.second;
      uint32_t Successes = 0;
      
      auto SuccessIt = SuccessfulRecoveries.find(Recovery.first());
      if (SuccessIt != SuccessfulRecoveries.end()) {
        Successes = SuccessIt->second;
      }
      
      double SuccessRate = (Attempts > 0) ? (static_cast<double>(Successes) / Attempts * 100.0) : 0.0;
      
      OS << "  " << Recovery.first() << ": " << Successes << "/" << Attempts 
         << " (" << format("%.1f", SuccessRate) << "%)\n";
    }
  }
  
  // Print unsupported patterns if any
  if (!UnsupportedPatterns.empty()) {
    OS << "\nUnsupported Patterns Encountered (" << UnsupportedPatterns.size() << "):\n";
    for (const auto& Pattern : UnsupportedPatterns) {
      OS << "  " << Pattern << "\n";
    }
  }
  
  // Print graceful degradation status
  if (GracefulDegradationMode) {
    OS << "\nGraceful Degradation: ENABLED\n";
  } else {
    OS << "\nGraceful Degradation: DISABLED\n";
  }
}

void ErrorHandler::clearErrors() {
  Errors.clear();
  Statistics = ErrorStatistics();
  RecoveryAttempts.clear();
  SuccessfulRecoveries.clear();
  UnsupportedPatterns.clear();
  LLVM_DEBUG(dbgs() << "Cleared all MPI Sanitizer errors, statistics, and recovery data\n");
}

void ErrorHandler::setErrorRecoveryPolicy(bool ContinueOnWarnings, bool ContinueOnErrors) {
  this->ContinueOnWarnings = ContinueOnWarnings;
  this->ContinueOnErrors = ContinueOnErrors;
  
  LLVM_DEBUG(dbgs() << "Set error recovery policy: warnings=" 
                    << (ContinueOnWarnings ? "continue" : "stop")
                    << ", errors=" << (ContinueOnErrors ? "continue" : "stop") << "\n");
}

SmallVector<const MPIErrorInfo*, 8> ErrorHandler::getErrorsByCategory(ErrorCategory Category) const {
  SmallVector<const MPIErrorInfo*, 8> Result;
  for (const auto& Error : Errors) {
    if (Error.Category == Category) {
      Result.push_back(&Error);
    }
  }
  return Result;
}

SmallVector<const MPIErrorInfo*, 8> ErrorHandler::getErrorsByLevel(ErrorLevel Level) const {
  SmallVector<const MPIErrorInfo*, 8> Result;
  for (const auto& Error : Errors) {
    if (Error.Level == Level) {
      Result.push_back(&Error);
    }
  }
  return Result;
}

bool ErrorHandler::hasErrorCategory(ErrorCategory Category) const {
  for (const auto& Error : Errors) {
    if (Error.Category == Category) {
      return true;
    }
  }
  return false;
}

bool ErrorHandler::hasErrorLevel(ErrorLevel Level) const {
  for (const auto& Error : Errors) {
    if (Error.Level == Level) {
      return true;
    }
  }
  return false;
}

std::string ErrorHandler::formatErrorMessage(const MPIErrorInfo& Error) const {
  std::string Result;
  raw_string_ostream OS(Result);
  
  // Format: [LEVEL] [CATEGORY] Message
  OS << "[" << getErrorLevelName(Error.Level) << "] ";
  OS << "[" << getErrorCategoryName(Error.Category) << "] ";
  OS << Error.Message;
  
  // Add source location
  if (Error.Location) {
    OS << " (" << formatSourceLocation(Error.Location) << ")";
  }
  
  // Add function context
  if (Error.Func) {
    OS << " in " << Error.Func->getName();
  }
  
  // Add additional context
  for (const auto& Ctx : Error.Context) {
    OS << " [" << Ctx.first() << "=" << Ctx.second << "]";
  }
  
  return OS.str();
}

StringRef ErrorHandler::getErrorCategoryName(ErrorCategory Category) {
  switch (Category) {
    case ErrorCategory::PassInfrastructure: return "PassInfrastructure";
    case ErrorCategory::CallDetection: return "CallDetection";
    case ErrorCategory::MetadataExtraction: return "MetadataExtraction";
    case ErrorCategory::HookInsertion: return "HookInsertion";
    case ErrorCategory::StaticAnalysis: return "StaticAnalysis";
    case ErrorCategory::Configuration: return "Configuration";
    case ErrorCategory::RuntimeInterface: return "RuntimeInterface";
    case ErrorCategory::UnsupportedPattern: return "UnsupportedPattern";
  }
  return "Unknown";
}

StringRef ErrorHandler::getErrorLevelName(ErrorLevel Level) {
  switch (Level) {
    case ErrorLevel::Info: return "INFO";
    case ErrorLevel::Warning: return "WARNING";
    case ErrorLevel::Error: return "ERROR";
    case ErrorLevel::Fatal: return "FATAL";
  }
  return "UNKNOWN";
}

ErrorLevel ErrorHandler::parseErrorLevel(StringRef LevelStr) {
  if (LevelStr.equals_insensitive("info")) return ErrorLevel::Info;
  if (LevelStr.equals_insensitive("warning")) return ErrorLevel::Warning;
  if (LevelStr.equals_insensitive("error")) return ErrorLevel::Error;
  if (LevelStr.equals_insensitive("fatal")) return ErrorLevel::Fatal;
  return ErrorLevel::Error; // Default
}

ErrorCategory ErrorHandler::parseErrorCategory(StringRef CategoryStr) {
  if (CategoryStr.equals_insensitive("PassInfrastructure")) return ErrorCategory::PassInfrastructure;
  if (CategoryStr.equals_insensitive("CallDetection")) return ErrorCategory::CallDetection;
  if (CategoryStr.equals_insensitive("MetadataExtraction")) return ErrorCategory::MetadataExtraction;
  if (CategoryStr.equals_insensitive("HookInsertion")) return ErrorCategory::HookInsertion;
  if (CategoryStr.equals_insensitive("StaticAnalysis")) return ErrorCategory::StaticAnalysis;
  if (CategoryStr.equals_insensitive("Configuration")) return ErrorCategory::Configuration;
  if (CategoryStr.equals_insensitive("RuntimeInterface")) return ErrorCategory::RuntimeInterface;
  if (CategoryStr.equals_insensitive("UnsupportedPattern")) return ErrorCategory::UnsupportedPattern;
  return ErrorCategory::PassInfrastructure; // Default
}

void ErrorHandler::reportToDiagnosticEngine(const MPIErrorInfo& Error) {
  // Create diagnostic info and report through LLVM's diagnostic engine
  auto DiagInfo = std::make_unique<MPISanitizerDiagnosticInfo>(Error);
  Context.diagnose(*DiagInfo);
  
  // Also output to debug stream if verbose reporting is enabled
  if (VerboseReporting) {
    LLVM_DEBUG(dbgs() << formatErrorMessage(Error) << "\n");
  }
}

void ErrorHandler::updateStatistics(const MPIErrorInfo& Error) {
  // Update level counts
  switch (Error.Level) {
    case ErrorLevel::Info:
      Statistics.InfoCount++;
      break;
    case ErrorLevel::Warning:
      Statistics.WarningCount++;
      break;
    case ErrorLevel::Error:
      Statistics.ErrorCount++;
      break;
    case ErrorLevel::Fatal:
      Statistics.FatalCount++;
      break;
  }
  
  // Update category counts
  StringRef CategoryName = getErrorCategoryName(Error.Category);
  Statistics.CategoryCounts[CategoryName]++;
}

MPIErrorInfo ErrorHandler::createMPIErrorInfo(ErrorLevel Level, ErrorCategory Category, StringRef Message) {
  MPIErrorInfo Error(Level, Category, Message);
  
  // Set timestamp
  auto Now = std::chrono::system_clock::now();
  auto Duration = Now.time_since_epoch();
  Error.Timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(Duration).count();
  
  return Error;
}

DebugLoc ErrorHandler::extractDebugLocation(const Instruction* Inst) {
  if (!Inst) {
    return DebugLoc();
  }
  
  return Inst->getDebugLoc();
}

std::string ErrorHandler::formatSourceLocation(const DebugLoc& Location) const {
  if (!Location) {
    return "unknown location";
  }
  
  std::string Result;
  raw_string_ostream OS(Result);
  
  OS << Location->getFilename();
  if (Location->getLine() > 0) {
    OS << ":" << Location->getLine();
    if (Location->getColumn() > 0) {
      OS << ":" << Location->getColumn();
    }
  }
  
  return OS.str();
}

RecoveryContext ErrorHandler::createRecoveryContext(const MPIErrorInfo& Error, StringRef Phase) const {
  RecoveryContext Context;
  Context.CurrentError = &Error;
  Context.ProcessingPhase = Phase;
  Context.TotalErrorCount = Statistics.getTotalCount();
  
  // Count errors in the same category
  StringRef CategoryName = getErrorCategoryName(Error.Category);
  auto CountIt = Statistics.CategoryCounts.find(CategoryName);
  if (CountIt != Statistics.CategoryCounts.end()) {
    Context.CategoryErrorCount = CountIt->second;
  }
  
  // Determine if this is a critical path
  Context.IsCriticalPath = (Error.Category == ErrorCategory::PassInfrastructure ||
                           Error.Category == ErrorCategory::RuntimeInterface);
  
  // Generate fallback options
  Context.FallbackOptions = generateRecoveryRecommendations(Error);
  
  return Context;
}

bool ErrorHandler::isErrorSevereInContext(const MPIErrorInfo& Error, const RecoveryContext& Context) const {
  // Error is severe if:
  // 1. It's on a critical path
  // 2. Too many errors in the same category
  // 3. Fatal or error level with no recovery options
  
  if (Context.IsCriticalPath && Error.Level >= ErrorLevel::Error) {
    return true;
  }
  
  if (Context.CategoryErrorCount > 10) { // Too many errors in same category
    return true;
  }
  
  if (Error.Level == ErrorLevel::Fatal) {
    return true;
  }
  
  if (Error.Level == ErrorLevel::Error && Context.FallbackOptions.empty()) {
    return true;
  }
  
  return false;
}

bool ErrorHandler::hasAlternativeApproaches(ErrorCategory Category) const {
  switch (Category) {
    case ErrorCategory::CallDetection:
      // Can use different detection methods
      return true;
    case ErrorCategory::MetadataExtraction:
      // Can use conservative assumptions
      return true;
    case ErrorCategory::HookInsertion:
      // Can use different instrumentation approaches
      return true;
    case ErrorCategory::StaticAnalysis:
      // Can disable optimizations
      return true;
    case ErrorCategory::Configuration:
      // Can use defaults
      return true;
    default:
      return false;
  }
}

void ErrorHandler::recordRecoveryAttempt(RecoveryStrategy Strategy, bool Success) {
  std::string StrategyName;
  switch (Strategy) {
    case RecoveryStrategy::Stop: StrategyName = "Stop"; break;
    case RecoveryStrategy::ContinueReduced: StrategyName = "ContinueReduced"; break;
    case RecoveryStrategy::ContinueFallback: StrategyName = "ContinueFallback"; break;
    case RecoveryStrategy::SkipAndContinue: StrategyName = "SkipAndContinue"; break;
    case RecoveryStrategy::RetryAlternative: StrategyName = "RetryAlternative"; break;
  }
  
  RecoveryAttempts[StrategyName]++;
  if (Success) {
    SuccessfulRecoveries[StrategyName]++;
  }
  
  LLVM_DEBUG(dbgs() << "Recorded recovery attempt: " << StrategyName 
                    << " (success: " << (Success ? "yes" : "no") << ")\n");
}

double ErrorHandler::getRecoverySuccessRate(RecoveryStrategy Strategy) const {
  std::string StrategyName;
  switch (Strategy) {
    case RecoveryStrategy::Stop: StrategyName = "Stop"; break;
    case RecoveryStrategy::ContinueReduced: StrategyName = "ContinueReduced"; break;
    case RecoveryStrategy::ContinueFallback: StrategyName = "ContinueFallback"; break;
    case RecoveryStrategy::SkipAndContinue: StrategyName = "SkipAndContinue"; break;
    case RecoveryStrategy::RetryAlternative: StrategyName = "RetryAlternative"; break;
  }
  
  auto AttemptsIt = RecoveryAttempts.find(StrategyName);
  auto SuccessIt = SuccessfulRecoveries.find(StrategyName);
  
  if (AttemptsIt == RecoveryAttempts.end() || AttemptsIt->second == 0) {
    return 0.0;
  }
  
  uint32_t Successes = (SuccessIt != SuccessfulRecoveries.end()) ? SuccessIt->second : 0;
  return static_cast<double>(Successes) / static_cast<double>(AttemptsIt->second);
}

//===----------------------------------------------------------------------===//
// ErrorContext Implementation
//===----------------------------------------------------------------------===//

ErrorContext::ErrorContext(ErrorHandler& Handler, StringRef ContextName, StringRef ContextValue)
    : Handler(Handler) {
  // Save current context and add new context
  // This is a simplified implementation - in a full implementation,
  // we would maintain a context stack in the ErrorHandler
  addContext(ContextName, ContextValue);
}

ErrorContext::~ErrorContext() {
  // Restore previous context
  // In a full implementation, we would pop the context stack
}

void ErrorContext::addContext(StringRef Key, StringRef Value) {
  // In a full implementation, this would add to the current context
  // that gets automatically included in error reports
  SavedContext[Key] = Value.str();
}
