# plexus

**plexus** is a high-performance, multithreaded task scheduling framework for C++20 based on Directed Acyclic Graphs (DAG). It orchestrates units of work (Nodes) based on data access rules (Dependencies) to dynamically execute tasks as soon as their prerequisites are met.

## Features

- **Work-Stealing Scheduler**: High-performance thread pool using per-thread distributed queues to minimize contention and maximize cache locality.
- **Dynamic Task Graph**: Nodes execute immediately upon dependency resolution, maximizing parallelism.
- **Strict Phase Separation**: Heavy validation and optimization during "Baking", allocation-free execution during "Runtime".
- **Data-Driven Dependencies**: Automatic topology inference based on Read/Write access to Resources.
- **Resilient Error Handling**: Multiple recovery policies including `Continue`, `CancelDependents`, and `CancelGraph`.
- **Graph Visualization**: Textual debug output for auditing the baked execution graph structure and priorities.
- **Fail-Fast Safety**: Immediate termination on cyclic dependencies or invalid access patterns during baking.
- **Modern C++**: Built with C++20.

## Building

plexus uses CMake. To build the library and tests:

```bash
mkdir build
cd build
cmake ..
make
ctest
```

## Benchmarking

plexus uses **Google Benchmark** to measure performance. 

### 1. Build Benchmarks
Always build in **Release** mode for accurate timings:

```bash
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make plexusBenchmarks
```

### 2. Run Benchmarks

```bash
./plexusBenchmarks
```

## Basic Usage

```cpp
#include "plexus/context.h"
#include "plexus/graph_builder.h"
#include "plexus/executor.h"
#include <iostream>

void example() {
    // Register a resource
    Plexus::Context ctx;
    auto buffer_id = ctx.register_resource("BufferA");

    // Create a graph builder
    Plexus::GraphBuilder builder(ctx);

    // Create a node that writes to BufferA
    builder.add_node({
        "Writer",
        []() { std::cout << "Writing...\n"; },
        {{buffer_id, Plexus::Access::WRITE}}
    });

    // Create a node that reads from BufferA
    builder.add_node({
        "Reader",
        []() { std::cout << "Reading...\n"; },
        {{buffer_id, Plexus::Access::READ}}
    });

    // Bake the graph
    auto graph = builder.bake();

    // Execute the graph
    // (An internal ThreadPool is created and managed automatically)
    Plexus::Executor executor;
    executor.run(graph);
}
```

### Advanced: Manual Thread Pool Management
If you want to share a single thread pool across multiple executors or configure threads manually:

```cpp
#include "plexus/detail/thread_pool.h"

Plexus::ThreadPool pool; // 8 worker threads by default
Plexus::Executor executor(pool);
executor.run(graph);
```

## Advanced Usage

### Dependency Resolution (WAW, WAR)
plexus automatically handles complex dependency chains.

- **Read-After-Write (RAW)**: A Reader will always run after a Writer of the same resource has finished.
- **Write-After-Write (WAW)**: If multiple nodes write to the same resource, the order is determined by registration order (or priority). The second writer will run after the first.
- **Write-After-Read (WAR)**: A Writer will run after all current Readers of a resource have finished.

### Node Configuration
The `NodeConfig` struct gives you fine-grained control over task execution.

#### 1. Dependencies
The most common way to order nodes. You declare which Resources a node needs to READ or WRITE. The `GraphBuilder` infers the dependency arrows automatically.

```cpp
builder.add_node({
    .debug_name = "MyNode",
    .work_function = [](){},
    .dependencies = {{resource_id, Plexus::Access::READ}}
});
```

#### 2. Explicit Ordering (`run_after`, `run_before`)
For cases where data dependencies aren't enough (or don't exist), you can force edges between nodes.

```cpp
auto node_a = builder.add_node({...});
// Force Node B to run after Node A
builder.add_node({
    .debug_name = "Node B",
    .run_after = {node_a}
});
```

#### 3. Task Priority (`priority`)
You can assign an integer priority to any node (Default: 0).
**Important**: This controls **Execution Urgency**, not Baking Order. 
- Higher priority tasks are executed earlier by the thread pool when multiple tasks are ready to run simultaneously.
- It does **not** override dependencies. A high-priority task must still wait for its dependencies to finish.
- The `GraphBuilder` calculates an *effective priority* for each node:
  `Effective Priority = (User Priority * 1000) + Critical Path Length + Descendants`
  
  This ensures that "Bottleneck" nodes (roots of deep dependency trees) get a natural boost, but you can always just set a high user priority (e.g. 100) to force a node to the front of the line.

```cpp
builder.add_node({
    .debug_name = "CriticalTask",
    .priority = 100, // Runs ASAP
    .error_policy = Plexus::ErrorPolicy::CancelGraph // Stop everything if this fails
});
```

#### 4. Error Policies (`error_policy`)
You can define how the executor reacts when a node's `work_function` throws an exception.

- `ErrorPolicy::Continue` (Default): The error is recorded, but dependent nodes are still triggered. Best for "best-effort" tasks.
- `ErrorPolicy::CancelDependents`: If this node fails, all its direct and indirect descendants are skipped. Other independent branches continue.
- `ErrorPolicy::CancelGraph`: If this node fails, no new tasks are scheduled. The executor waits for currently running tasks to finish and then rethrows the exception.

```cpp
builder.add_node({
    .debug_name = "OptionalTask",
    .work_function = []() { /* ... */ },
    .error_policy = Plexus::ErrorPolicy::Continue 
});
```

#### 5. Thread Affinity (`thread_affinity`)
You can control which thread a node runs on using the `thread_affinity` option.

- `ThreadAffinity::Any` (Default): The node can be executed by any worker thread in the pool.
- `ThreadAffinity::Main`: The node will explicitly run on the thread that called `executor.run()`. This is essential for integrating with APIs that are not thread-safe (e.g. OpenGL, OS Event Loops).

```cpp
builder.add_node({
    .debug_name = "RenderTask",
    .work_function = []() { render_frame(); },
    .thread_affinity = Plexus::ThreadAffinity::Main 
});
```

### Profiling
You can hook into the `Executor` to measure performance.

```cpp
Plexus::Executor executor(pool);
executor.set_profiler_callback([](const char* name, double duration_ms) {
    std::cout << "[Profile] " << name << " took " << duration_ms << "ms\n"; 
});
executor.run(graph);
```

### Visualizing the Graph
You can dump a textual representation of the execution graph for debugging. This shows the resolved dependencies, entry nodes, and effective priorities.

```cpp
auto graph = builder.bake();
graph.dump_debug(std::cout);
```

## Documentation
To generate full API documentation:
```bash
cd build
make docs
```
Open `docs/doxygen/html/index.html` in your browser.
