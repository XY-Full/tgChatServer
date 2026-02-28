#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "EventLoopWrapper.h"
#include "MsgWrapper.h"
#include "TcpConnection.h"
#include "shm/shm_ringbuffer.h"

class PackBase;

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
    void eventLoop();

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
    std::thread event_thread_;

    std::string shm_name_;
    std::unique_ptr<ShmRingBuffer<uint8_t>> tcp_recv_buffer_;

    RecvHandler recv_handler_;
};
