#include "plexus/context.h"
#include "plexus/graph_builder.h"
#include <gtest/gtest.h>

TEST(TopologyTest, ReadAfterWrite) {
    Plexus::Context ctx;
    auto res_id = ctx.register_resource("BufferA");

    Plexus::GraphBuilder builder(ctx);

    // Node A writes to BufferA
    // Node B reads from BufferA
    // Expectation: A runs in Wave 0, B runs in Wave 1

    bool a_ran = false;
    bool b_ran = false;

    builder.add_node({"WriterA", [&]() { a_ran = true; }, {{res_id, Plexus::Access::WRITE}}});

    builder.add_node({"ReaderB", [&]() { b_ran = true; }, {{res_id, Plexus::Access::READ}}});

    auto graph = builder.bake();

    // Check we have nodes
    ASSERT_FALSE(graph.nodes.empty());

    // Expectation: A -> B
    // A is Node 0, B is Node 1

    // Verify A -> B edge
    bool a_to_b = false;
    for (int dep : graph.nodes[0].dependents)
        if (dep == 1)
            a_to_b = true;
    EXPECT_TRUE(a_to_b);
    EXPECT_GE(graph.nodes[1].initial_dependencies, 1);
}

TEST(TopologyTest, WriteAfterRead) {
    Plexus::Context ctx;
    auto res_id = ctx.register_resource("BufferA");

    Plexus::GraphBuilder builder(ctx);

    // Node A reads BufferA
    // Node B writes BufferA
    // Expectation: A -> B

    builder.add_node({"ReaderA", []() {}, {{res_id, Plexus::Access::READ}}});

    builder.add_node({"WriterB", []() {}, {{res_id, Plexus::Access::WRITE}}});

    auto graph = builder.bake();

    // Expectation: A -> B (WAR)
    // A is Node 0, B is Node 1

    bool a_to_b = false;
    for (int dep : graph.nodes[0].dependents)
        if (dep == 1)
            a_to_b = true;
    EXPECT_TRUE(a_to_b);
    EXPECT_GE(graph.nodes[1].initial_dependencies, 1);
}
