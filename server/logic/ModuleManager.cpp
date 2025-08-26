#include "ModuleManager.h"
#include "Chat/chat_mgr.h"
#include "Heart/HeartHandler.h"
#include "TelegramBot/TelegramBot.h"

void ModuleManager::registerAllModule()
{
    heartMgr_ = std::make_shared<HeartHandler>(busd_);

    chatMgr_ = std::make_shared<ChatMgr>(busd_);

    telegramBotMgr_ = std::make_shared<TelegramBot>(busd_);
}
