#pragma once
#include "SessionManager.h"
#include "auth/IAuthProvider.h"
#include "handler/ConndHeartHandler.h"
#include "network/IListener.h"
#include "network/AppMsg.h"
#include <memory>
#include <unordered_map>
#include <vector>

/**
 * @brief connd 上行消息分发器
 *
 * 负责将客户端发来的消息按类型路由：
 *   CS_HEART_BEAT → ConndHeartHandler（本地处理）
 *   CS_LOGIN      → 调用 IAuthProvider，完成鉴权和 session 绑定
 *   其他          → 检查 authed 后透传给 logic（通过 bus）
 *
 * 下行（logic → client）通过 GlobalSpace()->bus_->RegistMessage 注册，
 * 收到后用 IListener::send 推给客户端。
 */
class ConndMsgDispatcher
{
public:
    /**
     * @param session_mgr   会话管理器引用
     * @param auth_provider 鉴权提供者
     * @param listeners     所有已启动的 IListener（用于向客户端发包）
     */
    ConndMsgDispatcher(SessionManager&                    session_mgr,
                       IAuthProvider&                     auth_provider,
                       std::vector<IListener*>            listeners);

    /**
     * @brief 处理来自客户端的上行消息（由 IListener 回调触发）
     */
    void onClientMessage(uint64_t conn_id, std::shared_ptr<AppMsg> msg);

    /**
     * @brief 处理来自 logic 的下行消息（bus 注册回调触发）
     * 消息的 header_.conn_id_ 字段标识目标客户端。
     */
    void onDownstreamMessage(const AppMsg& msg);

private:
    void handleLogin(uint64_t conn_id, const AppMsg& msg);
    void forwardToLogic(uint64_t conn_id, const AppMsg& msg);
    void forwardToAccount(uint64_t conn_id, const AppMsg& msg);

    // 找到能处理此 conn_id 的 listener（通过 conn_id 范围区分协议）
    IListener* findListener(uint64_t conn_id);

    // 向指定 conn_id 发送错误回包
    void sendErrorToClient(uint64_t conn_id, int32_t err_code, const std::string& msg_type);

    SessionManager&         session_mgr_;
    IAuthProvider&          auth_provider_;
    std::vector<IListener*> listeners_;
    ConndHeartHandler       heart_handler_;
};
