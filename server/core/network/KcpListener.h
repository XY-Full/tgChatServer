#pragma once
#include "IListener.h"
#include "AppMsg.h"
#include "Log.h"
#include <arpa/inet.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <vector>

// KCP 是 C 库，需要 extern "C" 包裹
extern "C" {
#include "../../../third/kcp/ikcp.h"
}

/**
 * @brief KCP 监听器（UDP + KCP 可靠传输）
 *
 * 架构：
 *   - 单个 UDP socket 接收所有客户端数据
 *   - 每个客户端以 (ip:port) 为 key，维护独立的 ikcpcb
 *   - Timer 线程周期调用 ikcp_update（推荐 10ms）
 *   - 收到完整消息后按 AppMsg 二进制协议解析，触发 recv_handler
 *
 * 握手约定（轻量）：
 *   第一个 KCP 数据包的首 4 字节为客户端自选的 conv（会话 ID）。
 *   conv 由客户端生成，server 端用 conv 标识 session。
 *   conv 与 conn_id 的映射在 KcpListener 内部维护。
 *
 * 注意：KCP 本身不加密，生产环境建议上层加密。
 */
class KcpListener : public IListener
{
public:
    KcpListener(int32_t port,
                RecvHandler  recv_handler,
                CloseHandler close_handler = nullptr,
                ConnHandler  conn_handler  = nullptr)
        : port_(port)
    {
        recv_handler_  = std::move(recv_handler);
        close_handler_ = std::move(close_handler);
        conn_handler_  = std::move(conn_handler);
    }

    ~KcpListener() override { stop(); }

    bool start() override;
    void stop() override;
    int32_t send(uint64_t conn_id, std::shared_ptr<AppMsgWrapper> pack) override;
    void close_conn(uint64_t conn_id) override;
    std::string proto_name() const override { return "kcp"; }

private:
    struct KcpSession
    {
        ikcpcb*             kcp     = nullptr;
        uint64_t            conn_id = 0;
        sockaddr_in         addr{};
        std::vector<uint8_t> recv_buf;   // KCP 层收到的完整数据
        std::chrono::steady_clock::time_point last_active;
    };

    void ioLoop();
    void updateLoop();
    void processUdpPacket(const uint8_t* data, size_t len, const sockaddr_in& from);
    void dispatchKcpData(KcpSession& session);
    void removeSession(uint32_t conv);

    static int kcpOutput(const char* buf, int len, ikcpcb* kcp, void* user);
    static std::string addrKey(const sockaddr_in& addr);

    int32_t               port_;
    int                   udp_fd_ = -1;
    std::atomic<bool>     running_{false};
    std::thread           io_thread_;
    std::thread           update_thread_;
    std::atomic<uint64_t> next_conn_id_{2000000};  // KCP conn_id 从 200w 开始
    std::mutex            mu_;

    // conv → session
    std::unordered_map<uint32_t, std::shared_ptr<KcpSession>>     conv_to_sess_;
    // addr_key → conv（用于首包建立 session）
    std::unordered_map<std::string, uint32_t>                      addr_to_conv_;
    // conn_id → conv（用于 send/close）
    std::unordered_map<uint64_t, uint32_t>                         id_to_conv_;

    static constexpr int KCP_UPDATE_INTERVAL_MS = 10;
    static constexpr int IDLE_TIMEOUT_SEC       = 60;
};
