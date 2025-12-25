#include "Cycles/graph_builder.h"
#include <gtest/gtest.h>

using namespace Cycles;

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
  builder.add_node(msgB);

  auto graph = builder.bake();

  // Check we have waves
  ASSERT_FALSE(graph.waves.empty());

  // Simplest case: A in Wave 0, B in Wave 1.
  // Ideally we'd inspect the tasks, but ExecutionGraph tasks are void()
  // For now, let's trust the waves count if they are separated.
  // Since B depends on A, and they are otherwise independent,
  // A MUST proceed B. Due to the way Kahn's algorithm works, A will be picked
  // up first (indegree 0), then B (indegree became 0). They won't share a wave
  // if B depends on A.

  ASSERT_GE(graph.waves.size(), 2);
  ASSERT_EQ(graph.waves[0].tasks.size(), 1);
  ASSERT_EQ(graph.waves[1].tasks.size(), 1);
}

TEST(TopologyTest, ExplicitOrderingRunBefore) {
  Context ctx;
  GraphBuilder builder(ctx);

  // Node A says "Run Before B"
  NodeConfig msgA;
  msgA.debug_name = "NodeA";
  msgA.work_function = []() {};
  auto idA = builder.add_node(msgA);

  NodeConfig msgB;
  msgB.debug_name = "NodeB";
  msgB.work_function = []() {};
  auto idB = builder.add_node(msgB);

  // Hack: Retroactively modify A... wait, GraphBuilder stores copies.
  // We cannot modify A after add_node.
  // We have to set the ID beforehand or rely on stable IDs?
  // NodeID is just an index.
  // So if we know A is 0 and B is 1...

  // Correct usage:
  // We want A to run before B.
  // But we defined A before knowing B's ID.
  // This is the tricky part of "run_before".
  // Ideally we'd use a forward declaration mechanism, but NodeID is returned on
  // add. So usually "run_after" is easier to use.

  // To test run_before, let's create B first? No, A runs BEFORE B.
  // So B must exist for A to point to it.
  // Let's create B first, but give it no dependencies.
  // Then create A, and say A runs before B.

  // Wait, if A runs before B, then B depends on A.
  // Adjacency: A -> B. Indegree[B]++.

  // Let's re-try adding nodes in an order that makes sense for the API.

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

  ASSERT_GE(graph.waves.size(), 2);
  // Wave 0 should be A (Prereq), Wave 1 should be B (Dependent)
}

TEST(TopologyTest, ExplicitCycle) {
  Context ctx;
  GraphBuilder builder(ctx);

  // A runs after B
  // B runs after A

  // This needs us to add A, then B, but A refers to B?
  // Circular dependency construction is tricky with sequential IDs.

  // 1. Add A.
  NodeConfig msgA;
  msgA.debug_name = "NodeA";
  msgA.work_function = []() {};
  // msgA runs after B... but B doesn't exist yet so we don't know ID.
  // But we know IDs are 0, 1, 2...
  // Let's assume ID of B is 1.
  msgA.run_after.push_back(1);
  builder.add_node(msgA); // ID 0

  // 2. Add B.
  NodeConfig msgB;
  msgB.debug_name = "NodeB";
  msgB.work_function = []() {};
  msgB.run_after.push_back(0); // Runs after A
  builder.add_node(msgB);      // ID 1

  EXPECT_THROW({ builder.bake(); }, std::runtime_error);
}
