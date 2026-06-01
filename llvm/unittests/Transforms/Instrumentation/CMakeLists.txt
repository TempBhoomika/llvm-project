set(LLVM_LINK_COMPONENTS
  Analysis
  AsmParser
  Core
  Instrumentation
  Passes
  ProfileData
  Support
)

add_llvm_unittest(InstrumentationTests
  MemProfUseTest.cpp
  PGOInstrumentationTest.cpp
  MPIUsageSanitizer/MetadataExtractorTest.cpp
  MPIUsageSanitizer/HookInserterTest.cpp
  MPIUsageSanitizer/StaticAnalyzerTest.cpp
  MPIUsageSanitizerConfigTest.cpp
  )

target_link_libraries(InstrumentationTests PRIVATE LLVMTestingSupport)
