#include "plexus/thread_pool.h"

namespace Plexus {

    ThreadPool::ThreadPool() {
        // Leave one core for the main thread/OS
        unsigned int count = std::thread::hardware_concurrency();
        if (count == 0)
            count = 2; // Fallback
        if (count > 1)
            count--;

        for (unsigned int i = 0; i < count; ++i) {
            m_workers.emplace_back(&ThreadPool::worker_thread, this);
        }
    }

    ThreadPool::~ThreadPool() {
        m_stop = true;
        m_cv_work.notify_all();
        for (auto &worker : m_workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    void ThreadPool::dispatch(const std::vector<Task> &tasks) {
        if (tasks.empty())
            return;

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_active_tasks += static_cast<int>(tasks.size());
            for (const auto &task : tasks) {
                m_queue.push(task);
            }
        }
        m_cv_work.notify_all();
    }

    void ThreadPool::enqueue(Task task) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_active_tasks++;
            m_queue.push(std::move(task));
        }
        m_cv_work.notify_one();
    }

    void ThreadPool::wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv_done.wait(lock, [this]() { return m_active_tasks == 0; });
    }

    void ThreadPool::worker_thread() {
        while (true) {
            Task task;
            {
                std::unique_lock<std::mutex> lock(m_mutex);
                m_cv_work.wait(lock, [this]() { return m_stop || !m_queue.empty(); });

                if (m_stop && m_queue.empty()) {
                    return;
                }

                task = std::move(m_queue.front());
                m_queue.pop();
            }

            task();

            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_active_tasks--;
                if (m_active_tasks == 0) {
                    m_cv_done.notify_all();
                }
            }
        }
    }

}
