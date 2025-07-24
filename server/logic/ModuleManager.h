#pragma once
#include "Busd.h"
#include "Chat/chat_mgr.h"
#include "TelegramBot/TelegramBot.h"
#include <memory>
#include <vector>

class ILogic;
class HeartHandler;
class TelegramBot;
class ChatMgr;

class ModuleManager 
{
public:
    explicit ModuleManager(Busd* bus, Timer* loop)
        : busd_(bus) {}

    // 注册模块（每个模块自注册消息 -> 处理函数）
    void registerAllModule();

    std::shared_ptr<HeartHandler> getHeartHandler() { return heartMgr_; }
    std::shared_ptr<TelegramBot> getConfigUpdate() { return telegramBotMgr_; }
    std::shared_ptr<ChatMgr> getChatMgr() { return chatMgr_; }

private:
    std::shared_ptr<HeartHandler> heartMgr_;
    std::shared_ptr<TelegramBot> telegramBotMgr_;
    std::shared_ptr<ChatMgr> chatMgr_;

private:
    Busd* busd_;
    Timer* loop_;
    std::vector<ILogic*> modules_;
};