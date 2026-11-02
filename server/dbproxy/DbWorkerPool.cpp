#include "DbWorkerPool.h"

DbWorkerPool::DbWorkerPool(int thread_count) : thread_count_(thread_count)
{
    if (thread_count_ < 1) thread_count_ = 1;
}

DbWorkerPool::~DbWorkerPool()
{
    stop();
}

void DbWorkerPool::start()
{
    if (running_.load()) return;
    running_.store(true);
    for (int i = 0; i < thread_count_; ++i)
        workers_.emplace_back([this] { workerRoutine(); });
}

void DbWorkerPool::stop()
{
    if (!running_.exchange(false)) return;
    cv_.notify_all();
    for (auto& w : workers_)
        if (w.joinable()) w.join();
    workers_.clear();
}

void DbWorkerPool::post(Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
}

void DbWorkerPool::workerRoutine()
{
    while (running_.load())
    {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return !running_.load() || !tasks_.empty(); });
            if (!running_.load() && tasks_.empty()) break;
            if (tasks_.empty()) continue;
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        task();
    }
}
