# Changelog

All notable changes to the Plexus project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-01-10

### Added

#### Core Framework
- DAG-based task scheduling with topological sorting and wave-based execution
- Lock-free work-stealing thread pool using Chase-Lev deque implementation
- `Context` for resource lifecycle management
- `GraphBuilder` for declarative graph construction with cycle detection
- `ExecutionGraph` (baked graph) as an immutable, optimized execution plan
- `Executor` for parallel task execution with configurable thread pools

#### Resource API
- `Resource<T>` wrapper for type-safe data access
- `add_auto_node()` with automatic dependency inference from lambda parameters
- `add_typed_node()` with explicit `Read()`/`Write()` access tags
- Compile-time `static_assert` validation for resource type mismatches

#### Runtime API
- `Plexus::current_worker_index()` for thread identification
- `Plexus::worker_count()` for querying pool size
- Main thread affinity support (`ThreadAffinity::Main`)
- `ExecutionMode::Sequential` for single-threaded debugging

#### Error Handling
- Multiple recovery policies: `Continue`, `CancelDependents`, `CancelGraph`
- Exception propagation with preserved error messages

#### Testing
- Comprehensive GTest test suite (87+ tests, 100% pass rate)
- Unit tests for `WorkStealingQueue`, `RingBuffer`, `ThreadPool`, `FixedFunction`
- High-contention stress tests for lock-free data structures
- Boundary and edge case coverage for executor and graph builder

#### Documentation
- README with usage examples and API overview
- Style guide for contributors

[Unreleased]: https://github.com/yourname/plexus/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/yourname/plexus/releases/tag/v0.1.0
