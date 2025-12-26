#pragma once
#include "plexus/context.h"
#include "plexus/execution_graph.h"
#include "plexus/node.h"
#include <vector>

namespace Plexus {

    /**
     * @brief Constructs an ExecutionGraph from a set of Nodes and their
     * dependencies.
     *
     * The GraphBuilder is responsible for:
     * 1. Accepting node registration.
     * 2. Resolving dependencies (Read-After-Write, Write-After-Read,
     * Write-After-Write).
     * 3. Performing Topological Sort (Kahn's Algorithm).
     * 4. Detecting cycles.
     */
    class GraphBuilder {
    public:
        explicit GraphBuilder(Context &ctx);

        /**
         * @brief Registers a node for inclusion in the graph.
         * @param config The node configuration.
         * @return NodeID The unique ID of the registered node.
         */
        NodeID add_node(NodeConfig config);

        /**
         * @brief Compiles the registered nodes into an optimized ExecutionGraph.
         *
         * This process involves resolving the dependency graph and sorting it into
         * waves.
         *
         * @return ExecutionGraph The executable graph.
         * @throws std::runtime_error If a cyclic dependency is detected.
         */
        ExecutionGraph bake();

    private:
        Context &m_ctx;
        std::vector<NodeConfig> m_nodes;
    };

}
