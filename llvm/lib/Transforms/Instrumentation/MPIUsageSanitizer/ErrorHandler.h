//===- ErrorHandler.h - MPI Sanitizer Error Handling ----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the ErrorHandler class which provides comprehensive
// error handling and diagnostic integration for the MPI Usage Sanitizer.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_ERRORHANDLER_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_ERRORHANDLER_H

#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Instruction.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/raw_ostream.h"
#include <string>
#include <memory>

namespace llvm {

class Function;
class CallInst;
class InvokeInst;
struct CallSite;

/// Error severity levels for MPI sanitizer diagnostics
enum class ErrorLevel {
  /// Informational messages (e.g., optimization opportunities)
  Info = 0,
  
  /// Warning messages (e.g., potentially unsafe patterns)
  Warning = 1,
  
  /// Error messages (e.g., definite correctness issues)
  Error = 2,
  
  /// Fatal error messages (e.g., pass cannot continue)
  Fatal = 3
};

/// Error categories for MPI sanitizer diagnostics
enum class ErrorCategory {
  /// General pass infrastructure errors
  PassInfrastructure,
  
  /// MPI function detection and analysis errors
  CallDetection,
  
  /// Metadata extraction and parameter analysis errors
  MetadataExtraction,
  
  /// Hook insertion and code generation errors
  HookInsertion,
  
  /// Static analysis and optimization errors
  StaticAnalysis,
  
  /// Configuration and policy errors
  Configuration,
  
  /// Runtime library interface errors
  RuntimeInterface,
  
  /// Unsupported MPI patterns or constructs
  UnsupportedPattern
};

/// Detailed error information structure
struct MPIErrorInfo {
  /// Error severity level
  ErrorLevel Level;
  
  /// Error category
  ErrorCategory Category;
  
  /// Error message
  std::string Message;
  
  /// Source location (if available)
  DebugLoc Location;
  
  /// Associated function (if applicable)
  const Function* Func = nullptr;
  
  /// Associated instruction (if applicable)
  const Instruction* Inst = nullptr;
  
  /// Additional context information
  StringMap<std::string> Context;
  
  /// Error timestamp
  uint64_t Timestamp;
  
  /// Constructor
  MPIErrorInfo(ErrorLevel Level, ErrorCategory Category, StringRef Message)
      : Level(Level), Category(Category), Message(Message.str()),
        Timestamp(0) {}
};

/// Statistics for error reporting and analysis
struct ErrorStatistics {
  /// Count by error level
  uint32_t InfoCount = 0;
  uint32_t WarningCount = 0;
  uint32_t ErrorCount = 0;
  uint32_t FatalCount = 0;
  
  /// Count by error category
  StringMap<uint32_t> CategoryCounts;
  
  /// Total error count
  uint32_t getTotalCount() const {
    return InfoCount + WarningCount + ErrorCount + FatalCount;
  }
  
  /// Check if any errors occurred
  bool hasErrors() const {
    return ErrorCount > 0 || FatalCount > 0;
  }
  
  /// Check if any fatal errors occurred
  bool hasFatalErrors() const {
    return FatalCount > 0;
  }
};

/// Custom diagnostic info for MPI sanitizer
class MPISanitizerDiagnosticInfo : public DiagnosticInfo {
private:
  const MPIErrorInfo& Error;
  
public:
  MPISanitizerDiagnosticInfo(const MPIErrorInfo& Error);
  
  void print(DiagnosticPrinter &DP) const override;
  
  static bool classof(const DiagnosticInfo *DI) {
    return DI->getKind() == DK_FirstPluginKind;
  }
  
  /// Get the error info
  const MPIErrorInfo& getMPIErrorInfo() const { return Error; }
};

/// Error recovery strategy for different error scenarios
enum class RecoveryStrategy {
  /// Stop processing immediately
  Stop,
  
  /// Continue with reduced functionality
  ContinueReduced,
  
  /// Continue with fallback behavior
  ContinueFallback,
  
  /// Skip current operation and continue
  SkipAndContinue,
  
  /// Retry with different approach
  RetryAlternative
};

/// Error recovery context for decision making
struct RecoveryContext {
  /// Current error being processed
  const MPIErrorInfo* CurrentError = nullptr;
  
  /// Previous errors in same category
  uint32_t CategoryErrorCount = 0;
  
  /// Total error count so far
  uint32_t TotalErrorCount = 0;
  
  /// Current processing phase
  StringRef ProcessingPhase;
  
  /// Available fallback options
  SmallVector<StringRef, 4> FallbackOptions;
  
  /// Whether this is a critical path
  bool IsCriticalPath = false;
};

/// Error Handler for MPI Sanitizer
///
/// Provides comprehensive error handling and diagnostic integration with LLVM's
/// diagnostic engine. Supports error categorization, source location tracking,
/// statistics collection, and graceful error recovery.
class ErrorHandler {
public:
  ErrorHandler(LLVMContext& Context);
  ~ErrorHandler();
  
  /// Report an error with basic information
  void reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message);
  
  /// Report an error with source location
  void reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message,
                   const DebugLoc& Location);
  
  /// Report an error with associated instruction
  void reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message,
                   const Instruction* Inst);
  
  /// Report an error with associated function
  void reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message,
                   const Function* Func);
  
  /// Report an error with full context information
  void reportError(ErrorLevel Level, ErrorCategory Category, StringRef Message,
                   const Instruction* Inst, const StringMap<std::string>& Context);
  
  /// Report MPI-specific errors with call site information
  void reportMPIError(ErrorLevel Level, StringRef Message, const CallSite& Site);
  
  /// Report configuration errors
  void reportConfigError(ErrorLevel Level, StringRef Message, StringRef ConfigKey = "");
  
  /// Report hook insertion errors
  void reportHookError(ErrorLevel Level, StringRef Message, const Instruction* Inst,
                       StringRef HookName = "");
  
  /// Report static analysis errors
  void reportAnalysisError(ErrorLevel Level, StringRef Message, const Function* Func,
                           StringRef AnalysisType = "");
  
  /// Check if pass should continue after error
  bool shouldContinueAfterError(ErrorLevel Level) const;
  
  /// Check if pass should continue after error with context
  bool shouldContinueAfterError(const MPIErrorInfo& Error) const;
  
  /// Determine recovery strategy for a given error
  RecoveryStrategy determineRecoveryStrategy(const MPIErrorInfo& Error, const RecoveryContext& Context) const;
  
  /// Execute error recovery strategy
  bool executeRecoveryStrategy(RecoveryStrategy Strategy, const RecoveryContext& Context);
  
  /// Check if pass should continue after error with enhanced context
  bool shouldContinueAfterError(const MPIErrorInfo& Error, const RecoveryContext& Context) const;
  
  /// Implement graceful degradation for unsupported patterns
  bool handleUnsupportedPattern(const MPIErrorInfo& Error, StringRef PatternDescription);
  
  /// Collect and report error statistics
  void collectErrorStatistics(const MPIErrorInfo& Error);
  
  /// Generate error recovery recommendations
  SmallVector<StringRef, 4> generateRecoveryRecommendations(const MPIErrorInfo& Error) const;
  
  /// Check if error threshold has been exceeded for a category
  bool isErrorThresholdExceeded(ErrorCategory Category) const;
  
  /// Set error thresholds for different categories
  void setErrorThreshold(ErrorCategory Category, uint32_t Threshold);
  
  /// Enable/disable graceful degradation mode
  void setGracefulDegradationMode(bool Enable) { GracefulDegradationMode = Enable; }
  
  /// Check if graceful degradation is enabled
  bool isGracefulDegradationEnabled() const { return GracefulDegradationMode; }
  
  /// Get current error statistics
  const ErrorStatistics& getStatistics() const { return Statistics; }
  
  /// Print error statistics to output stream
  void printStatistics(raw_ostream& OS) const;
  
  /// Clear all recorded errors and statistics
  void clearErrors();
  
  /// Set error recovery policy
  void setErrorRecoveryPolicy(bool ContinueOnWarnings, bool ContinueOnErrors);
  
  /// Enable/disable verbose error reporting
  void setVerboseReporting(bool Verbose) { VerboseReporting = Verbose; }
  
  /// Enable/disable error statistics collection
  void setStatisticsCollection(bool Collect) { CollectStatistics = Collect; }
  
  /// Get all recorded errors
  const SmallVector<MPIErrorInfo, 16>& getErrors() const { return Errors; }
  
  /// Get errors by category
  SmallVector<const MPIErrorInfo*, 8> getErrorsByCategory(ErrorCategory Category) const;
  
  /// Get errors by level
  SmallVector<const MPIErrorInfo*, 8> getErrorsByLevel(ErrorLevel Level) const;
  
  /// Check if specific error category has occurred
  bool hasErrorCategory(ErrorCategory Category) const;
  
  /// Check if specific error level has occurred
  bool hasErrorLevel(ErrorLevel Level) const;
  
  /// Format error message with context
  std::string formatErrorMessage(const MPIErrorInfo& Error) const;
  
  /// Get error category name as string
  static StringRef getErrorCategoryName(ErrorCategory Category);
  
  /// Get error level name as string
  static StringRef getErrorLevelName(ErrorLevel Level);
  
  /// Convert string to error level
  static ErrorLevel parseErrorLevel(StringRef LevelStr);
  
  /// Convert string to error category
  static ErrorCategory parseErrorCategory(StringRef CategoryStr);

private:
  /// LLVM context for diagnostic reporting
  LLVMContext& Context;
  
  /// Recorded errors
  SmallVector<MPIErrorInfo, 16> Errors;
  
  /// Error statistics
  ErrorStatistics Statistics;
  
  /// Error recovery policy
  bool ContinueOnWarnings = true;
  bool ContinueOnErrors = false;
  
  /// Reporting options
  bool VerboseReporting = false;
  bool CollectStatistics = true;
  bool GracefulDegradationMode = true;
  
  /// Error thresholds per category
  StringMap<uint32_t> ErrorThresholds;
  
  /// Recovery statistics
  StringMap<uint32_t> RecoveryAttempts;
  StringMap<uint32_t> SuccessfulRecoveries;
  
  /// Unsupported pattern tracking
  std::set<std::string> UnsupportedPatterns;
  
  /// Maximum number of errors to collect (prevent memory issues)
  static constexpr size_t MaxErrorCount = 1000;
  
  /// Report error through LLVM diagnostic engine
  void reportToDiagnosticEngine(const MPIErrorInfo& Error);
  
  /// Update error statistics
  void updateStatistics(const MPIErrorInfo& Error);
  
  /// Create error info with current timestamp
  MPIErrorInfo createMPIErrorInfo(ErrorLevel Level, ErrorCategory Category, StringRef Message);
  
  /// Extract source location from instruction
  DebugLoc extractDebugLocation(const Instruction* Inst);
  
  /// Format source location for display
  std::string formatSourceLocation(const DebugLoc& Location) const;
  
  /// Check if error collection limit reached
  bool isErrorLimitReached() const { return Errors.size() >= MaxErrorCount; }
  
  /// Create recovery context for error processing
  RecoveryContext createRecoveryContext(const MPIErrorInfo& Error, StringRef Phase) const;
  
  /// Evaluate error severity in context
  bool isErrorSevereInContext(const MPIErrorInfo& Error, const RecoveryContext& Context) const;
  
  /// Check if alternative approaches are available
  bool hasAlternativeApproaches(ErrorCategory Category) const;
  
  /// Record recovery attempt and outcome
  void recordRecoveryAttempt(RecoveryStrategy Strategy, bool Success);
  
  /// Get recovery success rate for a strategy
  double getRecoverySuccessRate(RecoveryStrategy Strategy) const;
};

/// RAII helper for error context management
class ErrorContext {
public:
  ErrorContext(ErrorHandler& Handler, StringRef ContextName, StringRef ContextValue);
  ~ErrorContext();
  
  /// Add additional context information
  void addContext(StringRef Key, StringRef Value);
  
private:
  ErrorHandler& Handler;
  StringMap<std::string> SavedContext;
};

/// Convenience macros for error reporting
#define MPI_SANITIZER_ERROR(Handler, Category, Message) \
  (Handler).reportError(ErrorLevel::Error, ErrorCategory::Category, Message)

#define MPI_SANITIZER_WARNING(Handler, Category, Message) \
  (Handler).reportError(ErrorLevel::Warning, ErrorCategory::Category, Message)

#define MPI_SANITIZER_INFO(Handler, Category, Message) \
  (Handler).reportError(ErrorLevel::Info, ErrorCategory::Category, Message)

#define MPI_SANITIZER_FATAL(Handler, Category, Message) \
  (Handler).reportError(ErrorLevel::Fatal, ErrorCategory::Category, Message)

#define MPI_SANITIZER_ERROR_LOC(Handler, Category, Message, Inst) \
  (Handler).reportError(ErrorLevel::Error, ErrorCategory::Category, Message, Inst)

#define MPI_SANITIZER_WARNING_LOC(Handler, Category, Message, Inst) \
  (Handler).reportError(ErrorLevel::Warning, ErrorCategory::Category, Message, Inst)

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_ERRORHANDLER_H
