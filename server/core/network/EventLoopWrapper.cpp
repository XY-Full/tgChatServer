#include "EventLoopWrapper.h"
#include <stdexcept>

EventLoopWrapper::EventLoopWrapper()
{
    epoll_fd_ = epoll_create1(0);
    if (epoll_fd_ == -1)
    {
        throw std::runtime_error("Failed to create epoll instance: " + std::string(strerror(errno)));
    }
    events_.resize(64); // 初始大小，可根据需要调整
}

EventLoopWrapper::~EventLoopWrapper()
{
    if (epoll_fd_ != -1)
    {
        close(epoll_fd_);
    }
}

bool EventLoopWrapper::add(int fd, EventType events, EventCallback callback)
{
    epoll_event ev;
    ev.events = static_cast<uint32_t>(events); // 显式转换为 uint32_t
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1)
    {
        return false;
    }

    monitored_events_[fd] = events;
    callbacks_[fd] = callback;
    return true;
}

bool EventLoopWrapper::modify(int fd, EventType events)
{
    auto it = monitored_events_.find(fd);
    if (it == monitored_events_.end())
    {
        return false; // 文件描述符未注册
    }

    epoll_event ev;
    ev.events = static_cast<uint32_t>(events); // 显式转换为 uint32_t
    ev.data.fd = fd;

    if (epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev) == -1)
    {
        return false;
    }

    monitored_events_[fd] = events;
    return true;
}

bool EventLoopWrapper::updateCallback(int fd, EventCallback callback)
{
    auto it = callbacks_.find(fd);
    if (it == callbacks_.end())
    {
        return false; // 文件描述符未注册
    }

    callbacks_[fd] = callback;
    return true;
}

bool EventLoopWrapper::remove(int fd)
{
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr) == -1)
    {
        return false;
    }

    monitored_events_.erase(fd);
    callbacks_.erase(fd);
    return true;
}

int EventLoopWrapper::wait(int timeout_ms)
{
    int nfds = epoll_wait(epoll_fd_, events_.data(), events_.size(), timeout_ms);
    if (nfds == -1)
    {
        if (errno == EINTR)
        {
            return 0; // 被信号中断，不算错误
        }
        return -1; // 发生错误
    }

    static_assert(sizeof(epoll_event) == 12 || sizeof(epoll_event) == 16, 
              "Unexpected epoll_event size");
              
    // 清空就绪事件列表并重新填充
    ready_events_.clear();
    for (int i = 0; i < nfds; ++i)
    {
        EventType type = static_cast<EventType>(events_[i].events); // 直接转换回 EventType
        int32_t fd = events_[i].data.fd;
        ready_events_.emplace_back(fd, type);
    }

    // 如果需要，动态调整事件缓冲区大小
    if (nfds == static_cast<int>(events_.size()))
    {
        events_.resize(events_.size() * 2);
    }

    return nfds;
}

const std::vector<std::pair<int, EventType>> &EventLoopWrapper::getReadyEvents() const
{
    return ready_events_;
}

void EventLoopWrapper::processEvents()
{
    for (const auto &event : ready_events_)
    {
        int fd = event.first;
        EventType events = event.second;

        auto it = callbacks_.find(fd);
        if (it != callbacks_.end() && it->second)
        {
            it->second(fd, events);
        }
    }
}