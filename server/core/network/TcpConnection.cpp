#include "TcpConnection.h"
#include "EventLoopWrapper.h"
#include "GlobalSpace.h"
#include "Log.h"
#include "MsgWrapper.h"
#include "TcpServer.h"
#include <cstring>
#include <errno.h>
#include <memory>
#include <unistd.h>

Connection::Connection(int fd, int64_t conn_id, EventLoopWrapper &epoller,
                       RecvHandler recv_handler, CloseHandler close_handler, ShmRingBuffer<uint8_t> *recv_buffer)
    : fd_(fd), conn_id_(conn_id), epoller_(&epoller), recv_handler_(recv_handler), close_handler_(close_handler), recv_buffer_(recv_buffer)
{
    updateActiveTime();

    // 初始化部分发送状态
    partial_send_offset_ = 0;
}

Connection::~Connection()
{
    close();
}

void Connection::onReadable()
{
    uint8_t buffer[4096];
    while (true)
    {
        ssize_t n = read(fd_, buffer, sizeof(buffer));
        if (n > 0)
        {
            // 在接收数据前添加连接ID头
            uint8_t header[sizeof(int64_t)];
            memcpy(header, &conn_id_, sizeof(conn_id_));
            recv_buffer_->Push(header, sizeof(header));
            recv_buffer_->Push(buffer, n);
            updateActiveTime();
        }
        else if (n == 0)
        {
            // 对端关闭
            close_handler_(conn_id_);
            break;
        }
        else
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;

            ELOG << "Read error, conn_id: " << conn_id_ << ", errno: " << errno;
            close_handler_(conn_id_);
            break;
        }
    }

    processRecvBuffer();
}

void Connection::onWritable()
{
    // 处理部分发送的数据
    if (!partial_send_buffer_.empty())
    {
        ssize_t sent = write(fd_, partial_send_buffer_.data() + partial_send_offset_,
                             partial_send_buffer_.size() - partial_send_offset_);

        if (sent > 0)
        {
            partial_send_offset_ += sent;
            updateActiveTime();

            // 检查是否发送完成
            if (partial_send_offset_ >= partial_send_buffer_.size())
            {
                partial_send_buffer_.clear();
                partial_send_offset_ = 0;

                // 发送完成，取消写事件监听
                epoller_->modify(fd_, EventType::READ | EventType::EDGE_TRIGGER | EventType::RDHUP);
            }
        }
        else
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                ELOG << "Partial write error, conn_id: " << conn_id_ << ", errno: " << errno;
                close_handler_(conn_id_);
            }
        }
        return;
    }
}

void Connection::send(std::shared_ptr<AppMsgWrapper> pack)
{
    auto pack_ptr = reinterpret_cast<AppMsg *>(GlobalSpace()->shm_slab_.off2ptr(pack->offset_));
    pack_ptr->header_.conn_id_ = conn_id_;

    // 尝试直接发送
    struct msghdr msg = {0};
    struct iovec iov;

    iov.iov_base = pack_ptr;
    iov.iov_len = pack_ptr->header_.pack_len_;

    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    ssize_t sent = sendmsg(fd_, &msg, MSG_ZEROCOPY);

    if (sent == static_cast<ssize_t>(pack_ptr->header_.pack_len_))
    {
        // 完整发送
        updateActiveTime();
    }
    else if (sent > 0)
    {
        // 部分发送，保存剩余数据
        partial_send_buffer_.resize(pack_ptr->header_.pack_len_ - sent);
        memcpy(partial_send_buffer_.data(), reinterpret_cast<uint8_t *>(pack_ptr) + sent, partial_send_buffer_.size());
        partial_send_offset_ = 0;

        // 注册写事件继续发送
        epoller_->modify(fd_, EventType::READ | EventType::WRITE | EventType::EDGE_TRIGGER | EventType::RDHUP);
    }
    else
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            ELOG << "Direct send error, conn_id: " << conn_id_ << ", errno: " << errno;
            close_handler_(conn_id_);
        }
        else
        {
            // 暂时无法发送，保存整个包
            partial_send_buffer_.resize(pack_ptr->header_.pack_len_);
            memcpy(partial_send_buffer_.data(), reinterpret_cast<uint8_t *>(pack_ptr), pack_ptr->header_.pack_len_);
            partial_send_offset_ = 0;

            // 注册写事件
            epoller_->modify(fd_, EventType::READ | EventType::WRITE | EventType::EDGE_TRIGGER | EventType::RDHUP);
        }
    }
}

void Connection::updateActiveTime()
{
    last_active_time_ = std::chrono::steady_clock::now();
}

void Connection::close()
{
    if (fd_ != -1)
    {
        ::close(fd_);
        fd_ = -1;
    }
}

void Connection::processRecvBuffer()
{
    // 解析接收缓冲区中的数据包
    while (recv_buffer_->Size() >= sizeof(Header) + sizeof(int64_t))
    {
        // 先读取连接ID头
        int64_t recv_conn_id = 0;
        recv_buffer_->Pop(reinterpret_cast<uint8_t *>(&recv_conn_id), sizeof(recv_conn_id));

        auto msg_base = std::make_shared<PackBase>();
        recv_buffer_->Pop(reinterpret_cast<uint8_t *>(msg_base.get()), sizeof(Header));

        // 设置消息头中的连接ID
        msg_base->header_.conn_id_ = recv_conn_id;

        if (unlikely(msg_base->header_.pack_len_ <= 0 || recv_buffer_->Size() < msg_base->header_.pack_len_))
        {
            // 将剩下的数据返还给接收缓冲区
            recv_buffer_->PushFront(reinterpret_cast<uint8_t *>(msg_base.get()), sizeof(Header));
            recv_buffer_->PushFront(reinterpret_cast<uint8_t *>(&recv_conn_id), sizeof(recv_conn_id));
            break; // 数据不完整，等待更多数据
        }

        // 读取剩余数据
        recv_buffer_->Pop(reinterpret_cast<uint8_t *>(msg_base.get()) + sizeof(Header),
                          msg_base->header_.pack_len_ - sizeof(Header));

        // 修复回调调用，添加连接ID参数
        recv_handler_(recv_conn_id, msg_base);
    }
}