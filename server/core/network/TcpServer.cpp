#include "TcpServer.h"
#include "Log.h"
#include <cstring>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <sys/socket.h>
#include "GlobalSpace.h"
#include "PackBase.h"

TcpServer::TcpServer(int32_t port, RecvHandler recv_handler)
    : recv_handler_(recv_handler)
{
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd_, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ELOG << "Failed to bind to port " << port;
        exit(1);
    }

    ILOG << "Server started on port " << port;

    listen(server_fd_, 128);
    epoller_.add(server_fd_);
}

TcpServer::~TcpServer()
{
    stop();
    ::close(server_fd_);
}

void TcpServer::start()
{
    running_ = true;
    accept_thread_ = std::thread(&TcpServer::acceptLoop, this);
    GlobalSpace()->timer_->runEvery(1.0f, std::bind(&TcpServer::checkHeartbeats, this));
}

void TcpServer::stop()
{
    running_ = false;
    if (accept_thread_.joinable())
        accept_thread_.join();
}

void TcpServer::acceptLoop()
{
    while (running_)
    {
        for (int fd : epoller_.wait(1000))
        {
            if (fd == server_fd_)
            {
                int client_fd = accept(server_fd_, nullptr, nullptr);
                int64_t conn_id = next_conn_id_++;
                epoller_.add(client_fd);
                conn_map_[conn_id] = std::make_shared<SocketWrapper>(client_fd);
                fd_to_conn_[client_fd] = conn_id;
                ILOG << "New connection: " << conn_id;
            }
            else
            {
                int64_t conn_id = fd_to_conn_[fd];
                std::string len_buf;
                auto socket_ = conn_map_[conn_id];
                if (!socket_)
                {
                    ELOG << "Invalid connection ID " << conn_id << ", closing connection";
                    cleanupConnection(fd);
                    continue;
                }

                std::string pack;
                // 读取消息仅做peek
                if (!socket_->recvAll(pack, sizeof(Header), true))
                {
                    ELOG << "Receive length failed. Reconnecting...";
                    stop();
                    start();
                    return;
                }

                auto header = *reinterpret_cast<Header*>(pack.data());
                if (header.pack_len_ <= 0)// || header.pack_len_ > 10 * 1024 * 1024)
                {
                    ELOG << "Invalid message length: " << header.pack_len_;
                    continue;
                }

                std::string msg;
                if (!socket_->recvAll(msg, header.pack_len_))
                {
                    ELOG << "Receive message body failed. Reconnecting...";
                    stop();
                    start();
                    return;
                }

                auto msg_base = std::shared_ptr<PackBase>((PackBase*)(msg.data()));
                last_active_time_[conn_id] = std::chrono::steady_clock::now();

                recv_handler_(msg_base);
            }
        }
    }
}

void TcpServer::checkHeartbeats()
{
    std::lock_guard<std::mutex> lock(conn_mutex_);
    auto now = std::chrono::steady_clock::now();
    std::vector<int> fds_to_close;

    for (const auto &[conn_id, last_time] : last_active_time_)
    {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_time).count() > HEARTBEAT_TIMEOUT_SECONDS)
        {
            WLOG << "Heartbeat timeout for conn " << conn_id << ", kicking...";
            for (const auto &[fd, id] : fd_to_conn_)
            {
                if (id == conn_id)
                {
                    fds_to_close.push_back(fd);
                    break;
                }
            }
        }
    }

    for (int fd : fds_to_close)
    {
        cleanupConnection(fd);
    }
}

void TcpServer::cleanupConnection(int fd)
{
    auto it = fd_to_conn_.find(fd);
    if (it != fd_to_conn_.end())
    {
        int64_t conn_id = it->second;
        conn_map_.erase(conn_id);
        fd_to_conn_.erase(fd);
        last_active_time_.erase(it->second);
        epoller_.remove(fd);
        ::close(fd);
        ILOG << "Closed connection: " << conn_id;
    }
}