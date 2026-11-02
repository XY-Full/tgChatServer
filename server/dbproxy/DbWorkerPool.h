#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

/**
 * @brief DB 结果处理线程池
 *
 * 把结果集转成 protobuf 回包、以及 bus Reply
 * 这类 CPU 活放在这里，避免占用 DB IO 循环线程
 */
class DbWorkerPool
{
public:
    using Task = std::function<void()>;

    explicit DbWorkerPool(int thread_count = 4);
    ~DbWorkerPool();

    DbWorkerPool(const DbWorkerPool&)            = delete;
    DbWorkerPool& operator=(const DbWorkerPool&) = delete;

    void start();
    void stop();
    void post(Task task);

private:
    void workerRoutine();

    int                       thread_count_;
    std::vector<std::thread>  workers_;
    std::mutex                mutex_;
    std::condition_variable   cv_;
    std::queue<Task>          tasks_;
    std::atomic<bool>         running_{false};
};
