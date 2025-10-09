#include "TcpServer.h"
#include "EventLoopWrapper.h"
#include "GlobalSpace.h"
#include "Log.h"
#include "MsgWrapper.h"
#include "Timer.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

TcpServer::TcpServer(int32_t port, RecvHandler recv_handler, std::string shm_name, CloseHandler close_handler,
                     ConnHandler conn_handler)
    : recv_handler_(recv_handler), shm_name_(shm_name), close_handler_(close_handler), conn_handler_(conn_handler)
{
    server_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd_ == -1)
    {
        ELOG << "Failed to create socket: " << strerror(errno);
        exit(1);
    }

    int opt = 1;
    // if (setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    // {
    //     ELOG << "Failed to set SO_REUSEADDR: " << strerror(errno);
    //     close(server_fd_);
    //     exit(1);
    // }

    // 设置TCP_NODELAY减少小数据包的延迟
    if (setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1)
    {
        ELOG << "Failed to set TCP_NODELAY: " << strerror(errno);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd_, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ELOG << "Failed to bind to port " << port << ": " << strerror(errno);
        close(server_fd_);
        exit(1);
    }

    ILOG << "Server started on port " << port;

    if (listen(server_fd_, MAX_EVENTS) != 0)
    {
        ELOG << "Failed to listen: " << strerror(errno);
        close(server_fd_);
        exit(1);
    }

    // 注册服务器socket到epoll
    if (!epoller_.add(server_fd_, EventType::READ | EventType::EDGE_TRIGGER, [this](int fd, EventType events) {
            if ((events & EventType::READ) != EventType::NONE)
            {
                this->handleNewConnection(fd);
            }
        }))
    {
        ELOG << "Failed to add server fd to epoller";
        close(server_fd_);
        exit(1);
    }

    recv_buffer_ = new ShmRingBuffer<uint8_t>(shm_name_ + "_tcp_recv");
}

TcpServer::~TcpServer()
{
    stop();
    if (server_fd_ != -1)
    {
        ::close(server_fd_);
    }

    delete recv_buffer_;
}

void TcpServer::start()
{
    if (running_)
        return;

    running_ = true;
    accept_thread_ = std::thread(&TcpServer::acceptLoop, this);
    GlobalSpace()->timer_->runEvery(1.0f, std::bind(&TcpServer::checkHeartbeats, this));

    ILOG << "TcpServer started successfully";
}

void TcpServer::stop()
{
    if (!running_)
        return;

    running_ = false;

    // 先停止事件循环，再关闭连接
    // epoller_.stop();

    if (accept_thread_.joinable())
    {
        accept_thread_.join();
    }

    // 关闭所有连接
    std::lock_guard<std::mutex> lock(conn_mutex_);
    for (auto &[conn_id, conn] : conn_map_)
    {
        conn->close();
    }
    conn_map_.clear();
    fd_to_conn_.clear();

    ILOG << "TcpServer stopped";
}

void TcpServer::acceptLoop()
{
    while (running_)
    {
        int nfds = epoller_.wait(1000);
        if (nfds > 0)
        {
            epoller_.processEvents();
        }
    }
}

void TcpServer::handleNewConnection(int fd)
{
    // 接受所有挂起的连接
    while (running_)
    {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);

        int client_fd = accept4(server_fd_, (sockaddr *)&client_addr, &addr_len, SOCK_NONBLOCK);
        if (client_fd == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break; // 没有新连接了
            }
            else
            {
                ELOG << "Accept failed, errno: " << errno;
                break;
            }
        }

        int64_t conn_id = next_conn_id_++;
        auto conn = std::make_shared<Connection>(
            client_fd, conn_id, epoller_,
            [this](int64_t conn_id, std::shared_ptr<PackBase> msg) {
                // 传递连接ID给recv_handler
                this->recv_handler_(conn_id, msg);
            },
            [this](int64_t conn_id) { removeConnection(conn_id); }, recv_buffer_);

        // 注册客户端连接事件处理
        if (!epoller_.add(client_fd, EventType::READ | EventType::EDGE_TRIGGER | EventType::RDHUP,
                          [this](int fd, EventType events) { this->handleClientEvent(fd, events); }))
        {
            ELOG << "Failed to add client fd " << client_fd << " to epoller";
            ::close(client_fd);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(conn_mutex_);
            conn_map_[conn_id] = conn;
            fd_to_conn_[client_fd] = conn_id;
            if (conn_handler_)
            {
                conn_handler_(conn_id);
            }
        }

        ILOG << "New connection: " << conn_id << " from " << inet_ntoa(client_addr.sin_addr);
    }
}

void TcpServer::handleClientEvent(int fd, EventType events)
{
    int64_t conn_id = -1;
    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        auto it = fd_to_conn_.find(fd);
        if (it != fd_to_conn_.end())
        {
            conn_id = it->second;
        }
    }

    if (conn_id == -1)
    {
        ELOG << "Unknown fd: " << fd;
        epoller_.remove(fd);
        ::close(fd);
        return;
    }

    auto conn_it = conn_map_.find(conn_id);
    if (conn_it == conn_map_.end())
    {
        ELOG << "Connection not found for fd: " << fd;
        epoller_.remove(fd);
        ::close(fd);

        std::lock_guard<std::mutex> lock(conn_mutex_);
        fd_to_conn_.erase(fd);
        return;
    }

    auto conn = conn_it->second;

    if ((events & EventType::RDHUP) != EventType::NONE)
    {
        // 对端关闭连接
        ILOG << "Connection closed by peer: " << conn_id;
        removeConnection(conn_id);
    }
    else
    {
        if ((events & EventType::READ) != EventType::NONE)
        {
            conn->onReadable();
        }
        if ((events & EventType::WRITE) != EventType::NONE)
        {
            conn->onWritable();
        }
    }
}

void TcpServer::checkHeartbeats()
{
    std::vector<int64_t> conns_to_close;

    {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        auto now = std::chrono::steady_clock::now();

        for (const auto &[conn_id, conn] : conn_map_)
        {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - conn->lastActiveTime()).count() >
                HEARTBEAT_TIMEOUT_SECONDS)
            {
                WLOG << "Heartbeat timeout for conn " << conn_id << ", kicking...";
                conns_to_close.push_back(conn_id);
            }
        }
    }

    for (int64_t conn_id : conns_to_close)
    {
        removeConnection(conn_id);
    }
}

void TcpServer::removeConnection(int64_t conn_id)
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = conn_map_.find(conn_id);
    if (it != conn_map_.end())
    {
        int fd = it->second->fd();
        it->second->close();
        epoller_.remove(fd);
        fd_to_conn_.erase(fd);
        conn_map_.erase(it);
        ILOG << "Connection removed: " << conn_id;
    }

    if (close_handler_)
    {
        close_handler_(conn_id);
    }
}

int32_t TcpServer::send(int32_t conn_id, std::shared_ptr<AppMsgWrapper> pack)
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto it = conn_map_.find(conn_id);
    if (it != conn_map_.end())
    {
        it->second->send(pack);
        return 0;
    }
    return -1;
}