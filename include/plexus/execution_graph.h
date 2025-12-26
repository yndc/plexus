#pragma once
#include "plexus/node.h" // For ErrorPolicy
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace Plexus {
    /**
     * @brief Roughly a baked schedule of tasks.
     *
     * An ExecutionGraph organizes tasks into "Waves". All tasks in a single wave
     * are guaranteed to be independent of each other (conforming to dependencies)
     * and can safely run in parallel.
     */
    /**
     * @brief A baked dependency graph for asynchronous execution.
     *
     * Nodes are executed as soon as their dependencies are met.
     */
    struct ExecutionGraph {
        struct Node {
            std::function<void()> work;   ///< The user task
            std::vector<int> dependents;  ///< Indices of nodes waiting on this one
            int initial_dependencies = 0; ///< How many inputs this node needs
            int priority = 0;             ///< Effective priority for scheduling (higher is better)
            ErrorPolicy error_policy = ErrorPolicy::Continue; ///< Policy on failure
            std::string label;                                ///< Debug label
        };

        std::vector<Node> nodes;      ///< All nodes in the graph
        std::vector<int> entry_nodes; ///< Nodes with 0 dependencies (start here)

        /**
         * @brief Dumps a textual representation of the graph to the given stream.
         */
        void dump_debug(std::ostream &os) const;
    };
}
