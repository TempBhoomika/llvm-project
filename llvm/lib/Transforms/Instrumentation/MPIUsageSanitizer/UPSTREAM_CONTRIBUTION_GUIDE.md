# MPI Usage Sanitizer LLVM Pass - Upstream Contribution Guide

## Overview

This document provides comprehensive guidance for contributing the MPI Usage Sanitizer LLVM Pass to the upstream LLVM project. It covers code standards, documentation requirements, testing protocols, and the contribution process.

## LLVM Coding Standards Compliance

### Code Style and Formatting

The MPI Usage Sanitizer Pass strictly follows LLVM coding standards:

#### Naming Conventions
- **Classes**: PascalCase (e.g., `MPISanitizerPass`, `MetadataExtractor`)
- **Functions**: camelCase (e.g., `detectMPICalls`, `extractMetadata`)
- **Variables**: camelCase (e.g., `callSite`, `functionName`)
- **Constants**: UPPER_SNAKE_CASE (e.g., `MAX_CACHE_SIZE`)
- **Namespaces**: lowercase (e.g., `llvm`, `mpi_sanitizer`)

#### File Organization
```
llvm/lib/Transforms/Instrumentation/MPIUsageSanitizer/
├── MPISanitizerPass.h/cpp           # Main pass implementation
├── MPIFunctionDatabase.h/cpp        # MPI function signature database
├── MPICallDetector.h/cpp           # MPI call detection engine
├── MetadataExtractor.h/cpp         # Parameter metadata extraction
├── HookInserter.h/cpp              # Runtime hook insertion
├── StaticAnalyzer.h/cpp            # Static analysis framework
├── OptimizationEngine.h/cpp        # Performance optimization
├── ConfigurationManager.h/cpp      # Configuration management
├── ErrorHandler.h/cpp              # Error handling and diagnostics
├── PerformanceProfiler.h/cpp       # Performance profiling
├── PassOptimizer.h/cpp             # Pass optimization engine
└── test/                           # Comprehensive test suite
```

#### Header Guards and Includes
```cpp
#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPISANITIZERPASS_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPISANITIZERPASS_H

#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
// ... other includes

namespace llvm {
// Implementation
} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_MPISANITIZERPASS_H
```

### Documentation Standards

#### Doxygen Documentation
All public APIs include comprehensive Doxygen documentation:

```cpp
/// \brief MPI Usage Sanitizer Pass for runtime error detection and performance monitoring.
///
/// This pass instruments MPI function calls with runtime hooks to detect errors,
/// validate parameters, and monitor performance characteristics. It supports
/// multiple instrumentation modes and optimization strategies.
///
/// \see MPICallDetector for MPI call detection
/// \see MetadataExtractor for parameter analysis
/// \see HookInserter for runtime hook insertion
class MPISanitizerPass : public PassInfoMixin<MPISanitizerPass> {
public:
  /// \brief Run the MPI sanitizer pass on a module.
  ///
  /// \param M The module to instrument
  /// \param MAM Module analysis manager for accessing analyses
  /// \return PreservedAnalyses indicating which analyses are preserved
  PreservedAnalyses run(Module &M, ModuleAnalysisManager &MAM);
  
  /// \brief Get the pass name for debugging and logging.
  static StringRef name() { return "MPISanitizerPass"; }
};
```

#### Code Comments
- **Purpose**: Every function and class has a clear purpose statement
- **Algorithm**: Complex algorithms include step-by-step explanations
- **Edge Cases**: Special cases and error conditions are documented
- **Performance**: Performance implications are noted where relevant

### Error Handling and Diagnostics

#### LLVM Diagnostic Integration
```cpp
void MPISanitizerPass::emitDiagnostic(Function &F, StringRef Message, 
                                     DiagnosticSeverity Severity) {
  LLVMContext &Ctx = F.getContext();
  DiagnosticInfoOptimizationBase Diag(DEBUG_TYPE, F, F.getSubprogram(),
                                     Message, Severity);
  Ctx.diagnose(Diag);
}
```

#### Graceful Error Recovery
- Non-fatal errors allow pass to continue with reduced functionality
- Fatal errors provide clear diagnostic messages with source locations
- Error statistics are collected and reported for debugging

### Memory Management

#### LLVM Memory Management Patterns
- Use of `std::unique_ptr` and `std::shared_ptr` for automatic memory management
- RAII patterns for resource management
- Proper cleanup in destructors and error paths
- No raw pointer ownership (following LLVM guidelines)

## Testing Requirements

### Comprehensive Test Coverage

#### Unit Tests (GoogleTest)
```cpp
TEST_F(MPICallDetectorTest, DetectsBasicMPICalls) {
  auto M = createTestModule();
  MPICallDetector Detector;
  
  auto CallSites = Detector.detectMPICalls(*M->getFunction("test_function"));
  
  EXPECT_EQ(CallSites.size(), 3);
  EXPECT_EQ(CallSites[0].FunctionName, "MPI_Send");
  EXPECT_EQ(CallSites[1].FunctionName, "MPI_Recv");
  EXPECT_EQ(CallSites[2].FunctionName, "MPI_Finalize");
}
```

#### Lit Integration Tests
```llvm
; RUN: opt -load-pass-plugin %llvmshlibdir/LLVMMPIUsageSanitizer%shlibext \
; RUN:     -passes=mpi-sanitizer -S < %s | FileCheck %s

; CHECK-LABEL: @test_mpi_instrumentation
; CHECK: call{{.*}}@__mpi_sanitizer_pre_send
; CHECK: call i32 @MPI_Send
; CHECK: call{{.*}}@__mpi_sanitizer_post_send

define i32 @test_mpi_instrumentation() {
  ; Test implementation
}
```

#### Property-Based Tests
```cpp
PROPERTY_TEST(MPISanitizerPass, PreservesSemantics) {
  FORALL(module, RandomMPIModuleGenerator()) {
    auto OriginalModule = CloneModule(*module);
    auto InstrumentedModule = CloneModule(*module);
    
    // Apply sanitizer pass
    runMPISanitizerPass(*InstrumentedModule);
    
    // Verify semantic equivalence
    EXPECT_TRUE(semanticallyEquivalent(*OriginalModule, *InstrumentedModule));
  }
}
```

### Performance Validation
- Compilation time overhead < 5% for typical MPI applications
- Memory overhead < 10% during pass execution
- Scalability validation up to 10,000 function modules
- Regression testing against performance baselines

## Documentation Requirements

### User Documentation

#### README.md
```markdown
# MPI Usage Sanitizer LLVM Pass

A comprehensive LLVM transformation pass that instruments MPI programs for runtime error detection and performance monitoring.

## Features
- Runtime error detection for MPI operations
- Performance monitoring and bottleneck identification
- Multi-language support (C, C++, Fortran)
- Configurable instrumentation levels
- Integration with existing MPI applications

## Quick Start
```bash
# Compile with MPI sanitizer
clang -fpass-plugin=LLVMMPIUsageSanitizer.so -fmpi-sanitizer program.c -lmpi
```

#### User Guide
- Installation and setup instructions
- Configuration options and usage patterns
- Integration with build systems (CMake, Make, etc.)
- Troubleshooting common issues
- Performance tuning guidelines

#### Developer Guide
- Architecture overview and design principles
- API documentation and extension points
- Contributing guidelines and coding standards
- Testing procedures and validation methods
- Performance optimization techniques

### API Documentation

#### Doxygen Configuration
```doxygen
PROJECT_NAME           = "MPI Usage Sanitizer"
PROJECT_VERSION        = "1.0.0"
OUTPUT_DIRECTORY       = docs/
GENERATE_HTML          = YES
GENERATE_LATEX         = NO
EXTRACT_ALL            = YES
EXTRACT_PRIVATE        = NO
EXTRACT_STATIC         = YES
```

#### Sphinx Integration
- Integration with LLVM's Sphinx documentation system
- Cross-references to LLVM core documentation
- Code examples and tutorials
- Performance guides and best practices

## Contribution Process

### Pre-Submission Checklist

#### Code Quality
- [ ] All code follows LLVM coding standards
- [ ] Comprehensive Doxygen documentation for public APIs
- [ ] No compiler warnings with `-Wall -Wextra`
- [ ] Memory leaks detected with AddressSanitizer/Valgrind
- [ ] Thread safety validated with ThreadSanitizer

#### Testing
- [ ] All unit tests pass (`ninja check-mpi-sanitizer`)
- [ ] Integration tests pass on multiple platforms
- [ ] Property-based tests validate correctness properties
- [ ] Performance tests meet overhead requirements
- [ ] Regression tests pass against baseline

#### Documentation
- [ ] User guide updated with new features
- [ ] API documentation complete and accurate
- [ ] Code examples tested and verified
- [ ] Performance impact documented
- [ ] Migration guide for existing users

### Patch Series Organization

#### Logical Patch Breakdown
1. **Core Infrastructure** (Patches 1-3)
   - Basic pass structure and registration
   - MPI function database and call detection
   - Metadata extraction framework

2. **Hook Framework** (Patches 4-6)
   - Runtime hook insertion infrastructure
   - Multi-language support enhancements
   - Configuration management system

3. **Static Analysis** (Patches 7-9)
   - Static analysis framework
   - Optimization engine implementation
   - Performance profiling integration

4. **Error Handling** (Patches 10-12)
   - Comprehensive error handling
   - Runtime interface validation
   - Diagnostic integration

5. **Testing and Integration** (Patches 13-15)
   - Property-based testing framework
   - Performance optimization validation
   - LLVM build system integration

#### Patch Format
```
[PATCH 1/15] [Transforms] Add MPI Usage Sanitizer Pass infrastructure

This patch introduces the basic infrastructure for the MPI Usage Sanitizer
LLVM transformation pass, including pass registration, configuration
management, and integration with LLVM's pass manager.

The pass instruments MPI function calls with runtime hooks to detect
errors and monitor performance. This initial patch provides the
foundation for subsequent functionality.

Differential Revision: https://reviews.llvm.org/D12345
```

### Review Process

#### Code Review Guidelines
- **Incremental Reviews**: Submit patches in logical, reviewable chunks
- **Clear Descriptions**: Each patch has clear motivation and implementation details
- **Test Coverage**: Every patch includes appropriate tests
- **Performance Impact**: Document any performance implications
- **Backward Compatibility**: Ensure no breaking changes to existing APIs

#### Addressing Review Comments
- **Prompt Responses**: Address reviewer feedback within 48 hours
- **Clear Explanations**: Explain design decisions and trade-offs
- **Test Updates**: Update tests based on reviewer suggestions
- **Documentation Updates**: Keep documentation in sync with code changes

### Continuous Integration

#### Pre-Commit Testing
```bash
# Run comprehensive test suite
ninja check-llvm
ninja check-mpi-sanitizer

# Performance validation
python3 benchmark_optimization.py --validate-performance

# Code quality checks
clang-format --dry-run --Werror **/*.cpp **/*.h
clang-tidy --checks='-*,llvm-*,readability-*' **/*.cpp
```

#### Post-Commit Monitoring
- Monitor buildbot results across all supported platforms
- Address any regressions promptly
- Update documentation based on user feedback
- Maintain performance benchmarks

## Platform Support

### Supported Platforms
- **Linux**: x86_64, AArch64, PowerPC
- **macOS**: x86_64, Apple Silicon (M1/M2)
- **Windows**: x86_64 (MSVC and MinGW)
- **FreeBSD**: x86_64
- **AIX**: PowerPC (limited support)

### Compiler Support
- **Clang**: 12.0+ (primary development compiler)
- **GCC**: 9.0+ (compatibility testing)
- **MSVC**: 2019+ (Windows support)
- **Intel ICC**: 2021+ (HPC environments)

### MPI Implementation Support
- **Open MPI**: 4.0+
- **MPICH**: 3.3+
- **Intel MPI**: 2019+
- **Microsoft MPI**: 10.0+
- **IBM Spectrum MPI**: 10.3+

## Maintenance and Support

### Long-term Maintenance
- **API Stability**: Maintain backward compatibility for public APIs
- **Performance Monitoring**: Continuous performance regression testing
- **Security Updates**: Prompt response to security vulnerabilities
- **Community Support**: Active engagement with user community

### Version Management
- **Semantic Versioning**: Follow semantic versioning for releases
- **LTS Support**: Long-term support for stable versions
- **Migration Guides**: Comprehensive guides for major version upgrades
- **Deprecation Policy**: Clear deprecation timeline for obsolete features

## Conclusion

The MPI Usage Sanitizer LLVM Pass is designed to be a production-quality addition to the LLVM ecosystem. This contribution guide ensures that the implementation meets LLVM's high standards for code quality, testing, documentation, and maintainability.

The comprehensive approach to upstream contribution includes:
1. **Strict adherence to LLVM coding standards**
2. **Comprehensive testing at multiple levels**
3. **Thorough documentation for users and developers**
4. **Structured patch series for reviewable contributions**
5. **Long-term maintenance and support commitment**

By following this guide, the MPI Usage Sanitizer Pass will integrate seamlessly into the LLVM project and provide valuable functionality to the MPI development community.