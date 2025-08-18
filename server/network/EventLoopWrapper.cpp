#include "EventLoopWrapper.h"

#if defined(__linux__)
    #include <sys/epoll.h>
    #include <unistd.h>
#elif defined(__APPLE__) || defined(__FreeBSD__)
    #include <sys/event.h>
    #include <sys/time.h>
    #include <unistd.h>
#else
    #error "EventLoopWrapper is only for Linux/macOS/FreeBSD. Use IocpWrapper on Windows."
#endif

EventLoopWrapper::EventLoopWrapper()
{
#if defined(__linux__)
    loop_fd_ = epoll_create1(0);
#elif defined(__APPLE__) || defined(__FreeBSD__)
    loop_fd_ = kqueue();
#endif
}

EventLoopWrapper::~EventLoopWrapper()
{
    if (loop_fd_ != -1) ::close(loop_fd_);
}

bool EventLoopWrapper::add(int fd)
{
#if defined(__linux__)
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd;
    return epoll_ctl(loop_fd_, EPOLL_CTL_ADD, fd, &ev) == 0;
#elif defined(__APPLE__) || defined(__FreeBSD__)
    struct kevent ev{};
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    return kevent(loop_fd_, &ev, 1, nullptr, 0, nullptr) == 0;
#endif
}

bool EventLoopWrapper::remove(int fd)
{
#if defined(__linux__)
    return epoll_ctl(loop_fd_, EPOLL_CTL_DEL, fd, nullptr) == 0;
#elif defined(__APPLE__) || defined(__FreeBSD__)
    struct kevent ev{};
    EV_SET(&ev, fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    return kevent(loop_fd_, &ev, 1, nullptr, 0, nullptr) == 0;
#endif
}

std::vector<int> EventLoopWrapper::wait(int timeout_ms)
{
    std::vector<int> result;
#if defined(__linux__)
    epoll_event events[64];
    int nfds = epoll_wait(loop_fd_, events, 64, timeout_ms);
    if (nfds <= 0) return result;
    result.reserve(nfds);
    for (int i = 0; i < nfds; ++i)
        result.push_back(events[i].data.fd);

#elif defined(__APPLE__) || defined(__FreeBSD__)
    struct kevent events[64];
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = (timeout_ms % 1000) * 1000000;
    int nfds = kevent(loop_fd_, nullptr, 0, events, 64, &ts);
    if (nfds <= 0) return result;
    result.reserve(nfds);
    for (int i = 0; i < nfds; ++i)
        result.push_back((int)events[i].ident);
#endif
    return result;
}
