//===- MetadataExtractor.h - MPI Call Metadata Extraction -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the MetadataExtractor class which extracts parameter
// information from MPI function calls to enable detailed runtime validation
// and analysis.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_METADATAEXTRACTOR_H
#define LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_METADATAEXTRACTOR_H

#include "MPICallDetector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include <map>
#include <vector>
#include <memory>

namespace llvm {

/// Parameter role in MPI function calls
enum class ParameterRole {
  Buffer,        // Data buffer pointer
  Count,         // Number of elements
  Datatype,      // MPI datatype
  Communicator,  // MPI communicator
  Request,       // MPI request handle
  Status,        // MPI status object
  Root,          // Root process rank
  Tag,           // Message tag
  Source,        // Source process rank
  Destination,   // Destination process rank
  Rank,          // Process rank
  Size,          // Size parameter
  Displacement,  // Displacement array
  SendCount,     // Send count array
  RecvCount,     // Receive count array
  SendType,      // Send datatype array
  RecvType,      // Receive datatype array
  SendDispl,     // Send displacement array
  RecvDispl,     // Receive displacement array
  Operation,     // MPI operation (reduce, etc.)
  Group,         // MPI group
  Window,        // MPI window
  Info,          // MPI info object
  Attribute,     // Attribute key/value
  ErrorCode,     // Error code
  ErrorClass,    // Error class
  ErrorString,   // Error string
  Flag,          // Boolean flag
  // Fortran-specific parameter roles
  ArrayDescriptor,    // Fortran array descriptor (CFI_cdesc_t, etc.)
  CharacterLength,    // Hidden character string length parameter
  OptionalPresent,    // Optional parameter presence flag
  DerivedType,        // Fortran derived type
  TypeBoundProcedure, // Type-bound procedure pointer
  Unknown
};

/// Information about a single parameter
struct ParameterInfo {
  Value* ParamValue;
  Type* ParamType;
  ParameterRole Role;
  bool IsInput;
  bool IsOutput;
  unsigned Index;
  
  ParameterInfo(Value* V, Type* T, ParameterRole R, bool In, bool Out, unsigned Idx)
    : ParamValue(V), ParamType(T), Role(R), IsInput(In), IsOutput(Out), Index(Idx) {}
};

/// Extracted metadata from an MPI call site
struct MPICallMetadata {
  StringRef FunctionName;
  std::vector<Value*> Parameters;
  std::map<std::string, Value*> NamedParameters;
  std::vector<ParameterInfo> ParameterInfos;
  Type* ReturnType;
  CallingConv::ID CallConv;
  MPIFunctionType FunctionType;
  
  MPICallMetadata() : ReturnType(nullptr), CallConv(CallingConv::C), 
                      FunctionType(MPIFunctionType::Unknown) {}
};

/// Forward declarations
class MPIFunctionDatabase;

/// Parameter Analyzer - Analyzes MPI function parameters based on signatures
class ParameterAnalyzer {
public:
  ParameterAnalyzer(MPIFunctionDatabase* DB) : FunctionDB(DB) {}
  
  std::vector<ParameterInfo> analyzeCall(const CallBase& Site);

private:
  ParameterRole analyzeParameterByPosition(const CallBase& Site, unsigned Index, Type* ParamType);
  MPIFunctionDatabase* FunctionDB;
};

/// Type Analyzer - Analyzes LLVM types to determine parameter roles
class TypeAnalyzer {
public:
  TypeAnalyzer() = default;
  
  ParameterRole analyzeType(Type* T, unsigned Index, StringRef FunctionName);
  bool isBufferType(Type* T);
  bool isCommunicatorType(Type* T);
  bool isRequestType(Type* T);
  bool isStatusType(Type* T);
  
  // Fortran-specific type analysis
  bool isFortranArrayDescriptor(Type* T);
  bool isFortranDerivedType(Type* T);
  bool isFortranCharacterLength(Type* T, unsigned Index);
  bool isFortranOptionalPresent(Type* T, unsigned Index);
  bool isCFIDescriptor(Type* T);
  bool isGFortranArrayDescriptor(Type* T);
  bool isIntelArrayDescriptor(Type* T);
  bool isPGIArrayDescriptor(Type* T);

private:
  ParameterRole analyzePointerType(PointerType* PtrTy, unsigned Index, StringRef FunctionName);
  ParameterRole analyzeIntegerType(Type* IntTy, unsigned Index, StringRef FunctionName);
  ParameterRole analyzeFortranParameter(Type* T, unsigned Index, StringRef FunctionName);
};

/// Metadata Extraction System
///
/// Extracts parameter information from MPI function calls to enable
/// detailed runtime validation and analysis.
class MetadataExtractor {
public:
  MetadataExtractor();
  ~MetadataExtractor();
  
  /// Set the MPI function database for enhanced parameter analysis
  void setFunctionDatabase(MPIFunctionDatabase* DB);
  
  /// Extract complete metadata from an MPI call site
  MPICallMetadata extractMetadata(const CallBase& Site);
  
  /// Extract communicator parameter from MPI call
  Value* extractCommunicator(const CallBase& Site);
  
  /// Extract buffer information (pointer, size, type) from MPI call
  std::vector<Value*> extractBufferInfo(const CallBase& Site);
  
  /// Extract request handle from MPI call
  Value* extractRequestHandle(const CallBase& Site);
  
  /// Extract status object from MPI call
  Value* extractStatus(const CallBase& Site);
  
  // Fortran-specific extraction methods
  /// Extract Fortran array descriptor information
  std::vector<Value*> extractFortranArrayDescriptors(const CallBase& Site);
  
  /// Extract Fortran character string length parameters
  std::vector<Value*> extractCharacterLengths(const CallBase& Site);
  
  /// Extract Fortran optional parameter presence flags
  std::vector<Value*> extractOptionalPresenceFlags(const CallBase& Site);
  
  /// Extract Fortran derived type information
  std::vector<Value*> extractDerivedTypeInfo(const CallBase& Site);
  
  /// Detect Fortran parameter passing convention
  bool isFortranParameterPassing(const CallBase& Site);
  
  /// Handle Fortran pass-by-reference semantics
  std::vector<Value*> handleFortranPassByReference(const CallBase& Site);

private:
  /// Analyze parameters based on function signature
  std::vector<ParameterInfo> analyzeParameters(const CallBase& Site);
  
  /// Determine parameter role based on position and type
  ParameterRole determineParameterRole(const CallBase& Site, unsigned Index, 
                                       Type* ParamType);
  
  /// Extract named parameters for easier access
  std::map<std::string, Value*> extractNamedParameters(const CallBase& Site,
                                                       const std::vector<ParameterInfo>& Infos);
  
  std::unique_ptr<ParameterAnalyzer> ParamAnalyzer;
  std::unique_ptr<TypeAnalyzer> TypeAnalyzer;
};

} // namespace llvm

#endif // LLVM_LIB_TRANSFORMS_INSTRUMENTATION_MPIUSAGESANITIZER_METADATAEXTRACTOR_H