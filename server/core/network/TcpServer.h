#pragma once
#include "EventLoopWrapper.h"
#include "MsgWrapper.h"
#include "TcpConnection.h"
#include "shm/shm_ringbuffer.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>

class AppMsgWrapper;

class TcpServer
{
friend class Connection;
public:
    TcpServer(int32_t port, RecvHandler recv_handler, std::string shm_name, CloseHandler close_handler = nullptr, ConnHandler conn_handler = nullptr);
    ~TcpServer();

    void start();
    void stop();
    int32_t send(int32_t conn_id, std::shared_ptr<AppMsgWrapper> pack);

private:
    void acceptLoop();
    void checkHeartbeats();
    void removeConnection(int64_t conn_id);
    void handleNewConnection(int client_fd);
    void handleClientEvent(int fd, EventType events);

    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;
    std::mutex conn_mutex_;
    std::unordered_map<int64_t, std::shared_ptr<Connection>> conn_map_;
    std::unordered_map<int, int64_t> fd_to_conn_; // 快速查找映射
    std::atomic<int64_t> next_conn_id_{0};

    EventLoopWrapper epoller_;

    RecvHandler recv_handler_;
    CloseHandler close_handler_;
    ConnHandler conn_handler_;

    std::string shm_name_;

    uint64_t timer_id_;

    static constexpr int HEARTBEAT_TIMEOUT_SECONDS = 30;
    static constexpr int MAX_EVENTS = 128;
};