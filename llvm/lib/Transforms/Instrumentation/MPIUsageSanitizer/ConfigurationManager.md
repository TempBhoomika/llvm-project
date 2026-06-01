# MPI Usage Sanitizer Configuration Manager

The ConfigurationManager class provides comprehensive configuration management for the MPI Usage Sanitizer LLVM pass. It supports both command-line options and configuration files for flexible instrumentation policies.

## Features

### Command Line Options

The following command-line options are available:

- `--mpi-sanitizer-level=<level>`: Set instrumentation level (full, lightweight, performance)
- `--mpi-sanitizer-enable-optimizations`: Enable compile-time optimizations
- `--mpi-sanitizer-enable-performance`: Enable performance monitoring hooks
- `--mpi-sanitizer-enable-deadlock-detection`: Enable deadlock detection analysis
- `--mpi-sanitizer-enable-datarace-detection`: Enable data race detection analysis
- `--mpi-sanitizer-config=<file>`: Specify configuration file path
- `--mpi-sanitizer-verbose`: Enable verbose output
- `--mpi-sanitizer-statistics`: Print instrumentation statistics
- `--mpi-sanitizer-enable-types=<types>`: Comma-separated list of MPI function types to instrument
- `--mpi-sanitizer-disable-types=<types>`: Comma-separated list of MPI function types to skip
- `--mpi-sanitizer-enable-functions=<functions>`: Comma-separated list of specific MPI functions to instrument
- `--mpi-sanitizer-disable-functions=<functions>`: Comma-separated list of specific MPI functions to skip

### Instrumentation Levels

1. **Full Mode** (`full`): Instruments all enabled MPI operations with complete error checking and monitoring
2. **Lightweight Mode** (`lightweight`): Instruments only error-prone operations (point-to-point, requests, critical collectives)
3. **Performance Mode** (`performance`): Only instruments operations for performance monitoring

### MPI Function Types

The following MPI function types are supported:

- `pointtopoint` (or `p2p`): MPI_Send, MPI_Recv, MPI_Isend, MPI_Irecv, etc.
- `collective` (or `coll`): MPI_Bcast, MPI_Reduce, MPI_Allreduce, etc.
- `communicator` (or `comm`): MPI_Comm_create, MPI_Comm_split, etc.
- `datatype` (or `type`): MPI_Type_create, MPI_Type_commit, etc.
- `request` (or `req`): MPI_Wait, MPI_Test, etc.
- `info`: MPI_Info_create, MPI_Info_set, etc.
- `window` (or `win`): MPI_Win_create, MPI_Win_fence, etc.
- `file` (or `io`): MPI_File_open, MPI_File_read, etc.
- `topology` (or `topo`): MPI_Cart_create, MPI_Graph_create, etc.
- `environment` (or `env`): MPI_Init, MPI_Finalize, etc.

## Configuration File Format

Configuration files use INI-style format with sections and key-value pairs:

```ini
[global]
instrumentation_level=full
enable_optimizations=true
enable_performance_monitoring=false

[policy.type.collective]
enable_pre_hooks=true
enable_post_hooks=true
enable_performance_hooks=true

[policy.function.MPI_Send]
enable_pre_hooks=true
enable_post_hooks=true
enable_error_checking=true
```

### Configuration Sections

1. **[global]**: Global configuration options
2. **[policy.type.<type>]**: Policies for specific MPI function types
3. **[policy.function.<function>]**: Policies for specific MPI functions

### Policy Options

Each policy section supports the following options:

- `enable_pre_hooks`: Insert hooks before MPI calls (true/false)
- `enable_post_hooks`: Insert hooks after MPI calls (true/false)
- `enable_performance_hooks`: Insert performance monitoring hooks (true/false)
- `enable_error_checking`: Enable error checking instrumentation (true/false)
- `enable_deadlock_detection`: Enable deadlock detection (true/false)
- `enable_datarace_detection`: Enable data race detection (true/false)
- `optimization_level`: Optimization level (0=none, 1=basic, 2=aggressive)

## Usage Examples

### Basic Usage

```cpp
// Create configuration manager with default options
MPIUsageSanitizerOptions Options;
ConfigurationManager ConfigMgr(Options);

// Initialize with command line options
ConfigMgr.initialize();

// Check if a function should be instrumented
if (ConfigMgr.shouldInstrument("MPI_Send")) {
    // Instrument the function
}
```

### Custom Configuration

```cpp
// Load configuration from file
ConfigMgr.loadFromFile("mpi_sanitizer.conf");

// Set custom policies
InstrumentationPolicy Policy;
Policy.EnablePreHooks = true;
Policy.EnablePostHooks = false;
Policy.OptimizationLevel = 2;

ConfigMgr.setFunctionPolicy("MPI_Bcast", Policy);
```

### Instrumentation Decision Logic

```cpp
// Check instrumentation with analysis results
CallSite Site = /* ... */;
AnalysisResult Analysis = /* ... */;

if (ConfigMgr.shouldInstrument(Site, &Analysis)) {
    // Apply instrumentation based on policy
    InstrumentationPolicy Policy = ConfigMgr.getInstrumentationPolicy(Site.FunctionName);
    
    if (Policy.EnablePreHooks) {
        // Insert pre-call hook
    }
    
    if (Policy.EnablePostHooks) {
        // Insert post-call hook
    }
}
```

## Integration with LLVM Pass Manager

The ConfigurationManager integrates seamlessly with LLVM's pass manager system:

```cpp
class MPIUsageSanitizerPass : public PassInfoMixin<MPIUsageSanitizerPass> {
private:
    std::unique_ptr<ConfigurationManager> ConfigMgr;
    
public:
    MPIUsageSanitizerPass(const MPIUsageSanitizerOptions& Options) {
        ConfigMgr = std::make_unique<ConfigurationManager>(Options);
        ConfigMgr->initialize();
    }
    
    PreservedAnalyses run(Module& M, ModuleAnalysisManager& AM) {
        // Use ConfigMgr to make instrumentation decisions
        for (auto& F : M) {
            for (auto& Site : detectMPICalls(F)) {
                if (ConfigMgr->shouldInstrument(Site)) {
                    instrumentCallSite(Site);
                }
            }
        }
        return PreservedAnalyses::none();
    }
};
```

## Error Handling

The ConfigurationManager provides robust error handling:

- Invalid configuration files are reported with detailed error messages
- Malformed configuration entries are skipped with warnings
- Missing configuration files fall back to command-line options
- Invalid function names or types are ignored gracefully

## Performance Considerations

- Configuration parsing is performed once during initialization
- Instrumentation decisions use efficient lookup tables
- Policy evaluation is optimized for hot paths
- Memory usage scales linearly with the number of custom policies

## Thread Safety

The ConfigurationManager is designed to be thread-safe for read operations after initialization. Modification operations (setting policies) should be performed during initialization phase only.