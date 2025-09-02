#include "TcpClient.h"
#include "GlobalSpace.h"
#include "Log.h"
#include "AppMsg.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define close closesocket
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

TcpClient::TcpClient(const std::string &ip, int port, RecvHandler recv_handler)
    : ip_(ip), port_(port), recv_handler_(recv_handler)
{
#ifdef _WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif
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

    recv_thread_ = std::thread(&TcpClient::recvLoop, this);

    GlobalSpace()->timer_->runEvery(1.0f, std::bind(&TcpClient::checkHeartbeat, this));
}

void TcpClient::stop()
{
    running_ = false;

    if (recv_thread_.joinable())
        recv_thread_.join();

    std::lock_guard<std::mutex> lock(conn_mutex_);
    if (sock_ != -1)
    {
        close(sock_);
        sock_ = -1;
    }
}

void TcpClient::connectToServer()
{
    sock_ = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_ < 0)
    {
        ELOG << "Failed to create socket.";
        return;
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

    socket_ = std::make_unique<SocketWrapper>(sock_);
    last_active_time_ = std::chrono::steady_clock::now();

    ILOG << "Connected to server " << ip_ << ":" << port_;
}

void TcpClient::recvLoop()
{
    if(!recv_handler_)
    {
        ELOG << "RecvHandler is not set";
        return;
    }

    while (running_)
    {
        std::string pack;
        // 读取消息仅做peek
        if (!socket_->recvAll(pack, sizeof(Header), true))
        {
            ELOG << "Receive length failed. Reconnecting...";
            stop();
            start();
            return;
        }

        auto header = *reinterpret_cast<Header*>(pack.data());
        if (header.pack_len_ <= 0)// || header.pack_len_ > 10 * 1024 * 1024)
        {
            ELOG << "Invalid message length: " << header.pack_len_;
            continue;
        }

        std::string msg;
        if (!socket_->recvAll(msg, header.pack_len_))
        {
            ELOG << "Receive message body failed. Reconnecting...";
            stop();
            start();
            return;
        }

        auto msg_base = std::shared_ptr<PackBase>((PackBase*)(msg.data()));
        last_active_time_ = std::chrono::steady_clock::now();

        recv_handler_(msg_base);
    }
}

void TcpClient::send(char* data, uint32_t len)
{
    if (!running_)
    {
        ELOG << "Cannot send, client not running";
        return;
    }

    socket_->sendAll(std::string(data, len));
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
