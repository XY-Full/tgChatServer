#include "LoginHandler.h"
#include "GlobalSpace.h"
#include "login.pb.h"

LoginHandler::LoginHandler()
{
    registerHandlers();
}

void LoginHandler::registerHandlers()
{
    GlobalSpace()->bus_->RegistMessage(MsgID::CS_PLAYER_LOGIN_GAME,
                                       std::bind(&LoginHandler::onLoginGame, this, std::placeholders::_1));
}

// ─────────────────────────────────────────────
// CS_PLAYER_LOGIN_GAME：玩家登录游戏
// ─────────────────────────────────────────────
void LoginHandler::onLoginGame(const AppMsg &msg)
{
    PROCESS_NETPACK_BEGIN(cs::PlayerLoginGame);

    // TODO(dbproxy): 经 SS_DB_QUERY 加载/创建玩家数据（users 表），
    //                建立 uid -> 玩家对象 的内存索引
    // TODO(notify):  登录成功后下推离线期间的未读消息

    ILOG << "LoginHandler: player login, uid=" << uid;

    PROCESS_NETPACK_END();
}
