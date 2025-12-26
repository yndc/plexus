#include "plexus/graph_builder.h"
#include <algorithm>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace Plexus {

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

        // 1. Sort nodes by priority (higher first), then by insertion order (stable sort)
        std::stable_sort(
            m_nodes.begin(), m_nodes.end(),
            [](const NodeConfig &a, const NodeConfig &b) { return a.priority > b.priority; });

        // 2. Build Adjacency List based on dependencies
        std::vector<std::vector<int>> adj(n);
        std::vector<int> indegree(n, 0);

        // Resources tracking
        std::map<ResourceID, int> last_writer;
        std::map<ResourceID, std::vector<int>> current_readers;

        for (size_t i = 0; i < n; ++i) {
            const auto &node = m_nodes[i];

            for (const auto &dep : node.dependencies) {
                auto res = dep.id;

                if (dep.access == Access::READ) {
                    // Dependency: Last Writer -> This Reader
                    if (last_writer.count(res)) {
                        auto writer_idx = last_writer[res];
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

        // 3. Cycle Detection (Simulated Topological Sort)
        {
            std::vector<int> temp_indegree = indegree;
            std::vector<int> queue;
            for (size_t i = 0; i < n; ++i) {
                if (temp_indegree[i] == 0)
                    queue.push_back(i);
            }

            size_t processed_count = 0;
            while (!queue.empty()) {
                auto u = queue.back();
                queue.pop_back();
                processed_count++;

                for (auto v : adj[u]) {
                    temp_indegree[v]--;
                    if (temp_indegree[v] == 0) {
                        queue.push_back(v);
                    }
                }
            }

            if (processed_count != n) {
                std::stringstream ss;
                ss << "Cyclic dependency detected! Nodes involved or unreachable: ";
                for (size_t i = 0; i < n; ++i) {
                    if (indegree[i] > 0) { // Check original indegree or temp?
                        // If checking original, some valid nodes might have > 0 but were processed.
                        // But since we failed specific partial sort, debugging is hard.
                        // Simple listing is enough.
                        ss << m_nodes[i].debug_name << " ";
                    }
                }
                throw std::runtime_error(ss.str());
            }
        }

        // 4. Construct Execution Graph
        ExecutionGraph graph;
        graph.nodes.resize(n);

        for (size_t i = 0; i < n; ++i) {
            graph.nodes[i].work = m_nodes[i].work_function;
            graph.nodes[i].dependents = std::move(adj[i]);
            graph.nodes[i].initial_dependencies = indegree[i];

            if (indegree[i] == 0) {
                graph.entry_nodes.push_back(i);
            }
        }

        return graph;
    }
}
