#include "ModuleManager.h"
#include "Heart/HeartHandler.h"
#include "TelegramBot/TelegramBot.h"
#include "Chat/chat_mgr.h"

void ModuleManager::registerAllModule() 
{
    heartMgr_ = std::make_shared<HeartHandler>(busd_);

    telegramBotMgr_ = std::make_shared<TelegramBot>(busd_);

    chatMgr_ = std::make_shared<ChatMgr>(busd_);
}