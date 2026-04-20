#include "ConndMsgDispatcher.h"
#include "GlobalSpace.h"
#include "bus/IBus.h"
#include "Helper.h"
#include "Log.h"
#include "login.pb.h"
#include "msg_id.pb.h"
#include "err_code.pb.h"
#include <cstring>

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

    // 心跳：本地处理
    if (mid == static_cast<uint16_t>(MsgID::CS_HEART_BEAT))
    {
        heart_handler_.onHeart(*msg);
        return;
    }

    // 申请 Token：无需鉴权，透传给 account 服务签发 JWT
    if (mid == static_cast<uint16_t>(MsgID::CS_PLAYER_APPLY_TOKEN))
    {
        forwardToAccount(conn_id, *msg);
        return;
    }

    // 验证 Token：本地 JWT 验签
    if (mid == static_cast<uint16_t>(MsgID::CS_PLAYER_AUTH))
    {
        handleLogin(conn_id, *msg);
        return;
    }

    // 其他业务消息：必须已鉴权
    if (!session_mgr_.is_authed(conn_id))
    {
        WLOG << "ConndMsgDispatcher: unauthenticated msg mid=" << mid
             << " conn_id=" << conn_id << ", dropping";
        return;
    }

    forwardToLogic(conn_id, *msg);
}

// ─────────────────────────────────────────────
// 鉴权处理（CS_PLAYER_AUTH，本地验签）
// ─────────────────────────────────────────────

void ConndMsgDispatcher::handleLogin(uint64_t conn_id, const AppMsg& msg)
{
    cs::PlayerAuth login_msg;
    login_msg.ParseFromArray(msg.data_, msg.data_len_);
    const auto& req = login_msg.request();

    std::string token = req.token();

    ILOG << "ConndMsgDispatcher: login attempt conn_id=" << conn_id
         << " token_len=" << token.size();

    AuthResult auth_res = auth_provider_.verify(token);

    // 构造响应
    cs::PlayerAuth reply;
    auto* rsp = reply.mutable_response();

    if (!auth_res.ok)
    {
        WLOG << "ConndMsgDispatcher: auth failed conn_id=" << conn_id
             << " reason=" << auth_res.err_msg;
        rsp->set_err(ErrorCode::Error_auth_failed);

        auto pack = Helper::CreateCSPackage(reply);
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

    auto pack = Helper::CreateCSPackage(reply);
    if (pack)
    {
        auto* listener = findListener(conn_id);
        if (listener) listener->send(conn_id, pack);
    }
}

// ─────────────────────────────────────────────
// 转发到 logic（已鉴权业务消息）
// ─────────────────────────────────────────────

void ConndMsgDispatcher::forwardToLogic(uint64_t conn_id, const AppMsg& msg)
{
    DLOG << "ConndMsgDispatcher: forwardToLogic msg_id=" << msg.msg_id_
         << " conn_id=" << conn_id;

    if (!GlobalSpace()->bus_->ForwardRawAppMsg("LogicService", msg))
    {
        WLOG << "ConndMsgDispatcher: forwardToLogic failed for msg_id=" << msg.msg_id_
             << " conn_id=" << conn_id;
    }
}

// ─────────────────────────────────────────────
// 转发到 account（申请 token，无需鉴权）
// ─────────────────────────────────────────────

void ConndMsgDispatcher::forwardToAccount(uint64_t conn_id, const AppMsg& msg)
{
    DLOG << "ConndMsgDispatcher: forwardToAccount conn_id=" << conn_id;

    if (!GlobalSpace()->bus_->ForwardRawAppMsg("AccountService", msg))
    {
        WLOG << "ConndMsgDispatcher: forwardToAccount failed conn_id=" << conn_id;
    }
}

// ─────────────────────────────────────────────
// 下行：logic / account → connd → client
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

    // 将下行 AppMsg 直接复制到 slab 并封装为 AppMsgWrapper，
    // 通过 IListener::send 发给客户端。
    // data_ 已是序列化好的 proto bytes，不需要再次序列化。
    auto pack = Helper::ForwardRawAppMsg(msg, "");
    if (!pack)
    {
        ELOG << "ConndMsgDispatcher: ForwardRawAppMsg failed for conn_id=" << conn_id;
        return;
    }

    listener->send(conn_id, pack);
    ILOG << "ConndMsgDispatcher: pushed msg_id=" << msg.msg_id_
         << " to conn_id=" << conn_id;
}

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

IListener* ConndMsgDispatcher::findListener(uint64_t conn_id)
{
    // conn_id 范围约定：
    //   TCP listener:  [0,       1,000,000)
    //   WS  listener:  [1000000, 2,000,000)
    //   KCP listener:  [2000000, ∞)
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
    return listeners_.empty() ? nullptr : listeners_[0];
}
