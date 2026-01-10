#pragma once
#include <cstddef>

namespace Plexus {

    /**
     * @brief Returns the current worker thread index.
     *
     * @return Worker index in range [0, worker_count) when called from a worker thread,
     *         worker_count() when called from the main thread during ThreadAffinity::Main
     *         node execution, or SIZE_MAX if called from outside any Plexus execution context.
     *
     * @note Thread-safe: reads thread-local storage only.
     * @note Safe to call from within any node's work_function.
     */
    size_t current_worker_index();

    /**
     * @brief Returns the total number of worker threads.
     *
     * @return Number of workers in the thread pool, or 0 if called from outside
     *         any Plexus execution context.
     *
     * @note Thread-safe: reads thread-local storage only.
     */
    size_t worker_count();

} // namespace Plexus
