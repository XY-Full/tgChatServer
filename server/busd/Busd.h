#pragma once
#include "Channel.h"
#include "EventPump.hpp"
#include "NetPack.h"
#include "Timer.h"
#include "core_user.pb.h"
#include <functional>
#include <memory>
#include <thread>
#include <unordered_map>
#include <array>

class Busd : public EventPump
{
public:
    using NetPackHandler = std::function<void(const NetPack&)>;
    using EventHandler = std::function<MessagePtr()>;
    using ForwardHandleFunc = std::function<void(const NetPack&)>;

    Busd(Timer* loop,
         Channel<std::pair<int64_t, std::shared_ptr<NetPack>>>* in,
         Channel<std::pair<int64_t, std::shared_ptr<NetPack>>>* out);

    // 注册网络包处理函数
    void registerHandler(int32_t msg_id, NetPackHandler handler);

    void start();
    void stop();

    // 像单个玩家发送消息
    void sendToClient(int64_t uid, int32_t msg_id, const google::protobuf::Message& msg);
    // 像所有玩家发送消息
    void broadcastTotalPlayer(int32_t msg_id, const google::protobuf::Message &msg);

    void replyToClient(const NetPack& request, const google::protobuf::Message& msg);

private:
    void processMessages();
    void forwardC2S(const NetPack& pack);
    void forwardS2C(const NetPack& pack);

    std::unordered_map<int32_t, NetPackHandler> netpackHandlers_;
    std::unordered_map<std::string, NetPackHandler> eventHandlers_;
    Channel<std::pair<int64_t, std::shared_ptr<NetPack>>>* in_channel_ = nullptr;
    Channel<std::pair<int64_t, std::shared_ptr<NetPack>>>* out_channel_ = nullptr;
    Timer* loop_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_ = false;

    std::unordered_map<int64_t, core::UsrSvrMappingData> UsrSvrMap;
    std::array<ForwardHandleFunc, std::numeric_limits<int8_t>::max()> forwardHandleArray_;
};
