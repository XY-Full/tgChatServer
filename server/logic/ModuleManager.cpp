#include "ModuleManager.h"
#include "Heart/HeartHandler.h"
#include "Chat/ChatHandler.h"
#include "Login/LoginHandler.h"

void ModuleManager::registerAllModule()
{
    heartMgr_ = std::make_shared<HeartHandler>();
    chatHandler_ = std::make_shared<ChatHandler>();
    loginHandler_ = std::make_shared<LoginHandler>();
}
