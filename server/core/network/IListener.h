#pragma once
#include "AppMsg.h"
#include "MsgWrapper.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

/**
 * @brief 监听器抽象接口
 *
 * 统一 TCP / WebSocket / KCP 三种传输协议的监听行为。
 * 所有协议在收到数据后均将其解析为 AppMsg，上层无需关心底层协议细节。
 *
 * 回调约定：
 *   - recv_handler  : 收到完整的 AppMsg（已完成协议解帧）
 *   - close_handler : 连接关闭，conn_id 对应的 session 需要清理
 *   - conn_handler  : 新连接建立，conn_id 已分配
 */
class IListener
{
public:
    using RecvHandler  = std::function<void(uint64_t conn_id, std::shared_ptr<AppMsg> msg)>;
    using CloseHandler = std::function<void(uint64_t conn_id)>;
    using ConnHandler  = std::function<void(uint64_t conn_id)>;

    virtual ~IListener() = default;

    /**
     * @brief 启动监听，开始接受连接
     * @return true 成功，false 失败（端口占用等）
     */
    virtual bool start() = 0;

    /**
     * @brief 停止监听并关闭所有连接
     */
    virtual void stop() = 0;

    /**
     * @brief 向指定连接发送数据包
     * @param conn_id  目标连接 ID
     * @param pack     待发送的消息包（已封装为 AppMsgWrapper）
     * @return         0 成功，负数 失败
     */
    virtual int32_t send(uint64_t conn_id, std::shared_ptr<AppMsgWrapper> pack) = 0;

    /**
     * @brief 主动关闭指定连接
     * @param conn_id 目标连接 ID
     */
    virtual void close_conn(uint64_t conn_id) = 0;

    /**
     * @brief 返回协议名称，用于日志/session 标记
     */
    virtual std::string proto_name() const = 0;

protected:
    RecvHandler  recv_handler_;
    CloseHandler close_handler_;
    ConnHandler  conn_handler_;
};
