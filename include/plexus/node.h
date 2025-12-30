#pragma once
#include "plexus/context.h"
#include <cstdint>
#include <functional>
#include <optional>
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

    /// @brief Unique identifier for a Node Group (phase).
    using NodeGroupID = uint32_t;

    /**
     * @brief Configuration for a node group (execution phase).
     *
     * Groups allow organizing nodes into phases with explicit ordering.
     * All nodes in a group that has `run_after` dependencies will wait
     * for all nodes in those dependency groups to complete.
     */
    struct NodeGroupConfig {
        std::string name;                   ///< Human-readable name for debugging.
        std::vector<NodeGroupID> run_after; ///< Groups that must complete before this group starts.
    };

    /**
     * @brief Defines behavior when the node's work function throws an exception.
     */
    enum class ErrorPolicy {
        Continue,         ///< Ignore error, trigger dependents (Best Effort).
        CancelDependents, ///< Stop this branch, skip dependents. Independent branches continue.
        CancelGraph       ///< Stop scheduling ANY new tasks. Drain current tasks.
    };

    /**
     * @brief Specifies which thread a node should execute on.
     */
    enum class ThreadAffinity {
        Any = -1, ///< Can run on any worker thread.
        Main = 0  ///< Must run on the thread that called Executor::run().
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
         */
        int priority = 0;

        /**
         * @brief Policy to apply on failure. Default is Continue.
         */
        ErrorPolicy error_policy = ErrorPolicy::Continue;

        /**
         * @brief Specific thread requirement. Default is Any.
         */
        ThreadAffinity thread_affinity = ThreadAffinity::Any;

        /**
         * @brief Optional group this node belongs to.
         *
         * If set, this node inherits the group's ordering constraints.
         * Nodes in a group with `run_after` dependencies will wait for
         * all nodes in those dependency groups to complete.
         */
        std::optional<NodeGroupID> group_id;

        /**
         * @brief Groups that must complete before this node can start.
         *
         * This allows standalone nodes to synchronize with entire groups.
         */
        std::vector<NodeGroupID> run_after_group;

        /**
         * @brief Groups that must wait for this node to complete.
         *
         * All nodes in the specified groups will depend on this node.
         */
        std::vector<NodeGroupID> run_before_group;
    };
}
