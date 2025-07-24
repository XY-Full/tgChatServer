#include "player_chat_info.h"
#include "Helper.h"
#include <memory>

std::shared_ptr<cs::EasyPlayerInfo> getEasyPlayerInfo(int64_t playerid)
{
    // auto playerinfo = std::make_shared<cs::EasyPlayerInfo>();

    return nullptr;
}

PlayerChatLocalInfo::PlayerChatLocalInfo(int64_t playerid) : playerid_(playerid)
{
    chatInfo = std::make_shared<cs::PlayerChatInfo>();
    chatInfo->set_player_id(playerid);
    for (int i = 0; i < PLAYER_CHAT_SIMILAR_COUNT; i++)
    {
        PlayerMessages_.push(cs::ChatMessage());
    }

    playerData = getEasyPlayerInfo(playerid);
}

std::shared_ptr<cs::EasyPlayerInfo> PlayerChatLocalInfo::getPlayerData()
{
    if (playerData == nullptr)
    {
        playerData = getEasyPlayerInfo(playerid_);
    }
    return playerData;
}

void PlayerChatLocalInfo::toMessage(cs::PlayerChatInfo &info)
{
    info.CopyFrom(*chatInfo);
}

void PlayerChatLocalInfo::fromMessage(const cs::PlayerChatInfo &info)
{
    chatInfo->CopyFrom(info);
    playerid_ = info.player_id();
}

ErrorCode PlayerChatLocalInfo::blockPlayer(std::shared_ptr<cs::EasyPlayerInfo> playerData)
{
    if (playerData == nullptr)
        return Error_argument;

    int32_t maxBlock = 3;
    if (chatInfo->block_player().size() >= maxBlock)
    {
        ELOG << "player " << playerid_ << " block player count over limit, limit: " << maxBlock;
        return Error_block_player_over_limit;
    }

    chatInfo->add_block_player()->CopyFrom(*playerData);

    return Error_success;
}

ErrorCode PlayerChatLocalInfo::unblockPlayer(int64_t playerid)
{
    if (playerid == 0)
        return Error_argument;
    auto &block_player = *chatInfo->mutable_block_player();
    for (int i = 0; i < block_player.size(); ++i)
    {
        if (block_player.Get(i).player_id() == playerid)
        {
            // 从i的位置开始删除1个元素，为了保持顺序，若不用保持顺序可以尾删
            block_player.DeleteSubrange(i, 1);
            break;
        }
    }

    return Error_success;
}

void PlayerChatLocalInfo::beUnmuted()
{
    chatInfo->set_mute_end_time(0);
}

void PlayerChatLocalInfo::beMuted(int64_t mute_time)
{
    chatInfo->set_mute_end_time(Helper::timeGetTimeS() + mute_time);
}

ErrorCode PlayerChatLocalInfo::checkChat(const cs::ChatMessage &msg)
{
    if (playerData == nullptr)
    {
        playerData = std::make_shared<cs::EasyPlayerInfo>();
        playerData->CopyFrom(msg.player_info());
    }

    // 判断禁言结束时间
    if (chatInfo->mute_end_time() > Helper::timeGetTimeS())
    {
        ELOG << "player " << playerid_ << " is muted, mute time: " << chatInfo->mute_end_time();
        return Error_muted;
    }

    // 判断频道是否存在
    if (msg.channel_id() < 0 || msg.channel_id() >= CHANNEL_MAX)
    {
        ELOG << "player " << playerid_ << " channel not exist, channel: " << msg.channel_id();
        return Error_no_channel;
    }

    // 判断是否处于聊天CD（如果当前频道的cooldown不存在则补0）
    int64_t chatCD = 0;
    auto chat_cool_downs = chatInfo->mutable_chat_cool_down();
    while (chat_cool_downs->size() <= msg.channel_id())
    {
        chat_cool_downs->Add(0);
    }
    chatCD = chat_cool_downs->Get(msg.channel_id());

    if (chatCD > Helper::timeGetTimeS())
    {
        ELOG << "player " << playerid_ << " is in chat cd, cd time: " << chatCD;
        return Error_chat_in_cd;
    }

    // 检查字数限制
    if (msg.msg().size() > PLAYER_CHAT_MAX_LENGTH)
    {
        ELOG << "player " << playerid_ << " msg too long, length: " << msg.msg().size();
        return Error_msg_too_long;
    }

    chatInfo->set_chat_cool_down(msg.channel_id(), Helper::timeGetTimeS() + PLAYER_CHAT_CD_TIME);

    // 判断是否存在多条重复消息
    PlayerMessages_.push(msg);
    if (PlayerMessages_.isAllEqual())
    {
        // 触发禁言
        beMuted();
        return Error_muted;
    }

    return Error_success;
}
