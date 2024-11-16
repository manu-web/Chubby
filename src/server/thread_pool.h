#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include <vector>
#include <mutex>
#include <queue>
#include <condition_variable>
#include <thread>
#include <functional>

// Simple ThreadPool class
class ThreadPool {
public:
    ThreadPool(size_t num_threads);
    ~ThreadPool();

    // Add a task to the queue
    void enqueue(std::function<void()> task);

private:
    std::vector<std::thread> workers;
    std::queue<std::function<void()>> tasks;

    std::mutex queue_mutex;
    std::condition_variable condition;
    bool stop;

    void worker_thread();
};

#endif // THREAD_POOL_H