#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "Channel.h"
#include "NetPack.h"
#include "SocketWrapper.h"
#include "Timer.h"

class TcpClient
{
public:
    TcpClient(const std::string &ip, int port, Channel<std::shared_ptr<NetPack>> *in,
              Channel<std::shared_ptr<NetPack>> *out, Timer *timer);

    ~TcpClient();

    void start();
    void stop();

private:
    void connectToServer();
    void recvLoop();
    void sendLoop();
    void checkHeartbeat();

    std::string ip_;
    int port_;
    int sock_ = -1;

    std::unique_ptr<SocketWrapper> socket_;
    Channel<std::shared_ptr<NetPack>> *recv_channel_;
    Channel<std::shared_ptr<NetPack>> *send_channel_;
    Timer *timer_;

    std::atomic<bool> running_ = false;
    std::thread recv_thread_;
    std::thread send_thread_;
    std::chrono::steady_clock::time_point last_active_time_;
    std::mutex conn_mutex_;
    const int HEARTBEAT_TIMEOUT_SECONDS = 30;
};
