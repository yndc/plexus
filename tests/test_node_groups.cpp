#include "plexus/executor.h"
#include "plexus/graph_builder.h"
#include <algorithm>
#include <atomic>
#include <gtest/gtest.h>
#include <vector>

using namespace Plexus;

class NodeGroupTest : public ::testing::Test {
protected:
    Context ctx;
    Executor executor;

    NodeGroupTest() {}
};

// Test: Basic group ordering - sim_group -> pres_group
TEST_F(NodeGroupTest, BasicGroupOrdering) {
    GraphBuilder builder(ctx);

    // Define groups
    auto sim_group = builder.add_group({.name = "Simulation"});
    auto pres_group = builder.add_group({.name = "Presentation", .run_after = {sim_group}});

    std::atomic<int> counter{0};
    std::vector<int> sim_order(3, -1);
    std::vector<int> pres_order(2, -1);

    // Simulation nodes
    builder.add_node({.debug_name = "Sim1",
                      .work_function = [&]() { sim_order[0] = counter.fetch_add(1); },
                      .group_id = sim_group});
    builder.add_node({.debug_name = "Sim2",
                      .work_function = [&]() { sim_order[1] = counter.fetch_add(1); },
                      .group_id = sim_group});
    builder.add_node({.debug_name = "Sim3",
                      .work_function = [&]() { sim_order[2] = counter.fetch_add(1); },
                      .group_id = sim_group});

    // Presentation nodes (should run after all sim nodes complete)
    builder.add_node({.debug_name = "Pres1",
                      .work_function = [&]() { pres_order[0] = counter.fetch_add(1); },
                      .group_id = pres_group});
    builder.add_node({.debug_name = "Pres2",
                      .work_function = [&]() { pres_order[1] = counter.fetch_add(1); },
                      .group_id = pres_group});

    auto graph = builder.bake();
    executor.run(graph);

    // All sim nodes must complete before any pres node starts
    int max_sim = *std::max_element(sim_order.begin(), sim_order.end());
    int min_pres = *std::min_element(pres_order.begin(), pres_order.end());

    EXPECT_LT(max_sim, min_pres) << "All simulation nodes must complete before presentation starts";
}

// Test: run_after_group - standalone node syncing with a group
TEST_F(NodeGroupTest, RunAfterGroup) {
    GraphBuilder builder(ctx);

    auto sim_group = builder.add_group({.name = "Simulation"});

    std::atomic<int> counter{0};
    std::vector<int> sim_order(3, -1);
    int sync_order = -1;

    // Simulation nodes
    builder.add_node({.debug_name = "Sim1",
                      .work_function = [&]() { sim_order[0] = counter.fetch_add(1); },
                      .group_id = sim_group});
    builder.add_node({.debug_name = "Sim2",
                      .work_function = [&]() { sim_order[1] = counter.fetch_add(1); },
                      .group_id = sim_group});
    builder.add_node({.debug_name = "Sim3",
                      .work_function = [&]() { sim_order[2] = counter.fetch_add(1); },
                      .group_id = sim_group});

    // Sync node (standalone, depends on entire sim_group)
    builder.add_node({.debug_name = "SyncNode",
                      .work_function = [&]() { sync_order = counter.fetch_add(1); },
                      .run_after_group = {sim_group}});

    auto graph = builder.bake();
    executor.run(graph);

    int max_sim = *std::max_element(sim_order.begin(), sim_order.end());
    EXPECT_LT(max_sim, sync_order) << "Sync node must run after all simulation nodes";
}

// Test: run_before_group - node that must complete before a group starts
TEST_F(NodeGroupTest, RunBeforeGroup) {
    GraphBuilder builder(ctx);

    auto pres_group = builder.add_group({.name = "Presentation"});

    std::atomic<int> counter{0};
    int init_order = -1;
    std::vector<int> pres_order(2, -1);

    // Init node (runs before presentation group)
    builder.add_node({.debug_name = "InitNode",
                      .work_function = [&]() { init_order = counter.fetch_add(1); },
                      .run_before_group = {pres_group}});

    // Presentation nodes
    builder.add_node({.debug_name = "Pres1",
                      .work_function = [&]() { pres_order[0] = counter.fetch_add(1); },
                      .group_id = pres_group});
    builder.add_node({.debug_name = "Pres2",
                      .work_function = [&]() { pres_order[1] = counter.fetch_add(1); },
                      .group_id = pres_group});

    auto graph = builder.bake();
    executor.run(graph);

    int min_pres = *std::min_element(pres_order.begin(), pres_order.end());
    EXPECT_LT(init_order, min_pres) << "Init node must complete before any presentation node";
}

// Test: Full pipeline with sync nodes
TEST_F(NodeGroupTest, FullPipeline) {
    GraphBuilder builder(ctx);

    // Groups
    auto sim_group = builder.add_group({.name = "Simulation"});
    auto pres_group = builder.add_group({.name = "Presentation", .run_after = {sim_group}});

    std::atomic<int> counter{0};
    std::vector<int> sim_order(2, -1);
    int sync1_order = -1;
    std::vector<int> pres_order(2, -1);
    int sync2_order = -1;

    // Simulation phase
    builder.add_node({.debug_name = "Physics",
                      .work_function = [&]() { sim_order[0] = counter.fetch_add(1); },
                      .group_id = sim_group});
    builder.add_node({.debug_name = "AI",
                      .work_function = [&]() { sim_order[1] = counter.fetch_add(1); },
                      .group_id = sim_group});

    // Sync after simulation
    auto sync1 = builder.add_node({.debug_name = "SyncAfterSim",
                                   .work_function = [&]() { sync1_order = counter.fetch_add(1); },
                                   .run_after_group = {sim_group}});

    // Presentation phase (depends on sync1 explicitly AND group ordering)
    builder.add_node({.debug_name = "Cull",
                      .work_function = [&]() { pres_order[0] = counter.fetch_add(1); },
                      .run_after = {sync1},
                      .group_id = pres_group});
    builder.add_node({.debug_name = "Draw",
                      .work_function = [&]() { pres_order[1] = counter.fetch_add(1); },
                      .run_after = {sync1},
                      .group_id = pres_group});

    // Final sync
    builder.add_node({.debug_name = "SyncAfterPres",
                      .work_function = [&]() { sync2_order = counter.fetch_add(1); },
                      .run_after_group = {pres_group}});

    auto graph = builder.bake();
    executor.run(graph);

    int max_sim = *std::max_element(sim_order.begin(), sim_order.end());
    int min_pres = *std::min_element(pres_order.begin(), pres_order.end());
    int max_pres = *std::max_element(pres_order.begin(), pres_order.end());

    EXPECT_LT(max_sim, sync1_order) << "Sync1 must run after all sim nodes";
    EXPECT_LT(sync1_order, min_pres) << "Pres nodes must run after sync1";
    EXPECT_LT(max_pres, sync2_order) << "Sync2 must run after all pres nodes";
}

// Test: Parallel track merging (render thread running alongside main pipeline)
TEST_F(NodeGroupTest, ParallelTrackMerge) {
    GraphBuilder builder(ctx);

    auto sim_group = builder.add_group({.name = "Simulation"});

    std::atomic<int> counter{0};
    std::atomic<bool> sim_started{false};
    std::atomic<bool> render_started{false};
    int merge_order = -1;

    // Simulation nodes
    auto sim1 = builder.add_node({.debug_name = "Sim1",
                                  .work_function =
                                      [&]() {
                                          sim_started = true;
                                          counter.fetch_add(1);
                                      },
                                  .group_id = sim_group});

    // Render work (standalone, runs in parallel with simulation)
    auto render_work = builder.add_node({
        .debug_name = "RenderWork",
        .work_function =
            [&]() {
                render_started = true;
                counter.fetch_add(1);
            },
        // No group, no run_after - can start immediately
    });

    // Sync node after simulation
    auto sync = builder.add_node({.debug_name = "SyncAfterSim",
                                  .work_function = [&]() { counter.fetch_add(1); },
                                  .run_after_group = {sim_group}});

    // Merge point: waits for both sync AND render_work
    builder.add_node({.debug_name = "Merge",
                      .work_function = [&]() { merge_order = counter.fetch_add(1); },
                      .run_after = {sync, render_work}});

    auto graph = builder.bake();
    executor.run(graph);

    // Both tracks should have started
    EXPECT_TRUE(sim_started);
    EXPECT_TRUE(render_started);
    // Merge should be last (order 3, since we have 4 nodes total)
    EXPECT_EQ(merge_order, 3) << "Merge should be the last node to execute";
}

// Test: Empty group (no nodes in group)
TEST_F(NodeGroupTest, EmptyGroup) {
    GraphBuilder builder(ctx);

    auto empty_group = builder.add_group({.name = "Empty"});
    auto other_group = builder.add_group({.name = "Other", .run_after = {empty_group}});

    std::atomic<int> counter{0};
    int order = -1;

    // Only add node to other_group, empty_group has no nodes
    builder.add_node({.debug_name = "Node1",
                      .work_function = [&]() { order = counter.fetch_add(1); },
                      .group_id = other_group});

    auto graph = builder.bake();
    executor.run(graph);

    EXPECT_EQ(order, 0) << "Single node should execute";
}
