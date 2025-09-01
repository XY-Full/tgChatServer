#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "SocketWrapper.h"
#include "../common/Timer.h"

class AppMsg;
using RecvHandler = std::function<void(const AppMsg &)>;

class TcpClient
{
public:
    TcpClient(const std::string &ip, int port, RecvHandler recv_handler);

    ~TcpClient();

    void start();
    void stop();
    
    void send(char* data, uint32_t len);

private:
    void connectToServer();
    void recvLoop();
    void checkHeartbeat();

    std::string ip_;
    int port_;
    int sock_ = -1;

    std::unique_ptr<SocketWrapper> socket_;

    std::atomic<bool> running_ = false;
    std::thread recv_thread_;
    std::chrono::steady_clock::time_point last_active_time_;
    std::mutex conn_mutex_;
    const int HEARTBEAT_TIMEOUT_SECONDS = 30;

    RecvHandler recv_handler_;
};
