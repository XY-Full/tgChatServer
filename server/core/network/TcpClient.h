#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>

#include "EventLoopWrapper.h"
#include "MsgWrapper.h"
#include "TcpConnection.h"

class PackBase;
using RecvHandler = std::function<void(std::shared_ptr<PackBase>)>;

class TcpClient
{
public:
    TcpClient(const std::string &ip, int port, std::string shm_name, RecvHandler recv_handler);

    ~TcpClient();

    bool start();
    void stop();
    
    void send(std::shared_ptr<AppMsgWrapper> data);

private:
    bool connectToServer();
    void checkHeartbeat();

    void eventHandler(int fd, EventType events);
    void onReadable();
    void onWriteable();

    std::string ip_;
    int port_;
    int sock_ = -1;

    std::atomic<bool> running_ = false;
    std::chrono::steady_clock::time_point last_active_time_;
    const int HEARTBEAT_TIMEOUT_SECONDS = 30;

    EventLoopWrapper epoller_;
    std::shared_ptr<Connection> conn_;

    std::string shm_name_;

    RecvHandler recv_handler_;
};
