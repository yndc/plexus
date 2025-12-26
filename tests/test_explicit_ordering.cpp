#include "plexus/graph_builder.h"
#include <gtest/gtest.h>

using namespace Plexus;

TEST(TopologyTest, ExplicitOrdering) {
    Context ctx;
    GraphBuilder builder(ctx);

    // Node A (Execution order should be 0)
    NodeConfig msgA;
    msgA.debug_name = "NodeA";
    msgA.work_function = []() {};
    auto idA = builder.add_node(msgA);

    // Node B (Execution order should be 1), depends on A explicitly
    NodeConfig msgB;
    msgB.debug_name = "NodeB";
    msgB.work_function = []() {};
    msgB.run_after.push_back(idA);
    auto idB = builder.add_node(msgB);

    auto graph = builder.bake();

    // Check we have waves
    // Check we have nodes
    ASSERT_FALSE(graph.nodes.empty());

    // Since B depends on A, A must have B in dependents.
    // keys: A -> B
    bool found_edge = false;
    for (int dep : graph.nodes[idA].dependents) {
        if (dep == idB)
            found_edge = true;
    }
    EXPECT_TRUE(found_edge) << "Node A should have edge to Node B";
    EXPECT_GT(graph.nodes[idB].initial_dependencies, 0);
}

TEST(TopologyTest, ExplicitOrderingRunBefore) {
    Context ctx;
    GraphBuilder builder(ctx);

    // To test run_before, we create the dependent node first so we have its ID.
    // Then we create the prerequisite node and point it to the dependent.

    // 1. Add "Dependent" (B)
    NodeConfig msgDependent;
    msgDependent.debug_name = "B";
    msgDependent.work_function = []() {};
    auto idDependent = builder.add_node(msgDependent);

    // 2. Add "Prerequisite" (A) that says "I run before B"
    NodeConfig msgPrereq;
    msgPrereq.debug_name = "A";
    msgPrereq.work_function = []() {};
    msgPrereq.run_before.push_back(idDependent);
    builder.add_node(msgPrereq);

    auto graph = builder.bake();

    // With A -> B dependency, A must point to B.
    bool found_edge = false;
    // Note: ID of A (Prereq) is unknown until we added it. But it was added second.
    // IDs: Dependent=0, Prereq=1.
    // Prereq (1) -> Dependent (0).
    // run_before on Prereq means: Prereq -> Dependent.
    // So node 1 should have node 0 in dependents.

    // We can assume IDs 0 and 1 because we added them in order and default priority.
    int idPrereq = 1;

    for (int dep : graph.nodes[idPrereq].dependents) {
        if (dep == idDependent)
            found_edge = true;
    }
    EXPECT_TRUE(found_edge);
}

TEST(TopologyTest, ExplicitCycle) {
    Context ctx;
    GraphBuilder builder(ctx);

    // Test a circular dependency: A depends on B, and B depends on A.
    // Note: We rely on deterministic NodeID assignment (0, 1, 2...) here
    // because we need to reference Node B before it is added.

    // 1. Add A (ID 0). Declare it runs after B (ID 1).
    NodeConfig msgA;
    msgA.debug_name = "NodeA";
    msgA.work_function = []() {};
    msgA.run_after.push_back(1);
    builder.add_node(msgA);

    // 2. Add B (ID 1). Declare it runs after A (ID 0).
    NodeConfig msgB;
    msgB.debug_name = "NodeB";
    msgB.work_function = []() {};
    msgB.run_after.push_back(0);
    builder.add_node(msgB);

    EXPECT_THROW({ builder.bake(); }, std::runtime_error);
}
