#pragma once
#include "IListener.h"
#include "AppMsg.h"
#include "PackBase.h"
#include "Log.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <openssl/sha.h>
#include <string>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

/**
 * @brief WebSocket 监听器
 *
 * 实现 RFC 6455 WebSocket 协议：
 *   1. TCP accept
 *   2. HTTP Upgrade 握手（101 Switching Protocols）
 *   3. WebSocket 帧解析（binary frame only）
 *   4. 帧 payload 按 AppMsg 二进制协议解析后触发 recv_handler
 *
 * 帧格式（收到客户端的 masked frame）：
 *   [FIN|RSV|opcode 1B][MASK|payload_len 1B][ext_len 0/2/8B][mask 4B][data]
 *
 * 发送给客户端的帧为 unmasked binary frame。
 */
class WsListener : public IListener
{
public:
    WsListener(int32_t port,
               RecvHandler  recv_handler,
               CloseHandler close_handler = nullptr,
               ConnHandler  conn_handler  = nullptr)
        : port_(port)
    {
        recv_handler_  = std::move(recv_handler);
        close_handler_ = std::move(close_handler);
        conn_handler_  = std::move(conn_handler);
    }

    ~WsListener() override { stop(); }

    bool start() override;
    void stop() override;
    int32_t send(uint64_t conn_id, std::shared_ptr<AppMsgWrapper> pack) override;
    void close_conn(uint64_t conn_id) override;
    std::string proto_name() const override { return "ws"; }

private:
    // 每个 WS 连接的状态
    struct WsConn
    {
        int      fd        = -1;
        uint64_t conn_id   = 0;
        bool     handshaked = false;
        std::vector<uint8_t> recv_buf;  // 原始 TCP 数据缓冲
        std::chrono::steady_clock::time_point last_active;
    };

    void acceptLoop();
    void handleEvent(int fd, uint32_t events);
    bool doHandshake(WsConn& conn);
    void readData(WsConn& conn);
    bool parseFrames(WsConn& conn);
    void removeConn(int fd);

    // 构建 WS binary frame（unmasked，发往客户端）
    static std::vector<uint8_t> buildFrame(const uint8_t* data, size_t len);
    // base64 encode for Sec-WebSocket-Accept
    static std::string base64Encode(const uint8_t* data, size_t len);

    int32_t                  port_;
    int                      server_fd_ = -1;
    int                      epoll_fd_  = -1;
    std::atomic<bool>        running_{false};
    std::thread              io_thread_;
    std::atomic<uint64_t>    next_conn_id_{1000000};  // WS conn_id 从 100w 开始，避免与 TCP 重叠
    std::mutex               conn_mutex_;
    std::unordered_map<int, std::shared_ptr<WsConn>>       fd_to_conn_;
    std::unordered_map<uint64_t, std::shared_ptr<WsConn>>  id_to_conn_;

    static constexpr int MAX_EVENTS = 128;
    static constexpr int HEARTBEAT_TIMEOUT_SEC = 60;
    static constexpr const char* WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
};
