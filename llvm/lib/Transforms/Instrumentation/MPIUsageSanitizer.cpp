//===- MPIUsageSanitizer.cpp - MPI Usage Sanitizer instrumentation -------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements the MPI Usage Sanitizer pass which instruments MPI
// programs to enable runtime error detection and performance monitoring.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/MPIUsageSanitizer.h"
#include "MPIUsageSanitizer/MPICallDetector.h"
#include "MPIUsageSanitizer/MetadataExtractor.h"
#include "MPIUsageSanitizer/HookInserter.h"
#include "MPIUsageSanitizer/StaticAnalyzer.h"
#include "MPIUsageSanitizer/ConfigurationManager.h"
#include "MPIUsageSanitizer/ErrorHandler.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include <memory>
#include <chrono>

using namespace llvm;

#define DEBUG_TYPE "mpi-sanitizer"

// Command line options for MPI sanitizer configuration
cl::opt<bool> ClEnableOptimizations(
    "mpi-sanitizer-enable-optimizations",
    cl::desc("Enable compile-time optimizations for MPI sanitizer"),
    cl::Hidden, cl::init(true));

cl::opt<bool> ClEnablePerformanceMonitoring(
    "mpi-sanitizer-enable-performance",
    cl::desc("Enable performance monitoring hooks"),
    cl::Hidden, cl::init(false));

cl::opt<bool> ClEnableDeadlockDetection(
    "mpi-sanitizer-enable-deadlock-detection",
    cl::desc("Enable deadlock detection analysis"),
    cl::Hidden, cl::init(true));

cl::opt<bool> ClEnableDataRaceDetection(
    "mpi-sanitizer-enable-datarace-detection",
    cl::desc("Enable data race detection analysis"),
    cl::Hidden, cl::init(true));

cl::opt<std::string> ClInstrumentationLevel(
    "mpi-sanitizer-level",
    cl::desc("Instrumentation level: full, lightweight, or performance"),
    cl::Hidden, cl::init("full"));

cl::opt<std::string> ClConfigFile(
    "mpi-sanitizer-config",
    cl::desc("Configuration file for MPI sanitizer options"),
    cl::Hidden, cl::init(""));

cl::opt<bool> ClVerbose(
    "mpi-sanitizer-verbose",
    cl::desc("Enable verbose output for MPI sanitizer"),
    cl::Hidden, cl::init(false));

cl::opt<bool> ClStatistics(
    "mpi-sanitizer-statistics",
    cl::desc("Print instrumentation statistics"),
    cl::Hidden, cl::init(false));

namespace {

/// Convert string instrumentation level to enum
MPIUsageSanitizerOptions::InstrumentationLevel
parseInstrumentationLevel(StringRef Level) {
  if (Level == "full")
    return MPIUsageSanitizerOptions::InstrumentationLevel::Full;
  else if (Level == "lightweight")
    return MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight;
  else if (Level == "performance")
    return MPIUsageSanitizerOptions::InstrumentationLevel::Performance;
  else {
    errs() << "Warning: Unknown instrumentation level '" << Level 
           << "', using 'full'\n";
    return MPIUsageSanitizerOptions::InstrumentationLevel::Full;
  }
}

/// Create options from command line arguments
MPIUsageSanitizerOptions createOptionsFromCommandLine() {
  MPIUsageSanitizerOptions Options;
  Options.Level = parseInstrumentationLevel(ClInstrumentationLevel);
  Options.EnableOptimizations = ClEnableOptimizations;
  Options.EnablePerformanceMonitoring = ClEnablePerformanceMonitoring;
  Options.EnableDeadlockDetection = ClEnableDeadlockDetection;
  Options.EnableDataRaceDetection = ClEnableDataRaceDetection;
  Options.ConfigFile = ClConfigFile;
  return Options;
}

/// Statistics tracking for instrumentation
struct InstrumentationStatistics {
  unsigned TotalFunctions = 0;
  unsigned FunctionsWithMPI = 0;
  unsigned TotalMPICalls = 0;
  unsigned InstrumentedCalls = 0;
  unsigned OptimizedCalls = 0;
  unsigned SkippedCalls = 0;
  unsigned ErrorsEncountered = 0;
  std::chrono::milliseconds ProcessingTime{0};
  
  void print(raw_ostream &OS) const {
    OS << "MPI Usage Sanitizer Statistics:\n";
    OS << "  Total functions processed: " << TotalFunctions << "\n";
    OS << "  Functions with MPI calls: " << FunctionsWithMPI << "\n";
    OS << "  Total MPI calls found: " << TotalMPICalls << "\n";
    OS << "  MPI calls instrumented: " << InstrumentedCalls << "\n";
    OS << "  MPI calls optimized: " << OptimizedCalls << "\n";
    OS << "  MPI calls skipped: " << SkippedCalls << "\n";
    OS << "  Errors encountered: " << ErrorsEncountered << "\n";
    OS << "  Processing time: " << ProcessingTime.count() << " ms\n";
    if (TotalMPICalls > 0) {
      OS << "  Instrumentation coverage: " 
         << (100.0 * InstrumentedCalls / TotalMPICalls) << "%\n";
      OS << "  Optimization rate: "
         << (100.0 * OptimizedCalls / TotalMPICalls) << "%\n";
    }
  }
};

/// Custom diagnostic for MPI sanitizer issues
class MPISanitizerDiagnosticInfo : public DiagnosticInfo {
private:
  std::string Message;
  StringRef FunctionName;
  DebugLoc Loc;

public:
  MPISanitizerDiagnosticInfo(DiagnosticSeverity Severity, StringRef Msg,
                            StringRef FuncName = "", DebugLoc Location = {})
      : DiagnosticInfo(DK_Unsupported, Severity), Message(Msg.str()),
        FunctionName(FuncName), Loc(Location) {}

  void print(DiagnosticPrinter &DP) const override {
    DP << "MPI Usage Sanitizer: " << Message;
    if (!FunctionName.empty()) {
      DP << " in function '" << FunctionName << "'";
    }
    if (Loc) {
      DP << " at " << Loc->getFilename() << ":" << Loc.getLine();
    }
  }

  static bool classof(const DiagnosticInfo *DI) {
    return DI->getKind() == DK_Unsupported;
  }
};

} // anonymous namespace

//===----------------------------------------------------------------------===//
// MPIUsageSanitizerPass Implementation
//===----------------------------------------------------------------------===//

MPIUsageSanitizerPass::MPIUsageSanitizerPass(const MPIUsageSanitizerOptions &Options)
    : Options(Options) {
  // If no options provided, use command line options
  if (Options.Level == MPIUsageSanitizerOptions::InstrumentationLevel::Full &&
      !Options.EnableOptimizations && !Options.EnablePerformanceMonitoring &&
      Options.ConfigFile.empty()) {
    this->Options = createOptionsFromCommandLine();
  }
  
  LLVM_DEBUG(dbgs() << "MPIUsageSanitizerPass initialized with level: " 
                    << (int)this->Options.Level << "\n");
}

PreservedAnalyses MPIUsageSanitizerPass::run(Module &M, ModuleAnalysisManager &AM) {
  LLVM_DEBUG(dbgs() << "Running MPI Usage Sanitizer on module: " << M.getName() << "\n");
  
  auto StartTime = std::chrono::high_resolution_clock::now();
  bool ModuleChanged = false;
  InstrumentationStatistics Stats;
  
  // Initialize error handler for diagnostics
  ErrorHandler ErrHandler(M.getContext());
  
  // Initialize configuration manager
  ConfigurationManager ConfigMgr(Options);
  if (!ConfigMgr.initialize()) {
    ErrHandler.reportError("Failed to initialize configuration");
    Stats.ErrorsEncountered++;
    return PreservedAnalyses::all();
  }
  
  // Initialize components with error handling
  std::unique_ptr<MPICallDetector> CallDetector;
  std::unique_ptr<MetadataExtractor> MetadataExt;
  std::unique_ptr<StaticAnalyzer> Analyzer;
  std::unique_ptr<HookInserter> Inserter;
  
  CallDetector = std::make_unique<MPICallDetector>();
  MetadataExt = std::make_unique<MetadataExtractor>();
  
  // Configure static analyzer based on options
  if (Options.EnableOptimizations) {
    Analyzer = std::make_unique<StaticAnalyzer>();
  }
  
  // Configure hook insertion based on options
  HookConfiguration HookConfig;
  switch (Options.Level) {
    case MPIUsageSanitizerOptions::InstrumentationLevel::Full:
      HookConfig.Level = InstrumentationLevel::Full;
      break;
    case MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight:
      HookConfig.Level = InstrumentationLevel::Minimal;
      break;
    case MPIUsageSanitizerOptions::InstrumentationLevel::Performance:
      HookConfig.Level = InstrumentationLevel::Selective;
      HookConfig.EnablePerformanceHooks = true;
      break;
  }
  HookConfig.EnablePerformanceHooks = Options.EnablePerformanceMonitoring;
  
  Inserter = std::make_unique<HookInserter>(HookConfig);
  
  // Initialize detector with module context
  CallDetector->initialize(M);
  Inserter->setModule(&M);
  
  // Create hook function declarations
  Inserter->createHookDeclarations(M);
  
  // Process each function in the module
  for (Function &F : M) {
    if (F.isDeclaration()) {
      continue;
    }
    
    Stats.TotalFunctions++;
    
    LLVM_DEBUG(dbgs() << "Processing function: " << F.getName() << "\n");
    
    // Detect MPI calls in this function
    std::vector<CallSite> MPICalls = CallDetector->detectMPICalls(F);
    Stats.TotalMPICalls += MPICalls.size();
    
    if (MPICalls.empty()) {
      continue;
    }
    
    Stats.FunctionsWithMPI++;
    
    LLVM_DEBUG(dbgs() << "Found " << MPICalls.size() << " MPI calls in " 
                      << F.getName() << "\n");
    
    // Extract metadata for each call site
    std::vector<std::pair<CallSite, MPICallMetadata>> CallsWithMetadata;
    for (auto &Site : MPICalls) {
      MPICallMetadata Metadata = MetadataExt->extractMetadata(Site);
      CallsWithMetadata.emplace_back(Site, std::move(Metadata));
    }
    
    // Perform static analysis if enabled
    if (Analyzer && Options.EnableOptimizations) {
      for (auto& [Site, Metadata] : CallsWithMetadata) {
        if (Analyzer->isProvablySafe(Site, Metadata)) {
          Stats.OptimizedCalls++;
          LLVM_DEBUG(dbgs() << "Optimizing safe MPI call: " << Site.FunctionName << "\n");
        }
      }
    }
    
    // Insert hooks for MPI calls
    std::vector<CallSite> SitesToInstrument;
    for (const auto& [Site, Metadata] : CallsWithMetadata) {
      // Skip instrumentation for provably safe calls if optimization is enabled
      if (Analyzer && Options.EnableOptimizations && 
          Analyzer->isProvablySafe(Site, Metadata)) {
        Stats.SkippedCalls++;
        continue;
      }
      SitesToInstrument.push_back(Site);
    }
    
    if (!SitesToInstrument.empty() && Inserter->insertHooks(F, SitesToInstrument)) {
      ModuleChanged = true;
      Stats.InstrumentedCalls += SitesToInstrument.size();
      
      LLVM_DEBUG(dbgs() << "Instrumented " << SitesToInstrument.size() 
                        << " MPI calls in " << F.getName() << "\n");
    }
  }
  
  // Calculate processing time
  auto EndTime = std::chrono::high_resolution_clock::now();
  Stats.ProcessingTime = std::chrono::duration_cast<std::chrono::milliseconds>(
      EndTime - StartTime);
  
  // Print statistics if requested
  if (ClStatistics || ClVerbose) {
    Stats.print(outs());
  }
  
  // Report summary diagnostics
  if (Stats.TotalMPICalls > 0) {
    std::string SummaryMsg = "Processed " + std::to_string(Stats.TotalMPICalls) + 
                            " MPI calls, instrumented " + std::to_string(Stats.InstrumentedCalls);
    if (Stats.ErrorsEncountered > 0) {
      SummaryMsg += ", encountered " + std::to_string(Stats.ErrorsEncountered) + " errors";
    }
    
    DiagnosticSeverity Severity = Stats.ErrorsEncountered > 0 ? 
        DiagnosticSeverity::DS_Warning : DiagnosticSeverity::DS_Remark;

    MPIErrorInfo Err(
    Severity,
    ErrorCategory::General,
    SummaryMsg);  
    M.getContext().diagnose(MPISanitizerDiagnosticInfo(Err));
  }
  
  LLVM_DEBUG(dbgs() << "MPI Usage Sanitizer processed " << Stats.TotalMPICalls 
                    << " MPI calls, instrumented " << Stats.InstrumentedCalls 
                    << " in " << Stats.ProcessingTime.count() << " ms\n");
  
  if (ModuleChanged) {
    LLVM_DEBUG(dbgs() << "MPI Usage Sanitizer modified the module\n");
    
    // Preserve analyses that are not invalidated by our transformations
    PreservedAnalyses PA;
    PA.preserve<TargetLibraryAnalysis>();
    PA.preserve<CallGraphAnalysis>();
    
    // We may have modified the CFG by inserting calls, but we preserve
    // the overall structure
    PA.preserveSet<CFGAnalyses>();
    
    return PA;
  }
  
  return PreservedAnalyses::all();
}

void MPIUsageSanitizerPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
  static_cast<void>(MapClassName2PassName);
  OS << "mpi-sanitizer";
}

//===----------------------------------------------------------------------===//
// MPIUsageSanitizerFunctionPass Implementation
//===----------------------------------------------------------------------===//

MPIUsageSanitizerFunctionPass::MPIUsageSanitizerFunctionPass(
    const MPIUsageSanitizerOptions &Options)
    : Options(Options) {
  if (Options.Level == MPIUsageSanitizerOptions::InstrumentationLevel::Full &&
      !Options.EnableOptimizations && !Options.EnablePerformanceMonitoring &&
      Options.ConfigFile.empty()) {
    this->Options = createOptionsFromCommandLine();
  }
}

PreservedAnalyses MPIUsageSanitizerFunctionPass::run(Function &F, 
                                                     FunctionAnalysisManager &AM) {
  if (F.isDeclaration())
    return PreservedAnalyses::all();
    
  LLVM_DEBUG(dbgs() << "Running MPI Usage Sanitizer on function: " << F.getName() << "\n");
  
  auto StartTime = std::chrono::high_resolution_clock::now();
  
  // Initialize error handler
  ErrorHandler ErrHandler(F.getContext());
  
  // Initialize components with error handling
  std::unique_ptr<MPICallDetector> CallDetector;
  std::unique_ptr<MetadataExtractor> MetadataExt;
  std::unique_ptr<StaticAnalyzer> Analyzer;
  std::unique_ptr<HookInserter> Inserter;
  
  CallDetector = std::make_unique<MPICallDetector>();
  MetadataExt = std::make_unique<MetadataExtractor>();
  
  if (Options.EnableOptimizations) {
    Analyzer = std::make_unique<StaticAnalyzer>();
  }
  
  // Configure hook insertion
  HookConfiguration HookConfig;
  switch (Options.Level) {
    case MPIUsageSanitizerOptions::InstrumentationLevel::Full:
      HookConfig.Level = InstrumentationLevel::Full;
      break;
    case MPIUsageSanitizerOptions::InstrumentationLevel::Lightweight:
      HookConfig.Level = InstrumentationLevel::Minimal;
      break;
    case MPIUsageSanitizerOptions::InstrumentationLevel::Performance:
      HookConfig.Level = InstrumentationLevel::Selective;
      HookConfig.EnablePerformanceHooks = true;
      break;
  }
  HookConfig.EnablePerformanceHooks = Options.EnablePerformanceMonitoring;
  
  Inserter = std::make_unique<HookInserter>(HookConfig);
  
  // Initialize with module context
  Module* M = F.getParent();
  CallDetector->initialize(*M);
  Inserter->setModule(M);
  
  // Get alias analysis results for enhanced indirect call detection
  AAResults* AA = nullptr;
  if (AM.getCachedResult<AAManager>(F)) {
    AA = &AM.getResult<AAManager>(F);
  }
  
  // Detect MPI calls in this function
  std::vector<CallSite> MPICalls = CallDetector->detectMPICalls(F, AA);
  
  if (MPICalls.empty()) {
    return PreservedAnalyses::all();
  }
    
  LLVM_DEBUG(dbgs() << "Found " << MPICalls.size() << " MPI calls in " 
                    << F.getName() << "\n");
  
  // Create hook declarations if needed (should be done at module level)
  Inserter->createHookDeclarations(*M);
  
  // Extract metadata and perform analysis
  std::vector<CallSite> SitesToInstrument;
  for (const auto& Site : MPICalls) {
    MPICallMetadata Metadata = MetadataExt->extractMetadata(Site);
    
    // Skip instrumentation for provably safe calls if optimization is enabled
    if (Analyzer && Options.EnableOptimizations && 
        Analyzer->isProvablySafe(Site, Metadata)) {
      LLVM_DEBUG(dbgs() << "Skipping safe MPI call: " << Site.FunctionName << "\n");
      continue;
    }
    
    SitesToInstrument.push_back(Site);
  }
  
  // Insert hooks for selected MPI calls
  if (!SitesToInstrument.empty() && Inserter->insertHooks(F, SitesToInstrument)) {
    auto EndTime = std::chrono::high_resolution_clock::now();
    auto ProcessingTime = std::chrono::duration_cast<std::chrono::milliseconds>(
        EndTime - StartTime);
    
    LLVM_DEBUG(dbgs() << "MPI Usage Sanitizer instrumented " << SitesToInstrument.size() 
                      << " calls in function " << F.getName() 
                      << " in " << ProcessingTime.count() << " ms\n");
    
    // Preserve analyses that are not invalidated
    PreservedAnalyses PA;
    PA.preserve<DominatorTreeAnalysis>();
    PA.preserve<LoopAnalysis>();
    PA.preserve<ScalarEvolutionAnalysis>();
    PA.preserve<PostDominatorTreeAnalysis>();
    
    return PA;
  }
  
  return PreservedAnalyses::all();
}

void MPIUsageSanitizerFunctionPass::printPipeline(
    raw_ostream &OS, function_ref<StringRef(StringRef)> MapClassName2PassName) {
  static_cast<void>(MapClassName2PassName);
  OS << "mpi-sanitizer-function";
}

//===----------------------------------------------------------------------===//
// Legacy Pass Manager Support
//===----------------------------------------------------------------------===//

namespace llvm {
// Forward declarations for pass initialization
void initializeMPIUsageSanitizerLegacyPassPass(PassRegistry&);
void initializeMPIUsageSanitizerFunctionLegacyPassPass(PassRegistry&);
} // namespace llvm

namespace {

/// Legacy pass manager wrapper for MPIUsageSanitizerPass
class MPIUsageSanitizerLegacyPass : public ModulePass {
public:
  static char ID;
  
  MPIUsageSanitizerLegacyPass() : ModulePass(ID) {
    llvm::initializeMPIUsageSanitizerLegacyPassPass(*PassRegistry::getPassRegistry());
  }
  
  MPIUsageSanitizerLegacyPass(const MPIUsageSanitizerOptions &Options)
      : ModulePass(ID), Options(Options) {
    llvm::initializeMPIUsageSanitizerLegacyPassPass(*PassRegistry::getPassRegistry());
  }
  
  bool runOnModule(Module &M) override {
    ModuleAnalysisManager DummyMAM;
    MPIUsageSanitizerPass Pass(Options);
    auto PA = Pass.run(M, DummyMAM);
    return !PA.areAllPreserved();
  }
  
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
  
  StringRef getPassName() const override {
    return "MPI Usage Sanitizer (Legacy)";
  }

private:
  MPIUsageSanitizerOptions Options;
};

/// Legacy pass manager wrapper for MPIUsageSanitizerFunctionPass
class MPIUsageSanitizerFunctionLegacyPass : public FunctionPass {
public:
  static char ID;
  
  MPIUsageSanitizerFunctionLegacyPass() : FunctionPass(ID) {
    llvm::initializeMPIUsageSanitizerFunctionLegacyPassPass(*PassRegistry::getPassRegistry());
  }
  
  MPIUsageSanitizerFunctionLegacyPass(const MPIUsageSanitizerOptions &Options)
      : FunctionPass(ID), Options(Options) {
    llvm::initializeMPIUsageSanitizerFunctionLegacyPassPass(*PassRegistry::getPassRegistry());
  }
  
  bool runOnFunction(Function &F) override {
    FunctionAnalysisManager DummyFAM;
    MPIUsageSanitizerFunctionPass Pass(Options);
    auto PA = Pass.run(F, DummyFAM);
    return !PA.areAllPreserved();
  }
  
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
  }
  
  StringRef getPassName() const override {
    return "MPI Usage Sanitizer Function (Legacy)";
  }

private:
  MPIUsageSanitizerOptions Options;
};

} // anonymous namespace

char MPIUsageSanitizerLegacyPass::ID = 0;
char MPIUsageSanitizerFunctionLegacyPass::ID = 0;

// Legacy pass registration
INITIALIZE_PASS(MPIUsageSanitizerLegacyPass, "mpi-sanitizer-legacy",
                "MPI Usage Sanitizer (Legacy Pass Manager)", false, false)

INITIALIZE_PASS(MPIUsageSanitizerFunctionLegacyPass, "mpi-sanitizer-function-legacy",
                "MPI Usage Sanitizer Function (Legacy Pass Manager)", false, false)

//===----------------------------------------------------------------------===//
// Public Interface Functions
//===----------------------------------------------------------------------===//

/// Create a legacy module pass for MPI Usage Sanitizer
ModulePass *llvm::createMPIUsageSanitizerLegacyPass() {
  return new MPIUsageSanitizerLegacyPass();
}

/// Create a legacy module pass for MPI Usage Sanitizer with options
ModulePass *llvm::createMPIUsageSanitizerLegacyPass(const MPIUsageSanitizerOptions &Options) {
  return new MPIUsageSanitizerLegacyPass(Options);
}

/// Create a legacy function pass for MPI Usage Sanitizer
FunctionPass *llvm::createMPIUsageSanitizerFunctionLegacyPass() {
  return new MPIUsageSanitizerFunctionLegacyPass();
}

/// Create a legacy function pass for MPI Usage Sanitizer with options
FunctionPass *llvm::createMPIUsageSanitizerFunctionLegacyPass(const MPIUsageSanitizerOptions &Options) {
  return new MPIUsageSanitizerFunctionLegacyPass(Options);
}
