#include "CoroutineScheduler.h"

#include "Log.h"

#include <exception>

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
        // 业务代码抛出的异常必须止步于协程内：否则会顺着 resume() 冒到 worker 线程，
        // 变成未捕获异常直接 terminate 整个进程
        try
        {
            fn();
        }
        catch (const std::exception& e)
        {
            ELOG << "CoroutineScheduler: task " << item->id
                 << " threw std::exception: " << e.what();
        }
        catch (...)
        {
            ELOG << "CoroutineScheduler: task " << item->id << " threw unknown exception";
        }
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

bool CoroutineScheduler::prepare_wait()
{
    auto t = tls_current_task_;
    if (!t) return false;

    std::lock_guard<std::mutex> lk(t->val_mtx);
    if (t->wait_state.load(std::memory_order_relaxed) != static_cast<int>(WaitState::None))
        return false;   // 上一次等待还没收尾

    t->resume_value.reset();
    t->timed_out.store(false, std::memory_order_relaxed);
    t->timeout_enabled.store(false, std::memory_order_relaxed);
    t->wait_state.store(static_cast<int>(WaitState::Prepared), std::memory_order_relaxed);
    return true;
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

    {
        std::lock_guard<std::mutex> lk(t->val_mtx);

        // 若异步事件完成得太快，唤醒先于挂起到达，则直接取走结果，不真正挂起
        if (t->wait_state.load(std::memory_order_relaxed) == static_cast<int>(WaitState::Woken))
        {
            auto v = std::move(t->resume_value);
            t->resume_value.reset();
            t->wait_state.store(static_cast<int>(WaitState::None), std::memory_order_relaxed);
            const bool to = t->timed_out.load(std::memory_order_relaxed);
            return to ? nullptr : v;
        }

        // 真正挂起；Suspended 由 owner worker 在resume返回后标记，
        // 保证只有确认挂起的协程才会被入队唤醒
        if (timeout > std::chrono::milliseconds(0))
        {
            t->timeout_point = std::chrono::steady_clock::now() + timeout;
            t->timeout_enabled.store(true, std::memory_order_relaxed);
        }
        else
        {
            t->timeout_enabled.store(false, std::memory_order_relaxed);
        }
        t->timed_out.store(false, std::memory_order_relaxed);
        t->resume_value.reset();
        t->wait_state.store(static_cast<int>(WaitState::Prepared), std::memory_order_relaxed);
    }

    cotask::this_task::get_task()->yield();

    // 被 resume 唤醒后，取出参数
    std::lock_guard<std::mutex> lk(t->val_mtx);
    auto v = std::move(t->resume_value);
    t->resume_value.reset();
    t->timeout_enabled.store(false, std::memory_order_relaxed);
    t->wait_state.store(static_cast<int>(WaitState::None), std::memory_order_relaxed);

    return t->timed_out.load(std::memory_order_relaxed) ? nullptr : v;
}
bool CoroutineScheduler::resume(uint64_t task_id, AppMsgPtr value)
{
    auto t = find_task(task_id);
    if (!t) return false;
    if (!t->task || t->task->is_completed()) return false;

    bool enqueue = false;
    {
        std::lock_guard<std::mutex> lk(t->val_mtx);
        const int st = t->wait_state.load(std::memory_order_relaxed);

        // 已确认挂起：可以安全入队唤醒
        if (st == static_cast<int>(WaitState::Suspended))
        {
            t->resume_value = std::move(value);
            t->timeout_enabled.store(false, std::memory_order_relaxed);
            t->wait_state.store(static_cast<int>(WaitState::Woken), std::memory_order_relaxed);
            enqueue = true;
        }
        // 还没真正挂起，缓存结果
        // owner worker 在 task->resume() 返回后会看到 Woken 并重新排队
        else if (st == static_cast<int>(WaitState::Prepared))
        {
            t->resume_value = std::move(value);
            t->timeout_enabled.store(false, std::memory_order_relaxed);
            t->wait_state.store(static_cast<int>(WaitState::Woken), std::memory_order_relaxed);
        }
        else
        {
            return false;   // 该协程没有在等待或已被唤醒过，丢弃这次唤醒
        }
    }

    if (enqueue) enqueue_ready(t);
    return true;
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

        const auto now = std::chrono::steady_clock::now();
        std::vector<std::shared_ptr<TaskItem>> timeout_tasks;

        {
            std::lock_guard<std::mutex> lock(all_mtx_);
            for (auto &[id, task] : all_tasks_)
            {
                // 只对已确认挂起的协程做超时唤醒：未真正挂起的交给 owner worker 处理
                if (task->timeout_enabled.load(std::memory_order_relaxed) &&
                    task->wait_state.load(std::memory_order_relaxed) == static_cast<int>(WaitState::Suspended) &&
                    now >= task->timeout_point)
                {
                    timeout_tasks.push_back(task);
                }
            }
        }

        for (auto &task : timeout_tasks)
        {
            bool enqueue = false;
            {
                std::lock_guard<std::mutex> lk(task->val_mtx);
                if (task->wait_state.load(std::memory_order_relaxed) != static_cast<int>(WaitState::Suspended))
                    continue;   // 期间已被 resume
                task->timed_out.store(true, std::memory_order_relaxed);
                task->timeout_enabled.store(false, std::memory_order_relaxed);
                task->resume_value = nullptr;
                task->wait_state.store(static_cast<int>(WaitState::Woken), std::memory_order_relaxed);
                enqueue = true;
            }
            if (enqueue) enqueue_ready(task);
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

        bool threw = false;
        tls_current_task_ = task;
        try
        {
            task->task->resume();
        }
        catch (const std::exception& e)
        {
            threw = true;
            ELOG << "CoroutineScheduler: resume task " << task->id
                 << " threw std::exception: " << e.what();
        }
        catch (...)
        {
            threw = true;
            ELOG << "CoroutineScheduler: resume task " << task->id << " threw unknown exception";
        }
        tls_current_task_ = nullptr;

        if (threw)
        {
            // 状态已不可信，直接清理，不再排队
            std::lock_guard<std::mutex> lock(all_mtx_);
            all_tasks_.erase(task->id);
            continue;
        }

        // 此刻协程要么已结束，要么已挂起
        bool requeue   = false;
        bool completed = false;
        {
            std::lock_guard<std::mutex> lk(task->val_mtx);
            completed = task->task->is_completed();
            if (!completed)
            {
                const int st = task->wait_state.load(std::memory_order_relaxed);
                if (st == static_cast<int>(WaitState::Woken))
                {
                    // 唤醒先于真正挂起发生，重新入队
                    requeue = true;
                }
                else if (st == static_cast<int>(WaitState::Prepared))
                {
                    // 已确认挂起
                    task->wait_state.store(static_cast<int>(WaitState::Suspended), std::memory_order_relaxed);
                }
            }
        }

        if (requeue)
            enqueue_ready(task);
        else if (completed)
        {
            std::lock_guard<std::mutex> lock(all_mtx_);
            all_tasks_.erase(task->id);
        }
    }
}
