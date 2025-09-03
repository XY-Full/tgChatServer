#include "TcpClient.h"
#include "AppMsg.h"
#include "EventLoopWrapper.h"
#include "GlobalSpace.h"
#include "Log.h"
#include "PackBase.h"
#include "TcpConnection.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define close closesocket
#else
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

TcpClient::TcpClient(const std::string &ip, int port, std::string shm_name, RecvHandler recv_handler)
    : ip_(ip), port_(port), recv_handler_(recv_handler), shm_name_(shm_name)
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    conn_ = std::make_shared<Connection>(sock_, -1, shm_name_, epoller_, recv_handler_, [this](int64_t) { stop(); });
}

TcpClient::~TcpClient()
{
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

void TcpClient::start()
{
    running_ = true;
    connectToServer();

    GlobalSpace()->timer_->runEvery(1.0f, std::bind(&TcpClient::checkHeartbeat, this));
}

void TcpClient::stop()
{
    running_ = false;

    conn_->close();
}

void TcpClient::connectToServer()
{
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0)
    {
        ELOG << "Failed to create socket.";
        return;
    }

    int opt = 1;
    // 设置TCP_NODELAY减少小数据包的延迟
    if (setsockopt(sock_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt)) == -1)
    {
        ELOG << "Failed to set TCP_NODELAY: " << strerror(errno);
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    inet_pton(AF_INET, ip_.c_str(), &addr.sin_addr);

    if (connect(sock_, (sockaddr *)&addr, sizeof(addr)) != 0)
    {
        ELOG << "Failed to connect to server " << ip_ << ":" << port_;
        close(sock_);
        sock_ = -1;
        return;
    }

    // 注册服务器socket到epoll
    if (!epoller_.add(sock_, EventType::READ | EventType::EDGE_TRIGGER,
                      [this](int fd, EventType events) { this->eventHandler(fd, events); }))
    {
        ELOG << "Failed to add server fd to epoller";
        close(sock_);
        exit(1);
    }
    last_active_time_ = std::chrono::steady_clock::now();

    ILOG << "Connected to server " << ip_ << ":" << port_;
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

void TcpClient::send(char *data, uint32_t len)
{
    if (!running_)
    {
        ELOG << "Cannot send, client not running";
        return;
    }

    conn_->send((PackBase *)data);
}

void TcpClient::checkHeartbeat()
{
    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_active_time_).count() > HEARTBEAT_TIMEOUT_SECONDS)
    {
        ELOG << "Heartbeat timeout. Reconnecting...";
        stop();
        start();
    }
}
