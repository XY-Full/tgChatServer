#pragma once
#include "EventLoopWrapper.h"
#include "PackBase.h"
#include <cstdint>
#include "../shm/shm_ringbuffer.h"

class TcpServer;
class AppMsgWrapper;

using RecvHandler = std::function<void(std::shared_ptr<PackBase>)>;
using CloseHandler = std::function<void(int64_t)>;

class Connection : public std::enable_shared_from_this<Connection> {
public:
    Connection(int fd, int64_t conn_id, const std::string& shm_name, EventLoopWrapper& epoller, RecvHandler recv_handler, CloseHandler close_handler);
    ~Connection();

    int fd() const { return fd_; }
    int64_t conn_id() const { return conn_id_; }

    void onReadable();
    void onWritable();

    void send(std::shared_ptr<AppMsgWrapper> pack);

    void updateActiveTime(); // 更新心跳
    std::chrono::time_point<std::chrono::steady_clock> lastActiveTime() const { return last_active_time_; }

    void close();

private:
    int fd_;
    int64_t conn_id_;
    std::string shm_name;
    std::chrono::steady_clock::time_point last_active_time_;

    // 接收缓冲区
    ShmRingBuffer<uint8_t>* recv_buffer_;
    // 发送缓冲区
    ShmRingBuffer<std::shared_ptr<AppMsgWrapper>>* send_buffer_;

    EventLoopWrapper* epoller_;
    RecvHandler recv_handler_;
    CloseHandler close_handler_;

    // 解析接收缓冲区中的数据包
    void processRecvBuffer();
};