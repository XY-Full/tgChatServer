#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>

/**
 * @brief DB 专用事件循环
 *
 * 独立于 bus 的 IO 循环，只负责 MySQL 连接 socket 的可读/可写/异常与超时
 * 所有 *_start / *_cont 的续跑都在这个线程上发生，因此 DB 等待不占用任何
 * 线程资源；单个在飞的查询只消耗一个数据库连接
 *
 * waitSocket 的 mask 沿用 MariaDB 的 MYSQL_WAIT_READ / WRITE / EXCEPT / TIMEOUT
 */
class DbIoLoop
{
public:
    using Task = std::function<void()>;

    DbIoLoop();
    ~DbIoLoop();

    DbIoLoop(const DbIoLoop&)            = delete;
    DbIoLoop& operator=(const DbIoLoop&) = delete;

    bool start();
    void stop();

    // 在事件循环线程上执行任务
    void post(Task task);

    // 等待socket事件，条件满足时以status进行回调
    void waitSocket(int fd, int mask, int timeout_ms, std::function<void(int status)> cb);

    void cancelWait(int fd);

    bool isRunning() const { return running_.load(); }

private:
    struct Waiter
    {
        int                                   mask       = 0;
        bool                                  has_timer  = false;
        std::chrono::steady_clock::time_point deadline;
        std::function<void(int status)>       cb;
    };

    void run();
    void drainTasks();
    int  armFd(int fd, const Waiter& w);

    int                             epfd_    = -1;
    int                             wake_fd_ = -1; // eventfd，用于唤醒循环处理新任务
    std::thread                     thread_;
    std::atomic<bool>               running_{false};

    std::mutex                      mutex_;
    std::condition_variable         cv_;
    std::queue<Task>                tasks_;
    std::unordered_map<int, Waiter> waiters_; // 循环线程独占
    std::unordered_map<int, bool>   armed_;   // 已注册到 epoll 的 fd
};
