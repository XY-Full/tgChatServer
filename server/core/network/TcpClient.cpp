#include "TcpClient.h"
#include "EventLoopWrapper.h"
#include "GlobalSpace.h"
#include "Log.h"
#include "TcpConnection.h"
#include "Timer.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define close closesocket
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#endif

TcpClient::TcpClient(const std::string &ip, int port, std::string shm_name, RecvHandler recv_handler)
    : ip_(ip), port_(port), recv_handler_(recv_handler), shm_name_(shm_name)
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    tcp_recv_buffer_ = std::make_unique<ShmRingBuffer<uint8_t>>(shm_name_ + "_tcp_recv");
    (void)tcp_recv_buffer_; // Connection 独立创建自己的 buffer，此处预留为备用
}

TcpClient::~TcpClient()
{
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
    // tcp_recv_buffer_ 由 unique_ptr 管理，自动释放
}

bool TcpClient::start()
{
    ILOG << "Starting TcpClient to connect to " << ip_ << ":" << port_ << "...";

    running_ = true;
    bool result = connectToServer();
    if (!result)
        return false;

    event_thread_ = std::thread(&TcpClient::eventLoop, this);

    GlobalSpace()->timer_->runEvery(1.0f, std::bind(&TcpClient::checkHeartbeat, this));

    return result;
}

void TcpClient::stop()
{
    ELOG << "Stopping TcpClient...";
    running_ = false;

    // 若 stop() 是从 event_thread_ 自身调用（如 RDHUP 回调），
    // 不能 join 自己，否则死锁；让循环自然退出即可。
    if (event_thread_.joinable() &&
        event_thread_.get_id() != std::this_thread::get_id())
    {
        event_thread_.join();
    }

    if (conn_)
        conn_->close();
}

bool TcpClient::connectToServer()
{
    sock_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (sock_ < 0)
    {
        ELOG << "Failed to create socket.";
        return false;
    }

    int opt = 1;
    // 设置TCP_NODELAY减少小数据包的延迟
    if (setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1)
    {
        ELOG << "Failed to set TCP_NODELAY: " << strerror(errno);
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

    if (connect(sock_, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        if (errno == EINPROGRESS)
        {
            // 非阻塞 connect 正在进行，等待 WRITE 事件确认连接完成
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(sock_, &wfds);
            struct timeval tv{5, 0}; // 5 秒超时
            int ret = select(sock_ + 1, nullptr, &wfds, nullptr, &tv);
            if (ret <= 0)
            {
                ELOG << "Connect to " << ip_ << ":" << port_ << (ret == 0 ? " timed out" : " select error");
                close(sock_);
                sock_ = -1;
                return false;
            }
            // 确认连接是否真正成功
            int err = 0;
            socklen_t len = sizeof(err);
            getsockopt(sock_, SOL_SOCKET, SO_ERROR, &err, &len);
            if (err != 0)
            {
                ELOG << "Connect failed: " << strerror(err);
                close(sock_);
                sock_ = -1;
                return false;
            }
        }
        else
        {
            ELOG << "Failed to connect to server " << ip_ << ":" << port_;
            close(sock_);
            sock_ = -1;
            return false;
        }
    }

    // 注册服务器socket到epoll
    if (!epoller_.add(sock_, EventType::READ | EventType::EDGE_TRIGGER,
                      [this](int fd, EventType events) { this->eventHandler(fd, events); }))
    {
        ELOG << "Failed to add server fd to epoller";
        close(sock_);
        exit(1);
    }

    // TcpClient 每次重连会重建 Connection，但 tcp_recv_buffer_ 只创建一次可复用
    // 将所有权临时转移给 Connection，conn_ 析构时归还（用 aliasing shared_ptr 保持 buffer 存活）
    // 简单做法：传裸指针给 Connection，生命周期由 TcpClient 保证（conn_ 析构前 TcpClient 不会销毁 buffer）
    conn_ = std::make_shared<Connection>(
        sock_, -1, epoller_, recv_handler_, [this](int64_t) { stop(); },
        std::make_unique<ShmRingBuffer<uint8_t>>(shm_name_ + "_tcp_recv_conn"));

    conn_->updateActiveTime(); // 初始化心跳时间

    ILOG << "Connected to server " << ip_ << ":" << port_;
    return true;
}

void TcpClient::eventLoop()
{
    while (running_)
    {
        int nfds = epoller_.wait(1000);
        if (nfds > 0)
        {
            epoller_.processEvents();
        }
    }
}

void TcpClient::eventHandler(int fd, EventType events)
{
    if ((events & EventType::RDHUP) != EventType::NONE)
    {
        // 对端关闭连接
        ILOG << "Connection closed by center server";
        stop();
    }
    else
    {
        if ((events & EventType::READ) != EventType::NONE)
        {
            conn_->onReadable();
        }
        if ((events & EventType::WRITE) != EventType::NONE)
        {
            conn_->onWritable();
        }
    }
}

void TcpClient::send(std::shared_ptr<AppMsgWrapper> data)
{
    if (!running_)
    {
        ELOG << "Cannot send, client not running";
        return;
    }

    conn_->send(data);
}

void TcpClient::checkHeartbeat()
{
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - conn_->lastActiveTime()).count() > HEARTBEAT_TIMEOUT_SECONDS)
    {
        ELOG << "Heartbeat timeout. Reconnecting...";
        stop();
        start();
    }
}
