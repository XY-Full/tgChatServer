#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <sys/epoll.h>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// 事件类型定义 - 使用 uint32_t 作为底层类型
enum class EventType : uint32_t
{
    NONE = 0,
    READ = EPOLLIN,
    WRITE = EPOLLOUT,
    ERROR = EPOLLERR,
    HANGUP = EPOLLHUP,
    RDHUP = EPOLLRDHUP,
    EDGE_TRIGGER = EPOLLET,
    ONE_SHOT = EPOLLONESHOT,
    READ_WRITE = EPOLLIN | EPOLLOUT
};

// 重载位运算符，使 EventType 可以组合使用
inline EventType operator|(EventType a, EventType b)
{
    return static_cast<EventType>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}

inline EventType operator&(EventType a, EventType b)
{
    return static_cast<EventType>(static_cast<uint32_t>(a) & static_cast<uint32_t>(b));
}

inline EventType &operator|=(EventType &a, EventType b)
{
    a = a | b;
    return a;
}

// 事件回调类型
using EventCallback = std::function<void(int fd, EventType events)>;

class EventLoopWrapper
{
public:
    EventLoopWrapper();
    ~EventLoopWrapper();

    // 添加文件描述符到事件循环，指定关注的事件类型和回调函数
    bool add(int fd, EventType events, EventCallback callback);

    // 修改文件描述符的事件类型
    bool modify(int fd, EventType events);

    // 修改文件描述符的回调函数
    bool updateCallback(int fd, EventCallback callback);

    // 从事件循环中移除文件描述符
    bool remove(int fd);

    // 等待事件发生，返回就绪的文件描述符数量
    int wait(int timeout_ms = 1000);

    // 获取最近一次wait调用中发生的事件
    const std::vector<std::pair<int, EventType>> &getReadyEvents() const;

    // 处理所有就绪的事件
    void processEvents();

    // 获取epoll本体fd
    int getLoopFd() const
    {
        return epoll_fd_;
    }

private:
    int epoll_fd_;
    std::vector<epoll_event> events_; // 用于epoll_wait的缓冲区

    // 存储就绪的事件（fd和事件类型）
    std::vector<std::pair<int, EventType>> ready_events_;

    // 存储每个fd的回调函数
    std::unordered_map<int, EventCallback> callbacks_;

    // 存储每个fd当前关注的事件类型
    std::unordered_map<int, EventType> monitored_events_;
};