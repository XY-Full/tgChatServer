#pragma once
#include "Channel.h"
#include "EventLoopWrapper.h"
#include "SocketWrapper.h"
#include "Timer.h"
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

class PackBase;
using RecvHandler = std::function<void(std::shared_ptr<PackBase>)>;

class TcpServer
{
public:
    TcpServer(int32_t port, RecvHandler recv_handler);
    ~TcpServer();

    void start();
    void stop();

    int32_t send(int32_t conn_id, PackBase *pack);

private:
    void acceptLoop();
    void cleanupConnection(int fd);
    void checkHeartbeats();

    int server_fd_;
    EventLoopWrapper epoller_;
    std::unordered_map<int, int64_t> fd_to_conn_;
    std::unordered_map<int64_t, std::shared_ptr<SocketWrapper>> conn_map_;
    std::atomic<int64_t> next_conn_id_{1};

    std::thread accept_thread_;
    std::atomic<bool> running_{false};

    // 心跳相关参数
    std::unordered_map<int64_t, std::chrono::steady_clock::time_point> last_active_time_;
    std::mutex conn_mutex_;
    const int HEARTBEAT_TIMEOUT_SECONDS = 30;
    RecvHandler recv_handler_;
};