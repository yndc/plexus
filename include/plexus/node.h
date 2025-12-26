#pragma once
#include "plexus/context.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Plexus {
    /**
     * @brief Defines how a Node interacts with a Resource.
     */
    enum class Access {
        READ, ///< Read-only access. Shared with other readers.
        WRITE ///< Exclusive write access. Blocks readers and other writers.
    };

    /**
     * @brief Represents a single data dependency for a Node.
     */
    struct Dependency {
        ResourceID id; ///< The ID of the resource being accessed.
        Access access; ///< The type of access required.
    };

    /// @brief Unique identifier for a registered Node.
    using NodeID = uint32_t;

    /**
     * @brief Defines behavior when the node's work function throws an exception.
     */
    enum class ErrorPolicy {
        Continue,         ///< Ignore error, trigger dependents (Best Effort).
        CancelDependents, ///< Stop this branch, skip dependents. Independent branches continue.
        CancelGraph       ///< Stop scheduling ANY new tasks. Drain current tasks.
    };

    /**
     * @brief Configuration structure for creating a Node in the Graph.
     */
    struct NodeConfig {
        std::string debug_name;               ///< Human-readable name for debugging.
        std::function<void()> work_function;  ///< The actual logic to execute.
        std::vector<Dependency> dependencies; ///< List of resources this node uses.

        std::vector<NodeID> run_after;  ///< Nodes that must run BEFORE this node.
        std::vector<NodeID> run_before; ///< Nodes that must run AFTER this node.

        /**
         * @brief Base priority for the node.
         * Default is 0. Higher values run earlier.
         * This value is accumulated with descendant counts to form effective priority.
         */
        int priority = 0;

        /**
         * @brief Policy to apply on failure. Default is Continue.
         */
        ErrorPolicy error_policy = ErrorPolicy::Continue;
    };
}
