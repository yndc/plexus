#pragma once
#include "Cycles/context.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Cycles {
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
 * @brief Configuration structure for creating a Node in the Graph.
 */
struct NodeConfig {
  std::string debug_name;               ///< Human-readable name for debugging.
  std::function<void()> work_function;  ///< The actual logic to execute.
  std::vector<Dependency> dependencies; ///< List of resources this node uses.

  std::vector<NodeID> run_after;  ///< Nodes that must run BEFORE this node.
  std::vector<NodeID> run_before; ///< Nodes that must run AFTER this node.

  /**
   * @brief Priority for deterministic ordering of independent nodes.
   * Higher priority nodes are processed earlier in the sort, potentially
   * appearing earlier in the wave. Default is 0.
   */
  int priority = 0;
};
} // namespace Cycles
