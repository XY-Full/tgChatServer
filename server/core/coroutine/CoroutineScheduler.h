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

// 协程等待状态机
//
// None      -> 不在等待
// Prepared  -> 已调用prepare_wait，此时异步事件已发起，但还没真正挂起
// Suspended -> 已真正挂起，控制权已回到worker线程，此时才能安全入队唤醒
// Woken     -> 已被resume/超时唤醒，等协程自己取走结果
//
// 之所以要区分Prepared/Suspended，是因为resume可能在协程真正挂起之前就发生
// 即异步事件先完成、协程后挂起，此时不能入队，否则另一个worker会对一个
// 还在运行的协程做resume，造成栈错乱；这种情况交给owner worker在
// task->resume() 返回后重新检查并排队
enum class WaitState : int
{
    None      = 0,
    Prepared  = 1,
    Suspended = 2,
    Woken     = 3
};

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

    // 准备等待：必须在发起异步事件之前调用
    // 否则事件可能在 yield 之前就完成，resume 找不到等待中的协程，唤醒会被丢弃
    static bool prepare_wait();

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

        // 下面所有字段都由 val_mtx 保护：状态迁移必须和唤醒判断在同一临界区内
        std::mutex              val_mtx;
        std::condition_variable val_cv;
        std::atomic<int>        wait_state{static_cast<int>(WaitState::None)};
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
