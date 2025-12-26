#include "plexus/graph_builder.h"
#include <gtest/gtest.h>
#include <sstream>

TEST(GraphVizTest, DebugDump) {
    Plexus::Context ctx;
    Plexus::GraphBuilder builder(ctx);

    // Create a simple graph
    // A -> B
    //      |
    //      v
    //      C

    auto a = builder.add_node({"NodeA", []() {}, {}, {}, {}, 10});
    auto b = builder.add_node({"NodeB", []() {}, {}, {a}, {}, 5});
    auto c = builder.add_node({"NodeC", []() {}, {}, {b}, {}, 0});

    auto graph = builder.bake();

    std::stringstream ss;
    graph.dump_debug(ss);
    std::string output = ss.str();

    // Check for presence of labels
    EXPECT_NE(output.find("NodeA"), std::string::npos);
    EXPECT_NE(output.find("NodeB"), std::string::npos);
    EXPECT_NE(output.find("NodeC"), std::string::npos);

    // Check for priority presence (heuristic check)
    EXPECT_NE(output.find("Priority:"), std::string::npos);

    // Check for structure (heuristic)
    // Node A should be an entry node
    EXPECT_NE(output.find("Entry Nodes:"), std::string::npos);

    // A simple sanity check on the output format
    EXPECT_NE(output.find("Execution Graph Dump:"), std::string::npos);

    if (std::getenv("PLEXUS_TEST_VERBOSE")) {
        std::cout << output << std::endl;
    }
}
