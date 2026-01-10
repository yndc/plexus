#pragma once
#include "plexus/context.h"
#include "plexus/detail/function_traits.h"
#include "plexus/execution_graph.h"
#include "plexus/node.h"
#include "plexus/resource.h"
#include <functional>
#include <utility>
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
         * @brief Registers a node group (execution phase).
         * @param config The group configuration.
         * @return NodeGroupID The unique ID of the registered group.
         */
        NodeGroupID add_group(NodeGroupConfig config);

        /**
         * @brief Registers a node for inclusion in the graph.
         * @param config The node configuration.
         * @return NodeID The unique ID of the registered node.
         */
        NodeID add_node(NodeConfig config);

        /**
         * @brief Adds a node with explicit Read/Write tagged resources.
         *
         * Use this when you need explicit control over access types or when mixing
         * READ and WRITE access to multiple resources.
         *
         * Example:
         * @code
         * Resource<std::vector<int>> bufA(ctx, "A");
         * Resource<int> counter(ctx, "Counter");
         *
         * builder.add_typed_node(
         *     "Process",
         *     [](const std::vector<int>& a, int& cnt) { cnt = a.size(); },
         *     Read(bufA),
         *     Write(counter)
         * );
         * @endcode
         *
         * @tparam Func The lambda or function object type.
         * @tparam Accesses The access wrapper types (ReadAccess or WriteAccess).
         * @param name The debug name for this node.
         * @param work The work function to execute.
         * @param accesses The tagged resource accesses.
         * @return NodeID The unique ID of the registered node.
         */
        template <typename Func, typename... Accesses>
        NodeID add_typed_node(const std::string &name, Func &&work, Accesses &&...accesses) {
            return add_node({.debug_name = name,
                             .work_function = [work = std::forward<Func>(work),
                                               ... acc = std::forward<Accesses>(
                                                   accesses)]() mutable { work(acc.get()...); },
                             .dependencies = {accesses.dependency()...}});
        }

        /**
         * @brief Adds a node with automatic dependency inference.
         *
         * Automatically infers READ vs WRITE access from function parameter types:
         * - const T& → READ access
         * - T& → WRITE access
         *
         * Resources are matched to parameters positionally and must have matching types.
         *
         * Example:
         * @code
         * Resource<std::vector<int>> bufA(ctx, "A");
         * Resource<int> counter(ctx, "Counter");
         *
         * builder.add_auto_node(
         *     "Process",
         *     [](const std::vector<int>& a, int& cnt) { cnt = a.size(); },
         *     bufA,     // Inferred: READ (const&)
         *     counter   // Inferred: WRITE (&)
         * );
         * @endcode
         *
         * @tparam Func The lambda or function object type.
         * @tparam ResourceTypes The types of the resources (auto-deduced).
         * @param name The debug name for this node.
         * @param work The work function to execute.
         * @param resources The resources in the same order as function parameters.
         * @return NodeID The unique ID of the registered node.
         */
        template <typename Func, typename... ResourceTypes>
        NodeID add_auto_node(const std::string &name, Func &&work,
                             Resource<ResourceTypes> &...resources) {
            static_assert(detail::function_traits<Func>::arity == sizeof...(ResourceTypes),
                          "Function parameter count must match resource count");

            return add_auto_node_impl<Func>(name, std::forward<Func>(work),
                                            std::index_sequence_for<ResourceTypes...>{},
                                            resources...);
        }

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
        std::vector<NodeGroupConfig> m_groups;
        std::vector<NodeConfig> m_nodes;

        // Implementation helper that uses index sequence for proper expansion
        template <typename Func, std::size_t... Indices, typename... ResourceTypes>
        NodeID add_auto_node_impl(const std::string &name, Func &&work,
                                  std::index_sequence<Indices...>,
                                  Resource<ResourceTypes> &...resources) {
            // Validate that each Resource<T> matches the function's parameter type
            (detail::check_resource_type_match<Indices, Func, ResourceTypes>(), ...);

            return add_node(
                {.debug_name = name,
                 .work_function =
                     [work = std::forward<Func>(work), &resources...]() mutable {
                         work(access_resource<Func, ResourceTypes, Indices>(resources)...);
                     },
                 .dependencies = {
                     Dependency{resources.id(), detail::infer_access_at<Indices, Func>()}...}});
        }

        // Helper to provide const& or & based on function signature
        template <typename Func, typename T, std::size_t Idx>
        static decltype(auto) access_resource(Resource<T> &res) {
            using ArgType = typename detail::function_traits<Func>::template arg_type<Idx>;
            using BaseType = std::remove_reference_t<ArgType>;

            if constexpr (std::is_const_v<BaseType>) {
                return res.get(); // const T&
            } else {
                return res.get_mut(); // T&
            }
        }
    };

}
