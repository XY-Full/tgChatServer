#include "ILogic.h"
#include "network/AppMsg.h"

// 登录模块框架：玩家进入游戏后的数据加载入口。
// 当前为骨架实现，玩家数据加载(dbproxy)待接入。
class LoginHandler : public ILogic
{
public:
    LoginHandler();

    void registerHandlers() override;

private:
    void onLoginGame(const AppMsg &msg); // CS_PLAYER_LOGIN_GAME
};
