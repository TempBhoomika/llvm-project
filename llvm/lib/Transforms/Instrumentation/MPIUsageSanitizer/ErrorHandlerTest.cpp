//===- ErrorHandlerTest.cpp - MPI Sanitizer Error Handler Tests ---------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file contains unit tests for the ErrorHandler class.
//
//===----------------------------------------------------------------------===//

#include "ErrorHandler.h"
#include "MPICallDetector.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Support/raw_ostream.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

class ErrorHandlerTest : public ::testing::Test {
protected:
  void SetUp() override {
    Context = std::make_unique<LLVMContext>();
    Handler = std::make_unique<ErrorHandler>(*Context);
    
    // Create a simple module for testing
    Module = std::make_unique<llvm::Module>("test_module", *Context);
    
    // Create a test function
    FunctionType* FT = FunctionType::get(Type::getVoidTy(*Context), false);
    TestFunc = Function::Create(FT, Function::ExternalLinkage, "test_func", Module.get());
    BasicBlock* BB = BasicBlock::Create(*Context, "entry", TestFunc);
    
    // Create a test instruction
    IRBuilder<> Builder(BB);
    TestInst = Builder.CreateRetVoid();
  }
  
  void TearDown() override {
    Handler.reset();
    Module.reset();
    Context.reset();
  }
  
  std::unique_ptr<LLVMContext> Context;
  std::unique_ptr<ErrorHandler> Handler;
  std::unique_ptr<llvm::Module> Module;
  Function* TestFunc = nullptr;
  Instruction* TestInst = nullptr;
};

TEST_F(ErrorHandlerTest, BasicErrorReporting) {
  // Test basic error reporting
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, "Test warning message");
  
  const auto& Stats = Handler->getStatistics();
  EXPECT_EQ(Stats.WarningCount, 1u);
  EXPECT_EQ(Stats.ErrorCount, 0u);
  EXPECT_EQ(Stats.getTotalCount(), 1u);
  
  const auto& Errors = Handler->getErrors();
  EXPECT_EQ(Errors.size(), 1u);
  EXPECT_EQ(Errors[0].Level, ErrorLevel::Warning);
  EXPECT_EQ(Errors[0].Category, ErrorCategory::CallDetection);
  EXPECT_EQ(Errors[0].Message, "Test warning message");
}

TEST_F(ErrorHandlerTest, ErrorReportingWithInstruction) {
  // Test error reporting with instruction context
  Handler->reportError(ErrorLevel::Error, ErrorCategory::HookInsertion, 
                       "Hook insertion failed", TestInst);
  
  const auto& Stats = Handler->getStatistics();
  EXPECT_EQ(Stats.ErrorCount, 1u);
  
  const auto& Errors = Handler->getErrors();
  EXPECT_EQ(Errors.size(), 1u);
  EXPECT_EQ(Errors[0].Inst, TestInst);
  EXPECT_EQ(Errors[0].Func, TestFunc);
}

TEST_F(ErrorHandlerTest, ErrorReportingWithFunction) {
  // Test error reporting with function context
  Handler->reportError(ErrorLevel::Info, ErrorCategory::StaticAnalysis, 
                       "Analysis completed", TestFunc);
  
  const auto& Stats = Handler->getStatistics();
  EXPECT_EQ(Stats.InfoCount, 1u);
  
  const auto& Errors = Handler->getErrors();
  EXPECT_EQ(Errors.size(), 1u);
  EXPECT_EQ(Errors[0].Func, TestFunc);
}

TEST_F(ErrorHandlerTest, MPISpecificErrorReporting) {
  // Create a test call site
  CallBase Site;
  Site.FunctionName = "MPI_Send";
  Site.Type = MPIFunctionType::PointToPoint;
  Site.Inst = TestInst;
  
  Handler->reportMPIError(ErrorLevel::Warning, "Invalid MPI usage detected", Site);
  
  const auto& Errors = Handler->getErrors();
  EXPECT_EQ(Errors.size(), 1u);
  EXPECT_EQ(Errors[0].Category, ErrorCategory::CallDetection);
  
  // Check MPI-specific context
  auto ContextIt = Errors[0].Context.find("mpi_function");
  EXPECT_NE(ContextIt, Errors[0].Context.end());
  EXPECT_EQ(ContextIt->second, "MPI_Send");
}

TEST_F(ErrorHandlerTest, ConfigurationErrorReporting) {
  Handler->reportConfigError(ErrorLevel::Error, "Invalid configuration value", "mpi.level");
  
  const auto& Errors = Handler->getErrors();
  EXPECT_EQ(Errors.size(), 1u);
  EXPECT_EQ(Errors[0].Category, ErrorCategory::Configuration);
  
  auto ContextIt = Errors[0].Context.find("config_key");
  EXPECT_NE(ContextIt, Errors[0].Context.end());
  EXPECT_EQ(ContextIt->second, "mpi.level");
}

TEST_F(ErrorHandlerTest, HookErrorReporting) {
  Handler->reportHookError(ErrorLevel::Fatal, "Failed to insert pre-call hook", 
                           TestInst, "mpi_pre_call_hook");
  
  const auto& Errors = Handler->getErrors();
  EXPECT_EQ(Errors.size(), 1u);
  EXPECT_EQ(Errors[0].Category, ErrorCategory::HookInsertion);
  EXPECT_EQ(Errors[0].Level, ErrorLevel::Fatal);
  
  auto ContextIt = Errors[0].Context.find("hook_name");
  EXPECT_NE(ContextIt, Errors[0].Context.end());
  EXPECT_EQ(ContextIt->second, "mpi_pre_call_hook");
}

TEST_F(ErrorHandlerTest, AnalysisErrorReporting) {
  Handler->reportAnalysisError(ErrorLevel::Warning, "Potential deadlock detected", 
                               TestFunc, "DeadlockAnalyzer");
  
  const auto& Errors = Handler->getErrors();
  EXPECT_EQ(Errors.size(), 1u);
  EXPECT_EQ(Errors[0].Category, ErrorCategory::StaticAnalysis);
  
  auto ContextIt = Errors[0].Context.find("analysis_type");
  EXPECT_NE(ContextIt, Errors[0].Context.end());
  EXPECT_EQ(ContextIt->second, "DeadlockAnalyzer");
}

TEST_F(ErrorHandlerTest, ErrorRecoveryPolicy) {
  // Test default recovery policy
  EXPECT_TRUE(Handler->shouldContinueAfterError(ErrorLevel::Info));
  EXPECT_TRUE(Handler->shouldContinueAfterError(ErrorLevel::Warning));
  EXPECT_FALSE(Handler->shouldContinueAfterError(ErrorLevel::Error));
  EXPECT_FALSE(Handler->shouldContinueAfterError(ErrorLevel::Fatal));
  
  // Change recovery policy
  Handler->setErrorRecoveryPolicy(true, true);
  EXPECT_TRUE(Handler->shouldContinueAfterError(ErrorLevel::Error));
  EXPECT_FALSE(Handler->shouldContinueAfterError(ErrorLevel::Fatal));
  
  // Test with error context
  MPIErrorInfo Error(ErrorLevel::Error, ErrorCategory::UnsupportedPattern, "Unsupported MPI pattern");
  EXPECT_TRUE(Handler->shouldContinueAfterError(Error)); // Unsupported patterns can be skipped
  
  MPIErrorInfo FatalError(ErrorLevel::Error, ErrorCategory::PassInfrastructure, "Infrastructure failure");
  EXPECT_FALSE(Handler->shouldContinueAfterError(FatalError)); // Infrastructure errors are serious
}

TEST_F(ErrorHandlerTest, ErrorStatistics) {
  // Report various errors
  Handler->reportError(ErrorLevel::Info, ErrorCategory::CallDetection, "Info message");
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::MetadataExtraction, "Warning message");
  Handler->reportError(ErrorLevel::Error, ErrorCategory::HookInsertion, "Error message");
  Handler->reportError(ErrorLevel::Fatal, ErrorCategory::Configuration, "Fatal message");
  
  const auto& Stats = Handler->getStatistics();
  EXPECT_EQ(Stats.InfoCount, 1u);
  EXPECT_EQ(Stats.WarningCount, 1u);
  EXPECT_EQ(Stats.ErrorCount, 1u);
  EXPECT_EQ(Stats.FatalCount, 1u);
  EXPECT_EQ(Stats.getTotalCount(), 4u);
  EXPECT_TRUE(Stats.hasErrors());
  EXPECT_TRUE(Stats.hasFatalErrors());
  
  // Check category counts
  EXPECT_EQ(Stats.CategoryCounts.lookup("CallDetection"), 1u);
  EXPECT_EQ(Stats.CategoryCounts.lookup("MetadataExtraction"), 1u);
  EXPECT_EQ(Stats.CategoryCounts.lookup("HookInsertion"), 1u);
  EXPECT_EQ(Stats.CategoryCounts.lookup("Configuration"), 1u);
}

TEST_F(ErrorHandlerTest, ErrorFiltering) {
  // Report errors of different categories and levels
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, "Warning 1");
  Handler->reportError(ErrorLevel::Error, ErrorCategory::CallDetection, "Error 1");
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::HookInsertion, "Warning 2");
  Handler->reportError(ErrorLevel::Error, ErrorCategory::HookInsertion, "Error 2");
  
  // Test filtering by category
  auto CallDetectionErrors = Handler->getErrorsByCategory(ErrorCategory::CallDetection);
  EXPECT_EQ(CallDetectionErrors.size(), 2u);
  
  auto HookInsertionErrors = Handler->getErrorsByCategory(ErrorCategory::HookInsertion);
  EXPECT_EQ(HookInsertionErrors.size(), 2u);
  
  // Test filtering by level
  auto WarningErrors = Handler->getErrorsByLevel(ErrorLevel::Warning);
  EXPECT_EQ(WarningErrors.size(), 2u);
  
  auto ErrorLevelErrors = Handler->getErrorsByLevel(ErrorLevel::Error);
  EXPECT_EQ(ErrorLevelErrors.size(), 2u);
  
  // Test category/level checks
  EXPECT_TRUE(Handler->hasErrorCategory(ErrorCategory::CallDetection));
  EXPECT_TRUE(Handler->hasErrorCategory(ErrorCategory::HookInsertion));
  EXPECT_FALSE(Handler->hasErrorCategory(ErrorCategory::Configuration));
  
  EXPECT_TRUE(Handler->hasErrorLevel(ErrorLevel::Warning));
  EXPECT_TRUE(Handler->hasErrorLevel(ErrorLevel::Error));
  EXPECT_FALSE(Handler->hasErrorLevel(ErrorLevel::Fatal));
}

TEST_F(ErrorHandlerTest, ErrorMessageFormatting) {
  MPIErrorInfo Error(ErrorLevel::Error, ErrorCategory::CallDetection, "Test error message");
  Error.Func = TestFunc;
  Error.Context["test_key"] = "test_value";
  
  std::string FormattedMessage = Handler->formatErrorMessage(Error);
  
  EXPECT_NE(FormattedMessage.find("ERROR"), std::string::npos);
  EXPECT_NE(FormattedMessage.find("CallDetection"), std::string::npos);
  EXPECT_NE(FormattedMessage.find("Test error message"), std::string::npos);
  EXPECT_NE(FormattedMessage.find("test_func"), std::string::npos);
  EXPECT_NE(FormattedMessage.find("test_key=test_value"), std::string::npos);
}

TEST_F(ErrorHandlerTest, ErrorClearance) {
  // Report some errors
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, "Warning");
  Handler->reportError(ErrorLevel::Error, ErrorCategory::HookInsertion, "Error");
  
  EXPECT_EQ(Handler->getStatistics().getTotalCount(), 2u);
  EXPECT_EQ(Handler->getErrors().size(), 2u);
  
  // Clear errors
  Handler->clearErrors();
  
  EXPECT_EQ(Handler->getStatistics().getTotalCount(), 0u);
  EXPECT_EQ(Handler->getErrors().size(), 0u);
}

TEST_F(ErrorHandlerTest, StringParsing) {
  // Test error level parsing
  EXPECT_EQ(ErrorHandler::parseErrorLevel("info"), ErrorLevel::Info);
  EXPECT_EQ(ErrorHandler::parseErrorLevel("WARNING"), ErrorLevel::Warning);
  EXPECT_EQ(ErrorHandler::parseErrorLevel("Error"), ErrorLevel::Error);
  EXPECT_EQ(ErrorHandler::parseErrorLevel("FATAL"), ErrorLevel::Fatal);
  EXPECT_EQ(ErrorHandler::parseErrorLevel("invalid"), ErrorLevel::Error); // Default
  
  // Test error category parsing
  EXPECT_EQ(ErrorHandler::parseErrorCategory("CallDetection"), ErrorCategory::CallDetection);
  EXPECT_EQ(ErrorHandler::parseErrorCategory("HOOKINSERTION"), ErrorCategory::HookInsertion);
  EXPECT_EQ(ErrorHandler::parseErrorCategory("configuration"), ErrorCategory::Configuration);
  EXPECT_EQ(ErrorHandler::parseErrorCategory("invalid"), ErrorCategory::PassInfrastructure); // Default
}

TEST_F(ErrorHandlerTest, ErrorLimitHandling) {
  // Test that error collection respects limits
  Handler->setStatisticsCollection(true);
  
  // Report many errors (more than the limit)
  for (int i = 0; i < 1100; ++i) {
    Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, 
                         "Warning " + std::to_string(i));
  }
  
  // Should not exceed the maximum error count
  EXPECT_LE(Handler->getErrors().size(), 1000u);
  
  // But statistics should still be accurate
  EXPECT_EQ(Handler->getStatistics().WarningCount, 1100u);
}

TEST_F(ErrorHandlerTest, VerboseReporting) {
  // Test verbose reporting mode
  Handler->setVerboseReporting(true);
  
  // This would normally output to debug stream
  Handler->reportError(ErrorLevel::Info, ErrorCategory::CallDetection, "Verbose test message");
  
  // Just verify the error was recorded
  EXPECT_EQ(Handler->getErrors().size(), 1u);
}

TEST_F(ErrorHandlerTest, RecoveryStrategyDetermination) {
  // Test recovery strategy determination for different error types
  MPIErrorInfo InfraError(ErrorLevel::Error, ErrorCategory::PassInfrastructure, "Infrastructure failure");
  MPIErrorInfo UnsupportedError(ErrorLevel::Warning, ErrorCategory::UnsupportedPattern, "Unsupported pattern");
  MPIErrorInfo ConfigError(ErrorLevel::Error, ErrorCategory::Configuration, "Config error");
  
  RecoveryContext Context;
  Context.TotalErrorCount = 5;
  Context.CategoryErrorCount = 2;
  Context.IsCriticalPath = false;
  
  // Infrastructure errors should stop
  RecoveryStrategy InfraStrategy = Handler->determineRecoveryStrategy(InfraError, Context);
  EXPECT_EQ(InfraStrategy, RecoveryStrategy::Stop);
  
  // Unsupported patterns should be skipped
  RecoveryStrategy UnsupportedStrategy = Handler->determineRecoveryStrategy(UnsupportedError, Context);
  EXPECT_EQ(UnsupportedStrategy, RecoveryStrategy::SkipAndContinue);
  
  // Configuration errors should use fallback
  RecoveryStrategy ConfigStrategy = Handler->determineRecoveryStrategy(ConfigError, Context);
  EXPECT_EQ(ConfigStrategy, RecoveryStrategy::ContinueFallback);
}

TEST_F(ErrorHandlerTest, ErrorThresholds) {
  // Test error threshold functionality
  Handler->setErrorThreshold(ErrorCategory::CallDetection, 3);
  
  // Report errors up to threshold
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, "Error 1");
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, "Error 2");
  EXPECT_FALSE(Handler->isErrorThresholdExceeded(ErrorCategory::CallDetection));
  
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, "Error 3");
  EXPECT_TRUE(Handler->isErrorThresholdExceeded(ErrorCategory::CallDetection));
}

TEST_F(ErrorHandlerTest, GracefulDegradation) {
  // Test graceful degradation for unsupported patterns
  Handler->setGracefulDegradationMode(true);
  EXPECT_TRUE(Handler->isGracefulDegradationEnabled());
  
  MPIErrorInfo UnsupportedError(ErrorLevel::Error, ErrorCategory::UnsupportedPattern, "Complex pattern");
  bool CanContinue = Handler->handleUnsupportedPattern(UnsupportedError, "ComplexMPIPattern");
  
  EXPECT_TRUE(CanContinue);
  
  // Disable graceful degradation
  Handler->setGracefulDegradationMode(false);
  EXPECT_FALSE(Handler->isGracefulDegradationEnabled());
  
  bool CannotContinue = Handler->handleUnsupportedPattern(UnsupportedError, "AnotherPattern");
  EXPECT_FALSE(CannotContinue);
}

TEST_F(ErrorHandlerTest, RecoveryRecommendations) {
  // Test recovery recommendation generation
  MPIErrorInfo CallDetectionError(ErrorLevel::Error, ErrorCategory::CallDetection, "Detection failed");
  auto Recommendations = Handler->generateRecoveryRecommendations(CallDetectionError);
  
  EXPECT_FALSE(Recommendations.empty());
  EXPECT_TRUE(std::find(Recommendations.begin(), Recommendations.end(), "Check MPI function signatures") != Recommendations.end());
}

TEST_F(ErrorHandlerTest, RecoveryStatistics) {
  // Test recovery statistics tracking
  Handler->recordRecoveryAttempt(RecoveryStrategy::SkipAndContinue, true);
  Handler->recordRecoveryAttempt(RecoveryStrategy::SkipAndContinue, false);
  Handler->recordRecoveryAttempt(RecoveryStrategy::ContinueFallback, true);
  
  double SkipSuccessRate = Handler->getRecoverySuccessRate(RecoveryStrategy::SkipAndContinue);
  EXPECT_DOUBLE_EQ(SkipSuccessRate, 0.5); // 1 success out of 2 attempts
  
  double FallbackSuccessRate = Handler->getRecoverySuccessRate(RecoveryStrategy::ContinueFallback);
  EXPECT_DOUBLE_EQ(FallbackSuccessRate, 1.0); // 1 success out of 1 attempt
}

TEST_F(ErrorHandlerTest, RecoveryContextCreation) {
  // Test recovery context creation
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, "Previous error");
  
  MPIErrorInfo CurrentError(ErrorLevel::Error, ErrorCategory::CallDetection, "Current error");
  RecoveryContext Context = Handler->createRecoveryContext(CurrentError, "test_phase");
  
  EXPECT_EQ(Context.CurrentError, &CurrentError);
  EXPECT_EQ(Context.ProcessingPhase, "test_phase");
  EXPECT_EQ(Context.TotalErrorCount, 1u); // One previous error
  EXPECT_EQ(Context.CategoryErrorCount, 1u); // One error in CallDetection category
  EXPECT_FALSE(Context.IsCriticalPath); // CallDetection is not critical path
  EXPECT_FALSE(Context.FallbackOptions.empty()); // Should have recommendations
}

TEST_F(ErrorHandlerTest, ErrorSeverityInContext) {
  MPIErrorInfo CriticalError(ErrorLevel::Error, ErrorCategory::PassInfrastructure, "Critical failure");
  MPIErrorInfo NonCriticalError(ErrorLevel::Warning, ErrorCategory::UnsupportedPattern, "Non-critical issue");
  
  RecoveryContext CriticalContext;
  CriticalContext.IsCriticalPath = true;
  CriticalContext.CategoryErrorCount = 2;
  
  RecoveryContext NonCriticalContext;
  NonCriticalContext.IsCriticalPath = false;
  NonCriticalContext.CategoryErrorCount = 1;
  
  EXPECT_TRUE(Handler->isErrorSevereInContext(CriticalError, CriticalContext));
  EXPECT_FALSE(Handler->isErrorSevereInContext(NonCriticalError, NonCriticalContext));
}

TEST_F(ErrorHandlerTest, AlternativeApproaches) {
  // Test alternative approach availability
  EXPECT_TRUE(Handler->hasAlternativeApproaches(ErrorCategory::CallDetection));
  EXPECT_TRUE(Handler->hasAlternativeApproaches(ErrorCategory::MetadataExtraction));
  EXPECT_TRUE(Handler->hasAlternativeApproaches(ErrorCategory::HookInsertion));
  EXPECT_TRUE(Handler->hasAlternativeApproaches(ErrorCategory::StaticAnalysis));
  EXPECT_TRUE(Handler->hasAlternativeApproaches(ErrorCategory::Configuration));
  
  EXPECT_FALSE(Handler->hasAlternativeApproaches(ErrorCategory::PassInfrastructure));
  EXPECT_FALSE(Handler->hasAlternativeApproaches(ErrorCategory::RuntimeInterface));
}

TEST_F(ErrorHandlerTest, EnhancedStatisticsPrinting) {
  // Test enhanced statistics printing with recovery data
  Handler->setErrorThreshold(ErrorCategory::CallDetection, 5);
  Handler->reportError(ErrorLevel::Warning, ErrorCategory::CallDetection, "Warning");
  Handler->recordRecoveryAttempt(RecoveryStrategy::SkipAndContinue, true);
  Handler->handleUnsupportedPattern(MPIErrorInfo(ErrorLevel::Info, ErrorCategory::UnsupportedPattern, "Test"), "TestPattern");
  
  std::string Output;
  raw_string_ostream OS(Output);
  Handler->printStatistics(OS);
  
  std::string Result = OS.str();
  
  // Check that enhanced statistics are included
  EXPECT_NE(Result.find("threshold"), std::string::npos);
  EXPECT_NE(Result.find("Recovery Statistics"), std::string::npos);
  EXPECT_NE(Result.find("Unsupported Patterns"), std::string::npos);
  EXPECT_NE(Result.find("Graceful Degradation"), std::string::npos);
}

} // anonymous namespace
