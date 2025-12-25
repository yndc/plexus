# Cycles

Cycles is a high-performance, multithreaded task scheduling framework for C++20 based on Directed Acyclic Graphs (DAG). It orchestrates units of work (Nodes) based on data access rules (Dependencies) to maximize parallel execution without race conditions.

## Features

- **Strict Phase Separation**: Heavy validation and optimization during "Baking", zero-allocation execution during "Runtime".
- **Data-Driven Dependencies**: Automatic topology inference based on Read/Write access to Resources.
- **Fail-Fast Safety**: Immediate termination on cyclic dependencies or invalid access patterns.
- **Modern C++**: Built with C++20.

## Building

Cycles uses CMake. To build the library and tests:

```bash
mkdir build
cd build
cmake ..
make
ctest
```

## Basic Usage

```cpp
#include "Cycles/context.h"
#include "Cycles/graph_builder.h"
#include <iostream>

void example() {
    Cycles::Context ctx;
    auto buffer_id = ctx.register_resource("BufferA");

    Cycles::GraphBuilder builder(ctx);

    // Node A: Writes to BufferA
    builder.add_node({
        "Writer",
        []() { std::cout << "Writing...\n"; },
        {{buffer_id, Cycles::Access::WRITE}}
    });

    // Node B: Reads from BufferA
    builder.add_node({
        "Reader",
        []() { std::cout << "Reading...\n"; },
        {{buffer_id, Cycles::Access::READ}}
    });

    // Bake into an execution graph
    auto graph = builder.bake();

    // Execute (Milestone 2 integration pending)
    for (const auto& wave : graph.waves) {
        for (const auto& task : wave.tasks) {
            task();
        }
    }
}
```
