#include "Cycles/graph_builder.h"
#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Cycles {

GraphBuilder::GraphBuilder(Context &ctx) : m_ctx(ctx) {}

NodeID GraphBuilder::add_node(NodeConfig config) {
  NodeID id = static_cast<NodeID>(m_nodes.size());
  m_nodes.push_back(std::move(config));
  return id;
}

ExecutionGraph GraphBuilder::bake() {
  auto n = m_nodes.size();
  if (n == 0)
    return {};

  // 1. Sort nodes by priority (higher first), then by insertion order (stable
  // sort) Actually, m_nodes is already in insertion order. If we want to
  // support priority, we should stable_sort. For now, let's assume registration
  // order is enough or simple priority sort.
  std::stable_sort(m_nodes.begin(), m_nodes.end(),
                   [](const NodeConfig &a, const NodeConfig &b) {
                     return a.priority > b.priority;
                   });

  // 2. Build Adjacency List based on dependencies
  std::vector<std::vector<int>> adj(n);
  std::vector<int> indegree(n, 0);

  // Resources tracking
  // We don't know the max ResourceID ahead of time easily without querying
  // Context or dynamic sizing. We'll use a map for sparse tracking or resize if
  // we knew max ID. Given Context::m_next_id, we could ask Context, but for now
  // map is safer.
  std::map<ResourceID, int> last_writer;
  std::map<ResourceID, std::vector<int>> current_readers;

  // Initialize tracking
  // Pre-scan for max id could optimize, but map is fine.

  for (size_t i = 0; i < n; ++i) {
    const auto &node = m_nodes[i];

    for (const auto &dep : node.dependencies) {
      auto res = dep.id;

      if (dep.access == Access::READ) {
        // Dependency: Last Writer -> This Reader
        if (last_writer.count(res)) {
          auto writer_idx = last_writer[res];
          // Avoid duplicate edges? Not strictly necessary for Kahn's but good.
          // Checking existence in vector is O(E), let's just add and assume ok
          // or use set if needed. For simplicity, just add.
          adj[writer_idx].push_back(i);
          indegree[i]++;
        }
        current_readers[res].push_back(i);
      } else { // WRITE
        // Dependency: Last Writer -> This Writer (WAW)
        if (last_writer.count(res)) {
          auto writer_idx = last_writer[res];
          if (writer_idx != static_cast<int>(i)) {
            adj[writer_idx].push_back(i);
            indegree[i]++;
          }
        }

        // Dependency: All Current Readers -> This Writer (WAR)
        for (auto reader_idx : current_readers[res]) {
          if (reader_idx != static_cast<int>(i)) {
            adj[reader_idx].push_back(i);
            indegree[i]++;
          }
        }

        // Update state
        last_writer[res] = i;
        current_readers[res].clear();
      }
    }

    // Explicit Ordering: run_after
    for (auto dependee : node.run_after) {
      if (dependee < n && dependee != i) {
        adj[dependee].push_back(i);
        indegree[i]++;
      }
    }

    // Explicit Ordering: run_before
    for (auto dependent : node.run_before) {
      if (dependent < n && dependent != i) {
        adj[i].push_back(dependent);
        indegree[dependent]++;
      }
    }
  }

  // 3. Topological Sort (Kahn's Algorithm) with Layering
  std::vector<int> queue;
  for (size_t i = 0; i < n; ++i) {
    if (indegree[i] == 0) {
      queue.push_back(i);
    }
  }

  ExecutionGraph graph;
  size_t processed_count = 0;

  while (!queue.empty()) {
    auto wave = ExecutionGraph::Wave{};
    std::vector<int> next_queue;

    for (auto u : queue) {
      // Add actual task to wave
      wave.tasks.push_back(m_nodes[u].work_function);
      processed_count++;

      for (auto v : adj[u]) {
        indegree[v]--;
        if (indegree[v] == 0) {
          next_queue.push_back(v);
        }
      }
    }

    graph.waves.push_back(std::move(wave));
    queue = std::move(next_queue);
  }

  // 4. Cycle Detection
  if (processed_count != n) {
    // Construct error message with remaining nodes
    // (Optional: Find the cycle for debug, simpler to just list nodes)
    std::stringstream ss;
    ss << "Cyclic dependency detected! Nodes involved or unreachable: ";
    for (size_t i = 0; i < n; ++i) {
      if (indegree[i] > 0) {
        ss << m_nodes[i].debug_name << " ";
      }
    }
    throw std::runtime_error(ss.str());
  }

  return graph;
}

} // namespace Cycles
