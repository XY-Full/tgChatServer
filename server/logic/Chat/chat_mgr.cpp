#include "chat_mgr.h"
#include "ILogic.h"

#include "Helper.h"
#include "chat.pb.h"
#include "msg_id.pb.h"
#include "notify.pb.h"
#include <memory>

std::string msg_name[4] = {"cross_message", "local_message", "guild_message", "private_message"};

int32_t max_save_msg[4] = {10000, 10000, 10000, 10000};

std::shared_ptr<cs::ChatMessage> makeNotifyFromChatMessage(cs::PlayerSendMessage_Request *request, int64_t playerid)
{
    auto ev = std::make_shared<cs::ChatMessage>();
    ev->set_player_id(playerid);
    ev->set_msg(request->msg());
    ev->set_emoji_id(request->emojiid());
    ev->set_channel_id(request->channel());
    ev->set_timestamp(time(nullptr));
    ev->set_chat_id(request->chatid());
    ev->set_msg_id(Helper::GenUID());

    return ev;
}

ChatMgr::ChatMgr(Busd *busd) : ILogic(busd)
{
    registerMessage();
    registerEvent();
}

ChatMgr::~ChatMgr(void)
{
    ILOG;
}

void ChatMgr::registerMessage()
{
    busd_->registerHandler(MSGID::CS_PLAYER_SEND_MESSAGE,
                           std::bind(&ChatMgr::playerSendChatMessage, this, std::placeholders::_1));

    busd_->registerHandler(MSGID::CS_PLAYER_REQ_MESSAGE,
                           std::bind(&ChatMgr::playerReqChatMessage, this, std::placeholders::_1));

    busd_->registerHandler(MSGID::CS_PLAYER_REQ_CHAT_INFO,
                           std::bind(&ChatMgr::playerReqChatInfo, this, std::placeholders::_1));
}

void ChatMgr::registerEvent()
{
    busd_->registerEventHandle(&ChatMgr::NotifyAllPlayer, this);
}

void ChatMgr::playerSendChatMessage(const NetPack& pPack)
{
    PROCESS_NETPACK_BEGIN(cs::PlayerSendMessage);

    auto playerChatInfo = getOrCreatePlayerChatInfo(uid);

    auto chatMessage = makeNotifyFromChatMessage(request, uid);

    auto errcode = playerChatInfo->checkChat(*chatMessage);
    if (errcode != Error_success)
    {
        ELOG << "check chat err, uid: " << uid << ", errcode: " << errcode;
        response->set_err(errcode);
        break;
    }

    response->set_err(errcode);

    onRecvChatMessage(uid, chatMessage);

    PROCESS_NETPACK_END();
}

void ChatMgr::playerReqChatMessage(const NetPack& pPack)
{
    PROCESS_NETPACK_BEGIN(cs::PlayerReqMessage);
    (void)uid;

    int32_t channel = request->channel();
    int64_t msgid = request->msg_id();
    int32_t count = request->after_count();

    if (count <= 0 || count > 100)
    {
        ELOG << "count err, count: " << count;
        response->set_err(Error_unknown);
        break;
    }

    auto it = messageMap_.find(channel);
    if (it == messageMap_.end())
    {
        ELOG << "no this channel, channel: " << channel;
        response->set_err(Error_no_channel);
        break;
    }

    auto &cacheMap = it->second;

    // 如果msgid非零，则正常找消息列表中的msgid
    if (msgid != 0)
    {
        auto it2 = cacheMap.find(msgid);
        if (it2 == cacheMap.end())
        {
            ELOG << "no this msg, msgid: " << msgid;
            response->set_err(Error_no_msg);
            break;
        }

        if (!cacheMap.empty() && count > 0)
        {
            do
            {
                auto msg = it2->second;
                auto chatMessage = std::make_shared<cs::ChatMessage>();
                chatMessage->CopyFrom(*msg);
                response->add_msg()->CopyFrom(*chatMessage);
                --count;
                if (it2 == cacheMap.begin() || count <= 0)
                    break;
                --it2;
            }
            while (true); // 逆序遍历，为了能够遍历到第一条消息，用dowile
        }
    }
    else // 否则从最新的消息开始找
    {
        auto it2 = cacheMap.rbegin();
        while (it2 != cacheMap.rend() && count > 0)
        {
            auto msg = it2->second;
            auto chatMessage = std::make_shared<cs::ChatMessage>();
            chatMessage->CopyFrom(*msg);
            response->add_msg()->CopyFrom(*chatMessage);
            --count;
            ++it2;
        }
    }
    PROCESS_NETPACK_END();
}

void ChatMgr::onRecvChatMessage(int64_t playerid, std::shared_ptr<cs::ChatMessage> chatMessage)
{
    auto ev = std::make_shared<cs::ChatMessage>();
    ev->CopyFrom(*chatMessage);

    if (chatMessage->channel_id() == CHANNEL_CROSS)
    {
        Helper::sendTgMessage(std::to_string(chatMessage->chat_id()), chatMessage->msg());
    }
    else if (chatMessage->channel_id() == CHANNEL_LOCAL)
    {
        Helper::sendTgMessage(std::to_string(chatMessage->chat_id()), chatMessage->msg());
    }

    Message2DB(chatMessage);
}

MessagePtr ChatMgr::NotifyAllPlayer(int64_t playerid, const std::shared_ptr<cs::ChatMessage> &ev)
{
    messageMap_.at(ev->channel_id())[ev->msg_id()] = ev;
    // 进行数量控制
    if ((int32_t)(messageMap_.at(ev->channel_id()).size()) > max_save_msg[ev->channel_id()])
    {
        messageMap_.at(ev->channel_id()).erase(messageMap_.at(ev->channel_id()).begin()->first);
    }

    cs::Notify notify;
    notify.mutable_chat_message()->CopyFrom(*ev);
    busd_->broadcastTotalPlayer(MSGID::SC_NOTIFY, notify);

    return nullptr;
}

void ChatMgr::Message2DB(std::shared_ptr<cs::ChatMessage> ev)
{
    (void)ev;
}

PlayerChatLocalInfoPtr ChatMgr::getOrCreatePlayerChatInfo(int64_t playerid)
{
    auto PlayerChatInfo = getPlayerChatInfo(playerid);
    if (!PlayerChatInfo)
    {
        PlayerChatInfo = std::make_shared<PlayerChatLocalInfo>(playerid);
        PlayerChatLocalInfo_[playerid] = PlayerChatInfo;
        PlayerChatInfo_[playerid] = PlayerChatInfo->getChatInfo();
    }
    return PlayerChatInfo;
}

PlayerChatLocalInfoPtr ChatMgr::getPlayerChatInfo(int64_t playerid)
{
    if (PlayerChatLocalInfo_.find(playerid) != PlayerChatLocalInfo_.end())
        return PlayerChatLocalInfo_.at(playerid);
    return nullptr;
}
