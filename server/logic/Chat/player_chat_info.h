#pragma once

#include "chat.pb.h"
#include "err_code.pb.h"
#include "deque"
#include "unordered_map"
#include <google/protobuf/message.h>
#include <string>

#define PLAYER_CHAT_SIMILARITY 0.8
#define PLAYER_CHAT_SIMILAR_COUNT 3
#define ONCE_MUTE_TIME 60
#define PLAYER_CHAT_CD_TIME 3
#define PLAYER_CHAT_MAX_LENGTH 100

enum CHANNEL
{
    CHANNEL_CROSS = 0,
    CHANNEL_LOCAL = 1,
    CHANNEL_GUILD = 2,
    CHANNEL_PRIVATE = 3,
    CHANNEL_MAX
};

class FixedQueue
{
public:
    FixedQueue(size_t size) : maxSize(size), allEqual(false)
    {
    }

    void push(const cs::ChatMessage &value)
    {
        if (queue.size() == maxSize)
        {
            cs::ChatMessage removed = queue.front();
            queue.pop_front();
            freq[removed.msg()]--;
            if (freq[removed.msg()] == 0)
            {
                freq.erase(removed.msg());
            }
        }

        queue.push_back(value);
        freq[value.msg() + (char)value.emoji_id()]++;

        // 判断是否全部相等
        allEqual = (queue.size() == maxSize && freq.size() == 1);
    }

    bool isAllEqual() const
    {
        return allEqual;
    }

private:
    size_t maxSize;
    std::deque<cs::ChatMessage> queue;
    std::unordered_map<std::string, int> freq;
    bool allEqual;
};

using PlayerChatInfoPtr = std::shared_ptr<cs::PlayerChatInfo>;

class PlayerChatLocalInfo
{
public:
    PlayerChatLocalInfo(int64_t playerid);
    ~PlayerChatLocalInfo()
    {
    }

    void setPlayerId(int64_t playerid)
    {
        playerid_ = playerid;
    }

    ErrorCode checkChat(const cs::ChatMessage &msg);

    void beMuted(int64_t mute_time = ONCE_MUTE_TIME);
    void beUnmuted();

    ErrorCode blockPlayer(std::shared_ptr<cs::EasyPlayerInfo> playerData);
    ErrorCode unblockPlayer(int64_t playerid);

    void toMessage(cs::PlayerChatInfo &info);
    void fromMessage(const cs::PlayerChatInfo &info);

    PlayerChatInfoPtr getChatInfo()
    {
        return chatInfo;
    }
    std::shared_ptr<cs::EasyPlayerInfo> getPlayerData();

private:
    PlayerChatInfoPtr chatInfo;
    int64_t playerid_;
    std::shared_ptr<cs::EasyPlayerInfo> playerData;
    FixedQueue PlayerMessages_{PLAYER_CHAT_SIMILAR_COUNT};
};
