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

Connection::Connection(int fd, int64_t conn_id, const std::string& shm_name, EventLoopWrapper& epoller, RecvHandler recv_handler, CloseHandler close_handler)
    : fd_(fd), conn_id_(conn_id), epoller_(&epoller), recv_handler_(recv_handler), close_handler_(close_handler)
{
    updateActiveTime();
    send_buffer_ = new ShmRingBuffer<std::shared_ptr<AppMsgWrapper>>(shm_name + "_send");
    recv_buffer_ = new ShmRingBuffer<uint8_t>(shm_name + "_recv");
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
    // 发送发送缓冲区中的数据
    if (send_buffer_->IsEmpty())
    {
        return;
    }

    std::shared_ptr<AppMsgWrapper> pack;
    send_buffer_->Pop(pack);

    auto pack_ptr = reinterpret_cast<AppMsg*>(GlobalSpace()->shm_slab_.off2ptr(pack->offset_));

    struct msghdr msg = {0};
    struct iovec iov;
    
    iov.iov_base = pack_ptr;
    iov.iov_len = pack_ptr->header_.pack_len_;
    
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    
    ssize_t sent = sendmsg(fd_, &msg, MSG_ZEROCOPY);
    if (sent > 0)
    {
        updateActiveTime();
    }
    else
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK)
        {
            ELOG << "Write error, conn_id: " << conn_id_ << ", errno: " << errno;
            close_handler_(conn_id_);
        }
    }

    // 如果发送缓冲区为空，取消监听写事件
    if (send_buffer_->IsEmpty())
    {
        epoller_->modify(fd_, EventType::READ | EventType::EDGE_TRIGGER | EventType::RDHUP);
    }
}

void Connection::send(std::shared_ptr<AppMsgWrapper> pack)
{
    // 将数据追加到发送缓冲区
    send_buffer_->Push(pack);

    // 如果发送缓冲区非空，需要监听写事件
    epoller_->modify(fd_, EventType::READ | EventType::WRITE | EventType::EDGE_TRIGGER | EventType::RDHUP);
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
    while (recv_buffer_->Size() >= sizeof(Header))
    {
        auto msg_base = std::make_shared<PackBase>();
        recv_buffer_->Pop(reinterpret_cast<uint8_t *>(msg_base.get()), sizeof(Header));

        if (unlikely(msg_base->header_.pack_len_ <= 0 || recv_buffer_->Size() < msg_base->header_.pack_len_))
        {
            // 将剩下的数据返还给接收缓冲区
            recv_buffer_->PushFront(reinterpret_cast<uint8_t *>(msg_base.get()), sizeof(Header));
            break; // 数据不完整，等待更多数据
        }

        // 刚刚读取了Header，现在把剩下的数据读进来
        recv_buffer_->Pop(reinterpret_cast<uint8_t *>(msg_base.get()) + sizeof(Header),
                          msg_base->header_.pack_len_ - sizeof(Header));
        recv_handler_(msg_base);
    }
}