#include "CoroutineScheduler.h"

thread_local std::shared_ptr<CoroutineScheduler::TaskItem> CoroutineScheduler::tls_current_task_ = nullptr;

CoroutineScheduler::CoroutineScheduler(size_t worker_count, size_t max_concurrency)
    : worker_count_(worker_count), max_concurrency_(max_concurrency)
{
    task_manager_ = TaskManager::create();
}

CoroutineScheduler::~CoroutineScheduler()
{
    stop();
}

void CoroutineScheduler::start()
{
    stopping_ = false;
    timeout_thread_running_ = true;

    // 启动超时处理线程
    timeout_thread_ = std::thread([this]() { timeout_worker_routine(); });

    for (size_t i = 0; i < worker_count_; ++i)
    {
        workers_.emplace_back([this, i]() { worker_routine(i); });
    }
}

void CoroutineScheduler::stop()
{
    stopping_ = true;
    timeout_thread_running_ = false;

    queue_cv_.notify_all();

    // 停止超时线程
    if (timeout_thread_.joinable())
        timeout_thread_.join();

    for (auto &w : workers_)
    {
        if (w.joinable())
            w.join();
    }
    workers_.clear();

    // 清理任务管理器
    if (task_manager_)
    {
        task_manager_->reset();
    }
}

uint64_t CoroutineScheduler::schedule(TaskFn fn)
{
    auto id = id_gen_.fetch_add(1);
    auto item = std::make_shared<TaskItem>();
    item->id = id;
    item->task = Task::create([fn, item]() {
        tls_current_task_ = item;
        fn();
        tls_current_task_ = nullptr;
    });

    {
        std::lock_guard<std::mutex> lock(all_mtx_);
        all_tasks_[id] = item;
    }

    enqueue_ready(item);
    return id;
}

uint64_t CoroutineScheduler::current_id()
{
    if (tls_current_task_)
    {
        return tls_current_task_->id;
    }
    return 0; // 主线程或非调度器内执行时
}

AppMsgPtr CoroutineScheduler::yield(std::chrono::milliseconds timeout)
{
    auto t = tls_current_task_;

    if (!t)
    {
        // 没有任务时直接让当前协程让出
        cotask::this_task::get_task()->yield();
        return nullptr;
    }

    // 如果超时时间小于等于 0，永远休眠
    if (timeout <= std::chrono::milliseconds(0))
    {
        t->waiting.store(true, std::memory_order_relaxed);
        t->timeout_enabled.store(false, std::memory_order_relaxed);
        t->timed_out.store(false, std::memory_order_relaxed);

        cotask::this_task::get_task()->yield();
    }
    else
    {
        // 超时机制
        t->waiting.store(true, std::memory_order_relaxed);
        t->timeout_point = std::chrono::steady_clock::now() + timeout;
        t->timeout_enabled.store(true, std::memory_order_relaxed);
        t->timed_out.store(false, std::memory_order_relaxed);

        cotask::this_task::get_task()->yield();
    }

    // 被 resume 唤醒后，取出参数
    std::unique_lock<std::mutex> lk(t->val_mtx);

    // 如果超时了
    if (t->timed_out.load(std::memory_order_relaxed))
    {
        return nullptr; // 超时返回 nullptr
    }

    return t->resume_value;
}

bool CoroutineScheduler::resume(uint64_t task_id, AppMsgPtr value)
{
    auto t = find_task(task_id);
    if (!t)
        return false;
    if (t->task && !t->task->is_completed())
    {
        bool expected = true;
        if (t->waiting.compare_exchange_strong(expected, false))
        {
            {
                std::lock_guard<std::mutex> lk(t->val_mtx);
                t->resume_value = std::move(value);
                t->timeout_enabled.store(false, std::memory_order_relaxed);
            }
            enqueue_ready(t);
            return true;
        }
    }
    return false;
}

void CoroutineScheduler::tick()
{
    if (task_manager_)
    {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        task_manager_->tick(time_t_now, 0);
    }
}

void CoroutineScheduler::timeout_worker_routine()
{
    while (timeout_thread_running_)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 10ms检查一次

        auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<TaskItem>> timeout_tasks;

        {
            std::lock_guard<std::mutex> lock(all_mtx_);
            for (auto &[id, task] : all_tasks_)
            {
                if (task->timeout_enabled.load(std::memory_order_relaxed) &&
                    task->waiting.load(std::memory_order_relaxed) && now >= task->timeout_point)
                {
                    timeout_tasks.push_back(task);
                }
            }
        }

        // 处理超时任务
        for (auto &task : timeout_tasks)
        {
            bool expected = true;
            if (task->waiting.compare_exchange_strong(expected, false))
            {
                {
                    std::lock_guard<std::mutex> lk(task->val_mtx);
                    task->timed_out.store(true, std::memory_order_relaxed);
                    task->timeout_enabled.store(false, std::memory_order_relaxed);
                    task->resume_value = nullptr; // 超时时不传递值
                }
                enqueue_ready(task);
            }
        }
    }
}

void CoroutineScheduler::enqueue_ready(std::shared_ptr<TaskItem> t)
{
    {
        std::lock_guard<std::mutex> lock(queue_mtx_);
        task_queue_.push(t);
    }
    queue_cv_.notify_one();
}

std::shared_ptr<CoroutineScheduler::TaskItem> CoroutineScheduler::find_task(uint64_t id)
{
    std::lock_guard<std::mutex> lock(all_mtx_);
    auto it = all_tasks_.find(id);
    return it == all_tasks_.end() ? nullptr : it->second;
}

void CoroutineScheduler::worker_routine(size_t worker_id)
{
    while (!stopping_)
    {
        std::shared_ptr<TaskItem> task;
        {
            std::unique_lock<std::mutex> lock(queue_mtx_);
            queue_cv_.wait(lock, [this]() { return stopping_ || !task_queue_.empty(); });
            if (stopping_)
                break;
            task = task_queue_.front();
            task_queue_.pop();
        }

        if (!task)
            continue;
        if (task->task->is_completed())
            continue;

        tls_current_task_ = task;
        task->task->resume();
        tls_current_task_ = nullptr;

        if (!task->task->is_completed() && !task->waiting.load(std::memory_order_relaxed))
        {
            enqueue_ready(task);
        }

        // 清理已完成的任务
        if (task->task->is_completed())
        {
            std::lock_guard<std::mutex> lock(all_mtx_);
            all_tasks_.erase(task->id);
        }
    }
}
