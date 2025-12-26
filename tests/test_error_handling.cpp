#include "plexus/executor.h"
#include "plexus/graph_builder.h"
#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <thread>

// Helper to create a basic builder
class ErrorHandlingTest : public ::testing::Test {
protected:
    Plexus::Context ctx;
    Plexus::GraphBuilder builder{ctx};
    Plexus::ThreadPool pool;
    Plexus::Executor executor{pool};
};

TEST_F(ErrorHandlingTest, ContinuePolicy) {
    // A -> B
    // A fails with Continue. B should run.

    bool a_ran = false;
    bool b_ran = false;

    auto A = builder.add_node({.debug_name = "A",
                               .work_function =
                                   [&]() {
                                       a_ran = true;
                                       throw std::runtime_error("Fail A");
                                   },
                               .priority = 0,
                               .error_policy = Plexus::ErrorPolicy::Continue});

    builder.add_node(
        {.debug_name = "B", .work_function = [&]() { b_ran = true; }, .run_after = {A}});

    auto graph = builder.bake();

    // Executor should rethrow the exception
    EXPECT_THROW({ executor.run(graph); }, std::runtime_error);

    EXPECT_TRUE(a_ran);
    EXPECT_TRUE(b_ran); // B must run despite A failure
}

TEST_F(ErrorHandlingTest, CancelDependentsPolicy) {
    // A -> B
    // C (Independent)
    // A fails with CancelDependents. B should NOT run. C should run.

    std::atomic<bool> b_ran{false};
    std::atomic<bool> c_ran{false};

    auto A = builder.add_node({.debug_name = "A",
                               .work_function = []() { throw std::runtime_error("Fail A"); },
                               .error_policy = Plexus::ErrorPolicy::CancelDependents});

    builder.add_node(
        {.debug_name = "B", .work_function = [&]() { b_ran = true; }, .run_after = {A}});

    builder.add_node({.debug_name = "C", .work_function = [&]() { c_ran = true; }});

    auto graph = builder.bake();

    EXPECT_THROW({ executor.run(graph); }, std::runtime_error);

    EXPECT_FALSE(b_ran); // B skipped
    EXPECT_TRUE(c_ran);  // C independent
}

TEST_F(ErrorHandlingTest, CancelGraphPolicy) {
    // A (Fail Instant, CancelGraph)
    // B (Success, Slow) -> C (Success)

    std::atomic<bool> a_ran{false};
    std::atomic<bool> b_ran{false};
    std::atomic<bool> c_ran{false};

    auto A = builder.add_node({.debug_name = "A",
                               .work_function =
                                   [&]() {
                                       a_ran = true;
                                       throw std::runtime_error("Stop The World");
                                   },
                               .priority = 100, // High Priority to ensure it runs/fails early
                               .error_policy = Plexus::ErrorPolicy::CancelGraph});

    auto B = builder.add_node({.debug_name = "B", .work_function = [&]() {
                                   // Sleep to ensure A has time to fail
                                   std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                   b_ran = true;
                               }});

    builder.add_node({
        .debug_name = "C", .work_function = [&]() { c_ran = true; }, .run_after = {B} // After B
    });

    auto graph = builder.bake();

    EXPECT_THROW({ executor.run(graph); }, std::runtime_error);

    EXPECT_TRUE(a_ran);
    EXPECT_TRUE(b_ran);  // B started before A killed everything (or parallel)
    EXPECT_FALSE(c_ran); // C never scheduled because A killed graph during B's execution
}
