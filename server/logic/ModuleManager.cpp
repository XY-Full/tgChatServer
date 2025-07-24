#include "ModuleManager.h"
#include "Heart/HeartHandler.h"
#include "TelegramBot/TelegramBot.h"

void ModuleManager::registerAllModule() 
{
    heartMgr_ = std::make_shared<HeartHandler>(busd_);

    telegramBotMgr_ = std::make_shared<TelegramBot>(busd_);
}