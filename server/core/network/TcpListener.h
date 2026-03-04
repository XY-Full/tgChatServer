#pragma once
#include "IListener.h"
#include "TcpServer.h"
#include <memory>
#include <string>

/**
 * @brief TCP 协议监听器
 *
 * 对 TcpServer 做薄封装，将其适配到 IListener 接口。
 * conn_id 直接复用 TcpServer 内部的 next_conn_id_。
 */
class TcpListener : public IListener
{
public:
    /**
     * @param port          监听端口
     * @param shm_name      共享内存 ring-buffer 名称（供 TcpServer 使用）
     * @param recv_handler  收到完整 AppMsg 后的回调
     * @param close_handler 连接关闭后的回调
     * @param conn_handler  新连接建立后的回调
     */
    TcpListener(int32_t port,
                std::string shm_name,
                RecvHandler  recv_handler,
                CloseHandler close_handler = nullptr,
                ConnHandler  conn_handler  = nullptr)
        : port_(port)
    {
        recv_handler_  = std::move(recv_handler);
        close_handler_ = std::move(close_handler);
        conn_handler_  = std::move(conn_handler);

        server_ = std::make_unique<TcpServer>(
            port_,
            recv_handler_,
            shm_name,
            close_handler_,
            conn_handler_);
    }

    bool start() override
    {
        server_->start();
        return true;
    }

    void stop() override
    {
        server_->stop();
    }

    int32_t send(uint64_t conn_id, std::shared_ptr<AppMsgWrapper> pack) override
    {
        return server_->send(static_cast<int32_t>(conn_id), pack);
    }

    void close_conn(uint64_t conn_id) override
    {
        // TcpServer 没有直接暴露 close，向对端发送 FIN 通过 removeConnection 触发。
        // 此处通过发送空包触发 close 逻辑，或通过 server_ 友元访问（暂用 NOOP）。
        // TODO: 若需要主动踢人，在 TcpServer 添加 closeConn(conn_id) 接口
        (void)conn_id;
    }

    std::string proto_name() const override { return "tcp"; }

private:
    int32_t                    port_;
    std::unique_ptr<TcpServer> server_;
};
