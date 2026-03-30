#pragma once
#include "../shm/shm_ringbuffer.h"
#include "EventLoopWrapper.h"
#include "AppMsg.h"
#include <chrono>
#include <cstdint>

class TcpServer;
class AppMsgWrapper;

using RecvHandler = std::function<void(uint64_t, std::shared_ptr<AppMsg>)>;
using CloseHandler = std::function<void(uint64_t)>;
using ConnHandler = std::function<void(uint64_t)>;

class Connection : public std::enable_shared_from_this<Connection>
{
public:
    Connection(int fd, int64_t conn_id, EventLoopWrapper &epoller,
               RecvHandler recv_handler, CloseHandler close_handler,
               std::unique_ptr<ShmRingBuffer<uint8_t>> recv_buffer);
    ~Connection();

    int fd() const
    {
        return fd_;
    }
    int64_t conn_id() const
    {
        return conn_id_;
    }

    void onReadable();
    void onWritable();

    AppMsg* get_pack_ptr(std::shared_ptr<AppMsgWrapper> pack);
    AppMsg* get_pack_ptr(AppMsg& pack);
    
    template<typename T>
    void send_impl(T&& pack);

    void send(std::shared_ptr<AppMsgWrapper> pack);
    void send(AppMsg& pack);

    void updateActiveTime(); // 更新心跳
    std::chrono::time_point<std::chrono::steady_clock> lastActiveTime() const
    {
        return last_active_time_;
    }

    void close();

private:
    int fd_;
    int64_t conn_id_;
    std::chrono::steady_clock::time_point last_active_time_;

    // 接收缓冲区（由 Connection 独占所有权，随 Connection 析构自动释放）
    std::unique_ptr<ShmRingBuffer<uint8_t>> recv_buffer_;

    // 添加部分发送状态跟踪
    std::vector<uint8_t> partial_send_buffer_; // 存储部分发送的数据
    size_t partial_send_offset_ = 0;           // 已发送字节数

    EventLoopWrapper *epoller_;
    RecvHandler recv_handler_;
    CloseHandler close_handler_;

    // 解析接收缓冲区中的数据包
    void processRecvBuffer();
};