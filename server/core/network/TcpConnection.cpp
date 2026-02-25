#include "TcpConnection.h"
#include "EventLoopWrapper.h"
#include "GlobalSpace.h"
#include "Log.h"
#include "AppMsg.h"
#include "TcpServer.h"
#include <cstring>
#include <errno.h>
#include <memory>
#include <unistd.h>
#include "Helper.h"

Connection::Connection(int fd, int64_t conn_id, EventLoopWrapper &epoller,
                       RecvHandler recv_handler, CloseHandler close_handler,
                       std::unique_ptr<ShmRingBuffer<uint8_t>> recv_buffer)
    : fd_(fd), conn_id_(conn_id), epoller_(&epoller), recv_handler_(recv_handler), close_handler_(close_handler),
      recv_buffer_(std::move(recv_buffer))
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
            recv_buffer_->Push(buffer, n);
            DLOG << "Read " << n << " bytes from conn_id: " << conn_id_ << ", current recv buffer size: " << recv_buffer_->Size();
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

    ILOG << "Attempted to send " << iov.iov_len << " bytes to conn_id: " << conn_id_ << ", sent: " << sent;

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
    while (true)
    {
        // 检查是否有足够数据读 Header
        if (recv_buffer_->Size() < sizeof(Header)) break;

        ILOG << "Processing recv buffer for conn_id: " << conn_id_ << ", current buffer size: " << recv_buffer_->Size();

        // Peek Header
        uint8_t peek_buf[sizeof(Header)];
        recv_buffer_->Peek(peek_buf, sizeof(peek_buf));

        Header header;
        memcpy(&header, peek_buf, sizeof(Header));

        if (unlikely(header.version_ != MAGIC_VERSION))
        {
            ELOG << "Invalid packet version: " << (int)header.version_ << ", conn_id: " << conn_id_;
            recv_buffer_->Drop(sizeof(Header));
            break;
        }

        if (unlikely(header.pack_len_ <= 0 || header.pack_len_ > MAX_PACKET_SIZE))
        {
            ELOG << "Invalid packet length: " << header.pack_len_;
            recv_buffer_->Drop(sizeof(Header));
            break;
        }

        // 等待完整包
        if (recv_buffer_->Size() < header.pack_len_) break;

        // 分配 pack_len 字节的连续内存，与发包时布局完全对称：
        // [AppMsg 结构体][body 数据]
        // 不能用 make_shared<AppMsg>()，那只分配 sizeof(AppMsg)，body 会越界
        uint8_t *raw = new uint8_t[header.pack_len_];
        recv_buffer_->Pop(raw, header.pack_len_);

        auto *msg_base = reinterpret_cast<AppMsg *>(raw);
        // 修正 data_ 指针：指向紧跟在 AppMsg 结构体后面的 body 区域
        msg_base->data_ = reinterpret_cast<char *>(raw) + sizeof(AppMsg);
        msg_base->header_.conn_id_ = conn_id_;

        // 用自定义 deleter 管理原始内存（不能用默认 delete，必须 delete[]）
        auto msg_ptr = std::shared_ptr<AppMsg>(msg_base, [raw](AppMsg *) { delete[] raw; });
        recv_handler_(conn_id_, msg_ptr);
    }
}