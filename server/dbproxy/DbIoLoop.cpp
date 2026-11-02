#include "DbIoLoop.h"

#include "Log.h"

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

// 与 MariaDB 的 MYSQL_WAIT_* 保持一致
static constexpr int kWaitRead    = 1;
static constexpr int kWaitWrite   = 2;
static constexpr int kWaitExcept  = 4;
static constexpr int kWaitTimeout = 8;

DbIoLoop::DbIoLoop() = default;

DbIoLoop::~DbIoLoop()
{
    stop();
}

bool DbIoLoop::start()
{
    if (running_.load()) return true;

    epfd_    = ::epoll_create1(EPOLL_CLOEXEC);
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (epfd_ < 0 || wake_fd_ < 0)
    {
        ELOG << "DbIoLoop: failed to create epoll/eventfd";
        return false;
    }

    epoll_event ev{};
    ev.events  = EPOLLIN;
    ev.data.fd = wake_fd_;
    if (::epoll_ctl(epfd_, EPOLL_CTL_ADD, wake_fd_, &ev) != 0)
    {
        ELOG << "DbIoLoop: failed to register wake fd";
        return false;
    }

    running_.store(true);
    thread_ = std::thread([this] { run(); });
    ILOG << "DbIoLoop: started";
    return true;
}

void DbIoLoop::stop()
{
    if (!running_.exchange(false)) return;

    post([] {});
    if (thread_.joinable()) thread_.join();

    if (wake_fd_ >= 0) { ::close(wake_fd_); wake_fd_ = -1; }
    if (epfd_ >= 0)    { ::close(epfd_);    epfd_    = -1; }

    ILOG << "DbIoLoop: stopped";
}

void DbIoLoop::post(Task task)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        tasks_.push(std::move(task));
    }
    if (wake_fd_ >= 0)
    {
        const uint64_t one = 1;
        // 唤醒run函数里的epoll_wait，立刻开始处理新任务
        const auto n = ::write(wake_fd_, &one, sizeof(one));
        (void)n;
    }
}

int DbIoLoop::armFd(int fd, const Waiter& w)
{
    epoll_event ev{};
    if (w.mask & kWaitRead)   ev.events |= EPOLLIN;
    if (w.mask & kWaitWrite)  ev.events |= EPOLLOUT;
    if (w.mask & kWaitExcept) ev.events |= (EPOLLERR | EPOLLHUP | EPOLLRDHUP);
    if (ev.events == 0)       ev.events = EPOLLIN; // 只等超时时至少挂上可读
    ev.data.fd = fd;

    const bool exists = armed_.count(fd) > 0;
    const int  op     = exists ? EPOLL_CTL_MOD : EPOLL_CTL_ADD;
    armed_[fd] = true;
    return ::epoll_ctl(epfd_, op, fd, &ev);
}

void DbIoLoop::waitSocket(int fd, int mask, int timeout_ms, std::function<void(int status)> cb)
{
    Waiter w;
    w.mask = mask;
    w.cb   = std::move(cb);
    w.has_timer = (mask & kWaitTimeout) != 0 && timeout_ms > 0;
    if (w.has_timer)
        w.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    waiters_[fd] = std::move(w);
    armFd(fd, waiters_[fd]);
}

void DbIoLoop::cancelWait(int fd)
{
    waiters_.erase(fd);
    if (armed_.erase(fd) > 0 && epfd_ >= 0) ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
}

void DbIoLoop::drainTasks()
{
    std::queue<Task> local;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        local.swap(tasks_);
    }
    while (!local.empty())
    {
        auto t = std::move(local.front());
        local.pop();
        t();
    }
}

void DbIoLoop::run()
{
    while (running_.load())
    {
        drainTasks();

        int timeout_ms = 1000;
        const auto now = std::chrono::steady_clock::now();
        for (const auto& kv : waiters_)
        {
            if (!kv.second.has_timer) continue;
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  kv.second.deadline - now).count();
            timeout_ms = std::min<int>(timeout_ms, static_cast<int>(std::max<int64_t>(left, 0)));
        }

        epoll_event events[64];
        const int n = ::epoll_wait(epfd_, events, 64, timeout_ms);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            ELOG << "DbIoLoop: epoll_wait failed: " << std::strerror(errno);
            continue;
        }

        for (int i = 0; i < n; ++i)
        {
            const int fd = events[i].data.fd;
            if (fd == wake_fd_)
            {
                uint64_t v = 0;
                const auto r = ::read(wake_fd_, &v, sizeof(v));
                (void)r;
                continue;
            }

            auto it = waiters_.find(fd);
            if (it == waiters_.end()) continue;

            int status = 0;
            if (events[i].events & (EPOLLIN | EPOLLRDHUP)) status |= kWaitRead;
            if (events[i].events & EPOLLOUT)                status |= kWaitWrite;
            if (events[i].events & (EPOLLERR | EPOLLHUP))   status |= kWaitExcept;

            auto cb = std::move(it->second.cb);
            waiters_.erase(it);
            armed_.erase(fd);
            ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
            cb(status);
        }

        const auto after = std::chrono::steady_clock::now();
        for (auto it = waiters_.begin(); it != waiters_.end();)
        {
            if (!it->second.has_timer || after < it->second.deadline)
            {
                ++it;
                continue;
            }
            auto cb = std::move(it->second.cb);
            const int fd = it->first;
            it = waiters_.erase(it);
            armed_.erase(fd);
            ::epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
            cb(kWaitTimeout);
        }
    }

    drainTasks();
}
