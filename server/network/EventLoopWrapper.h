#pragma once
#include <vector>

class EventLoopWrapper 
{
public:
    EventLoopWrapper();
    ~EventLoopWrapper();

    bool add(int fd);
    bool remove(int fd);
    std::vector<int> wait(int timeout_ms = 1000);

private:
    int loop_fd_; // epoll_fd 或 kqueue_fd
};
