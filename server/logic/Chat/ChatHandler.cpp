#include "ChatHandler.h"
#include "GlobalSpace.h"
#include "chat.pb.h"

#include <ctime>

ChatHandler::ChatHandler()
{
    registerHandlers();
}

void ChatHandler::registerHandlers()
{
    GlobalSpace()->bus_->RegistMessage(MsgID::CS_PLAYER_SEND_MESSAGE,
                                       std::bind(&ChatHandler::onSendMessage, this, std::placeholders::_1));
    GlobalSpace()->bus_->RegistMessage(MsgID::CS_PLAYER_REQ_MESSAGE,
                                       std::bind(&ChatHandler::onReqMessage, this, std::placeholders::_1));
    GlobalSpace()->bus_->RegistMessage(MsgID::CS_PLAYER_REQ_CHAT_INFO,
                                       std::bind(&ChatHandler::onReqChatInfo, this, std::placeholders::_1));
}

// ─────────────────────────────────────────────
// CS_PLAYER_SEND_MESSAGE：用户发消息
// ─────────────────────────────────────────────
void ChatHandler::onSendMessage(const AppMsg &msg)
{
    PROCESS_NETPACK_BEGIN(cs::PlayerSendMessage);

    const std::string &content = request->msg();
    if (content.empty())
    {
        response->set_err(ErrorCode::Error_wrong_msg);
        break;
    }
    if (content.size() > kMaxChatMsgLen)
    {
        response->set_err(ErrorCode::Error_msg_too_long);
        break;
    }
    if (request->channel() < kMinChannelId || request->channel() > kMaxChannelId)
    {
        response->set_err(ErrorCode::Error_no_channel);
        break;
    }

    // TODO(player): 禁言(Error_muted)与发言CD(Error_chat_in_cd)检查，依赖玩家数据加载
    // TODO(dbproxy): 通过 SS_DB_EXEC 持久化到 chat_messages 表
    // TODO(notify): 组装 cs::Notify(SC_NOTIFY) 经 connd 广播给频道内在线玩家

    cs::ChatMessage chat_msg;
    chat_msg.set_player_id(uid);
    chat_msg.set_msg(content);
    chat_msg.set_emoji_id(request->emojiid());
    chat_msg.set_channel_id(request->channel());
    chat_msg.set_chat_id(request->chatid());
    chat_msg.set_timestamp(time(nullptr));
    chat_msg.set_msg_id(nextMsgId());

    ILOG << "ChatHandler: msg accepted, uid=" << uid << " channel=" << request->channel()
         << " msg_id=" << chat_msg.msg_id();

    PROCESS_NETPACK_END();
}

// ─────────────────────────────────────────────
// CS_PLAYER_REQ_MESSAGE：拉取历史消息
// ─────────────────────────────────────────────
void ChatHandler::onReqMessage(const AppMsg &msg)
{
    PROCESS_NETPACK_BEGIN(cs::PlayerReqMessage);

    if (request->channel() < kMinChannelId || request->channel() > kMaxChannelId)
    {
        response->set_err(ErrorCode::Error_no_channel);
        break;
    }
    if (request->after_count() <= 0)
    {
        response->set_err(ErrorCode::Error_argument);
        break;
    }

    // TODO(dbproxy): 按 channel + msg_id 游标从 chat_messages 分页查询，
    //                填充 response->add_msg()。当前骨架返回空列表。

    PROCESS_NETPACK_END();
}

// ─────────────────────────────────────────────
// CS_PLAYER_REQ_CHAT_INFO：查询聊天状态
// ─────────────────────────────────────────────
void ChatHandler::onReqChatInfo(const AppMsg &msg)
{
    PROCESS_NETPACK_BEGIN(cs::PlayerReqChatInfo);

    // TODO(player): 从玩家数据填充禁言截止时间、聊天CD、拉黑列表。
    //               当前骨架返回默认状态（未禁言、无CD、无拉黑）。
    auto *info = response->mutable_info();
    info->set_player_id(uid);
    info->set_mute_end_time(0);

    PROCESS_NETPACK_END();
}
