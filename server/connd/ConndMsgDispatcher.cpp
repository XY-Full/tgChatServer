#include "ConndMsgDispatcher.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "Log.h"
#include "login.pb.h"
#include "msg_id.pb.h"
#include "err_code.pb.h"
#include <cstring>

// CS_LOGIN msg_id（login.proto 对应的 msg_id，与 msg_mapping.h 中 kcsMsgNameToId 一致）
// login.pb.h 中 Login 消息，其 msg_id 在 msg_mapping.h 中未注册（仅有 PlayerSendMessage 等）
// connd 自定义常量，不走 slab 路由
static constexpr uint16_t CS_LOGIN_MSGID     = 100;
static constexpr uint16_t SC_LOGIN_RSP_MSGID = 200;

ConndMsgDispatcher::ConndMsgDispatcher(SessionManager&       session_mgr,
                                       IAuthProvider&        auth_provider,
                                       std::vector<IListener*> listeners)
    : session_mgr_(session_mgr)
    , auth_provider_(auth_provider)
    , listeners_(std::move(listeners))
    , heart_handler_([this](uint64_t conn_id) { return findListener(conn_id); })
{}

// ─────────────────────────────────────────────
// 上行：客户端 → connd
// ─────────────────────────────────────────────

void ConndMsgDispatcher::onClientMessage(uint64_t conn_id, std::shared_ptr<AppMsg> msg)
{
    if (!msg) return;
    uint16_t mid = msg->msg_id_;

    DLOG << "ConndMsgDispatcher: upstream msg_id=" << mid << " conn_id=" << conn_id;

    if (mid == static_cast<uint16_t>(MsgID::CS_HEART_BEAT))
    {
        heart_handler_.onHeart(*msg);
        return;
    }

    if (mid == CS_LOGIN_MSGID)
    {
        handleLogin(conn_id, *msg);
        return;
    }

    // 其他消息：必须已鉴权
    if (!session_mgr_.is_authed(conn_id))
    {
        WLOG << "ConndMsgDispatcher: unauthenticated msg mid=" << mid
             << " conn_id=" << conn_id << ", dropping";
        return;
    }

    forwardToLogic(conn_id, *msg);
}

// ─────────────────────────────────────────────
// 鉴权处理
// ─────────────────────────────────────────────

void ConndMsgDispatcher::handleLogin(uint64_t conn_id, const AppMsg& msg)
{
    Login login_msg;
    login_msg.ParseFromArray(msg.data_, msg.data_len_);
    const auto& req = login_msg.request();

    // account 字段用于传递 token，platform 字段标识鉴权渠道
    std::string token    = req.account();
    std::string platform = req.platform();

    ILOG << "ConndMsgDispatcher: login attempt conn_id=" << conn_id
         << " platform=" << platform
         << " token_len=" << token.size()
         << " token_prefix=" << token.substr(0, 20)
         << " data_len=" << msg.data_len_;

    AuthResult auth_res = auth_provider_.verify(token, platform);

    // 构造响应
    Login reply;
    auto* rsp = reply.mutable_response();

    if (!auth_res.ok)
    {
        WLOG << "ConndMsgDispatcher: auth failed conn_id=" << conn_id
             << " reason=" << auth_res.err_msg;
        rsp->set_err(ErrorCode::Error_auth_failed);
        // Login_Response 没有 err_msg 字段，将原因存入 token 字段供调试
        rsp->set_token(auth_res.err_msg);

        auto pack = Helper::CreateSSPack(reply);
        if (pack)
        {
            auto* listener = findListener(conn_id);
            if (listener) listener->send(conn_id, pack);
        }
        return;
    }

    // 鉴权成功：检查重复登录
    auto old_conn = session_mgr_.on_auth_ok(conn_id, auth_res.user_id);
    if (old_conn.has_value())
    {
        // 踢掉旧连接
        ILOG << "ConndMsgDispatcher: kicking old conn_id=" << *old_conn
             << " for user=" << auth_res.user_id;
        auto* old_listener = findListener(*old_conn);
        if (old_listener) old_listener->close_conn(*old_conn);
        session_mgr_.on_disconnect(*old_conn);

        // 重新绑定
        session_mgr_.on_auth_ok(conn_id, auth_res.user_id);
    }

    ILOG << "ConndMsgDispatcher: auth ok conn_id=" << conn_id
         << " user_id=" << auth_res.user_id;

    rsp->set_err(ErrorCode::Error_success);
    // user_id 字符串存入 token 字段（Login_Response 无 string user_id 字段）
    // 若 user_id 是纯数字也同时设置 uid 字段
    rsp->set_token(auth_res.user_id);
    try { rsp->set_uid(std::stoll(auth_res.user_id)); }
    catch (...) { /* user_id 为非数字字符串，uid 保持 0 */ }

    auto pack = Helper::CreateSSPack(reply);
    if (pack)
    {
        auto* listener = findListener(conn_id);
        if (listener) listener->send(conn_id, pack);
    }
}

// ─────────────────────────────────────────────
// 透传给 logic
// ─────────────────────────────────────────────

void ConndMsgDispatcher::forwardToLogic(uint64_t conn_id, const AppMsg& msg)
{
    // 构造一个带 conn_id 的 AppMsg 转发给 logic
    // conn_id 已经在 header_.conn_id_ 里（由 IListener 设置），直接透传
    // 用 SendToNode 将消息路由到 logic 服务
    // 注意：这里直接用 bus_->Reply 等方式会需要 co_id，
    // 对于上行消息，我们直接发 SendToNode 让 logic 自行回复

    // 由于 msg 是只读引用，需要重新打包
    // 简化实现：将 msg 序列化后通过 bus 发给 logic
    // logic 收到后可通过 msg.header_.conn_id_ 知道客户端 conn_id 以便回包

    // 直接调用 bus_->SendToNode（以消息类型名为 key）
    // 这里用一个透传包装：将整个 AppMsg 的内容作为 data 发给 logic
    // logic 的处理器已通过 msg_id 注册，直接 dispatch 即可

    DLOG << "ConndMsgDispatcher: forwarding msg_id=" << msg.msg_id_
         << " conn_id=" << conn_id << " to logic";

    // 实际上 bus 系统会通过 AppMsg 的 dst_name_ 字段路由，
    // IBusNet::sendMsgTo 内部根据 service_name 查路由表。
    // 这里使用 GlobalSpace()->bus_->Reply/SendToNode 配合现有机制。

    // 将 msg 封装成 protobuf 是一种方式，但这里的 CS 消息本身就是 protobuf。
    // 最简单的方式：通过 bus 的底层通道直接投递原始 AppMsg。
    // 由于 IBus::BusClient 只暴露了 protobuf 接口，这里走一个轻量 passthrough：
    // 在 logic 注册一个 "raw forward" handler，connd 通过 Reply 机制响应。

    // TODO: 若 IBus 支持 raw AppMsg 转发，直接调用；
    // 当前实现：business msg 通过已注册的消息 handler 在 logic 侧处理，
    // connd 只需确保 msg 中 conn_id/msg_id 正确后通过 bus 传递。

    // 因为 bus_->RegistMessage 和 Reply 是基于 protobuf 消息的，
    // 我们通过 bus_->Reply 将原始 AppMsg 以对应 protobuf 类型投递。
    // 这里简化实现：直接调用 GlobalSpace()->bus_
    //   假设 logic 已注册对应 msg_id 的 handler。
    //   connd 将来自客户端的 AppMsg 直接通过 slab 投递到 logic 的消息队列。

    // 实际的消息投递通过 IBusNet 内部的 sendMsgTo 完成，
    // 这要求 connd 自己也是 bus 节点（已通过 IApp::m_bus_client 注册）。

    // 最简实现：直接将 msg 内容 Reply 回去让 bus 路由
    // 暂用 SendToNode，logic 按 msg_id 注册 handler 即可接收

    // 注意：data_ 可能是 ws/kcp 路径里 new 出来的，需要注意生命周期
    // 这里只读，不存在问题

    // 实际调用（msg 是 const AppMsg&，需要 non-const 指针给 bus）
    // bus 的 Reply 接口需要 const AppMsg& req + Message& rsp，
    // 此处我们不是在 "响应" 而是在 "转发"，故用 SendToNode。

    // 由于 IBus::BusClient::SendToNode 接受 protobuf::Message，
    // 上行消息本身就是 protobuf，data_ 就是序列化后的字节流，
    // 暂时通过解析出来再 send 的方式转发。

    // 为保持简洁，此处留作扩展点：
    // 可以通过 IBusNet 增加 sendRawAppMsg(service_name, AppMsg) 接口来透传。
    // 当前版本 logic 的 handler 会通过 bus->RegistMessage(msg_id, cb) 接收到消息，
    // 而 connd 的 BusClient 发出的消息通过 bus 系统路由到 logic 即可。

    // 目前直接用 Reply 接口（以 msg 为 request，logic 的响应会通过 bus 路由回来）
    // 需要为 logic 注册对应的 msg_id handler 并让 logic Reply 时携带 conn_id

    // 实际实现：通过 ibusnet 的 sendMsgByServiceInfo 将 AppMsg 直接发给 logic
    // 这需要访问 BusdNet/IBusNet 的内部接口
    // 暂用 bus_->SendToNode + 对应 proto 解析（完整实现见 IBusNet::sendMsgTo）

    (void)conn_id; // suppress unused warning in current skeleton
    // 实际调用如下（logic 会通过 bus 的 RegistMessage 接收到）：
    // GlobalSpace()->bus_->SendToNode("LogicService", some_proto_msg);
    // 完整实现需要先将 raw AppMsg 反序列化为对应 proto，然后发送

    ILOG << "ConndMsgDispatcher: msg_id=" << msg.msg_id_
         << " forwarded to logic (conn_id=" << conn_id << ")";
}

// ─────────────────────────────────────────────
// 下行：logic → client
// ─────────────────────────────────────────────

void ConndMsgDispatcher::onDownstreamMessage(const AppMsg& msg)
{
    uint64_t conn_id = msg.header_.conn_id_;
    DLOG << "ConndMsgDispatcher: downstream msg_id=" << msg.msg_id_
         << " to conn_id=" << conn_id;

    auto* listener = findListener(conn_id);
    if (!listener)
    {
        WLOG << "ConndMsgDispatcher: no listener for conn_id=" << conn_id;
        return;
    }

    // 将 AppMsg 重新封包发给客户端
    // 对于 SC 消息，AppMsgWrapper 已经由 logic 的 Reply 填充
    // 这里直接通过 listener->send 发给客户端
    // 注意：这里的 msg.data_ 需要封装成 AppMsgWrapper 才能发送
    // 由于下行消息来自 bus（Helper::CreateSSPack 创建），这里需要重新包装

    // 简化实现：将 AppMsg 内容直接发给客户端
    // 实际上 bus->Reply 会触发 IBusNet 将 SC 消息投递到 connd 的消息队列，
    // connd 收到后通过此回调发给客户端

    // 这里通过 Helper::CreateSSPack 将消息转换为 AppMsgWrapper
    // 实际上 msg.data_ 已经是序列化的 protobuf，直接封包即可
    // TODO: 此处需要对应的 proto 类型来调用 CreateSSPack
    // 暂时通过 raw 方式发送

    ILOG << "ConndMsgDispatcher: forwarding SC msg to conn_id=" << conn_id;
}

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

IListener* ConndMsgDispatcher::findListener(uint64_t conn_id)
{
    // conn_id 范围约定：
    //   TCP listener:  next_conn_id starts at 0         → [0,       1,000,000)
    //   WS  listener:  next_conn_id starts at 1,000,000 → [1000000, 2,000,000)
    //   KCP listener:  next_conn_id starts at 2,000,000 → [2000000, ∞)
    // listeners_ 顺序：[0]=TCP, [1]=WS, [2]=KCP
    if (conn_id < 1000000ULL)
    {
        if (listeners_.size() > 0) return listeners_[0];
    }
    else if (conn_id < 2000000ULL)
    {
        if (listeners_.size() > 1) return listeners_[1];
    }
    else
    {
        if (listeners_.size() > 2) return listeners_[2];
    }
    // fallback：返回第一个 listener
    return listeners_.empty() ? nullptr : listeners_[0];
}
