#pragma once
#include <memory>

class HeartHandler;
class ChatHandler;
class LoginHandler;

class ModuleManager
{
public:
    explicit ModuleManager()
    {
    }

    // 注册模块（每个模块构造时自注册消息 -> 处理函数）
    void registerAllModule();

    std::shared_ptr<HeartHandler> getHeartHandler()
    {
        return heartMgr_;
    }
    std::shared_ptr<ChatHandler> getChatHandler()
    {
        return chatHandler_;
    }
    std::shared_ptr<LoginHandler> getLoginHandler()
    {
        return loginHandler_;
    }

private:
    std::shared_ptr<HeartHandler> heartMgr_;
    std::shared_ptr<ChatHandler> chatHandler_;
    std::shared_ptr<LoginHandler> loginHandler_;
};
