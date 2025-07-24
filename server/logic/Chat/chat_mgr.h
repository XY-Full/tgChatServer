
#pragma once

#include "ILogic.h"
#include "ModuleManager.h"

#include "base.pb.h"
#include "player_chat_info.h"

using PlayerChatInfoPtr = std::shared_ptr<cs::PlayerChatInfo>;
using PlayerChatLocalInfoPtr = std::shared_ptr<PlayerChatLocalInfo>;

class ChatMgr : public ILogic
{
public:
    ChatMgr(Busd* busd);

    ~ChatMgr(void);

private:
    void registerEvent();
    void registerMessage();

    void playerReqChatInfo(const NetPack& pPack);
    void playerReqChatMessage(const NetPack& pPack);
    void playerSendChatMessage(const NetPack& pPack);

    MessagePtr NotifyAllPlayer(int64_t, const std::shared_ptr<cs::ChatMessage> &msg);
    void Message2DB(std::shared_ptr<cs::ChatMessage> ev);
    void onRecvChatMessage(int64_t playerid, std::shared_ptr<cs::ChatMessage> chatMessage);

    PlayerChatLocalInfoPtr getOrCreatePlayerChatInfo(int64_t playerid);
    PlayerChatLocalInfoPtr getPlayerChatInfo(int64_t playerid);

private:
    std::map<int32_t, std::map<int64_t, std::shared_ptr<cs::ChatMessage>>> messageMap_;
    std::map<int64_t, PlayerChatLocalInfoPtr> PlayerChatLocalInfo_;
    std::map<int64_t, std::shared_ptr<cs::PlayerChatInfo>> PlayerChatInfo_;
};
