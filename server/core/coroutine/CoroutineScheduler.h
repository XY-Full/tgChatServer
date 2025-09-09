#pragma once
#include "libcotask/task.h"
#include "libcotask/task_manager.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

class AppMsg;
using AppMsgPtr = std::shared_ptr<AppMsg>;

#define CUR_CO_ID (CoroutineScheduler::current_id())

class CoroutineScheduler
{
public:
    using Task = cotask::task<>;                       // 无返回值任务
    using TaskPtr = copp::memory::intrusive_ptr<Task>; // 使用 intrusive_ptr_ptr 管理任务
    using TaskFn = std::function<void()>;
    using TaskManager = cotask::task_manager<Task>;
    using TaskManagerPtr = typename TaskManager::ptr_type;

    explicit CoroutineScheduler(size_t worker_count = std::thread::hardware_concurrency(), size_t max_concurrency = 0);
    ~CoroutineScheduler();

    CoroutineScheduler(const CoroutineScheduler &) = delete;
    CoroutineScheduler &operator=(const CoroutineScheduler &) = delete;

    // 获取当前协程的 ID，如果不是在协程上下文中调用，返回 0
    static uint64_t current_id();

    // 启动调度器
    void start();
    // 停止调度器
    void stop();

    // 调度一个新任务，返回协程ID
    uint64_t schedule(TaskFn fn);

    // yield: 当前协程挂起指定时间后自动恢复
    static AppMsgPtr yield(std::chrono::milliseconds timeout = std::chrono::milliseconds(3000));

    // resume: 主动唤醒指定协程
    bool resume(uint64_t task_id, AppMsgPtr value = nullptr);

    // 处理超时任务（需要定期调用）
    void tick();

private:
    struct TaskItem
    {
        uint64_t id;
        TaskPtr task;
        std::atomic<bool> waiting{false}; // 标记协程是否挂起
        std::mutex val_mtx;
        std::condition_variable val_cv;
        AppMsgPtr resume_value;                              // 用于存储传递的参数
        std::chrono::steady_clock::time_point timeout_point; // 超时时间点
        std::atomic<bool> timeout_enabled{false};            // 是否启用超时
        std::atomic<bool> timed_out{false};                  // 是否已超时
    };

    void worker_routine(size_t worker_id);
    void enqueue_ready(std::shared_ptr<TaskItem> t);
    std::shared_ptr<TaskItem> find_task(uint64_t id);

private:
    size_t worker_count_;
    size_t max_concurrency_;
    std::vector<std::thread> workers_;

    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;
    std::queue<std::shared_ptr<TaskItem>> task_queue_;

    std::mutex all_mtx_;
    std::unordered_map<uint64_t, std::shared_ptr<TaskItem>> all_tasks_;

    // 超时管理
    TaskManagerPtr task_manager_;
    std::thread timeout_thread_;
    std::atomic<bool> timeout_thread_running_{false};
    void timeout_worker_routine();

    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> id_gen_{1};

    static thread_local std::shared_ptr<TaskItem> tls_current_task_;
};
