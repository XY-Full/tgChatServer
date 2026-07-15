#include "ILogic.h"
#include "network/AppMsg.h"

#include <cstdint>

// 聊天模块框架：消息收发、历史拉取、聊天状态查询。
// 当前为骨架实现，持久化(dbproxy)与下推广播(SC_NOTIFY)待接入。
class ChatHandler : public ILogic
{
public:
    ChatHandler();

    void registerHandlers() override;

private:
    void onSendMessage(const AppMsg &msg);  // CS_PLAYER_SEND_MESSAGE
    void onReqMessage(const AppMsg &msg);   // CS_PLAYER_REQ_MESSAGE
    void onReqChatInfo(const AppMsg &msg);  // CS_PLAYER_REQ_CHAT_INFO

    // TODO(dbproxy): 接入后由数据库自增主键生成全局消息ID，此处为进程内占位
    int64_t nextMsgId() { return next_msg_id_++; }

    static constexpr size_t kMaxChatMsgLen = 512; // 单条消息最大字节数
    static constexpr int32_t kMinChannelId = 0;   // 频道范围见 chat.proto：0~3 跨服/本服/工会/私聊
    static constexpr int32_t kMaxChannelId = 3;

    int64_t next_msg_id_ = 1;
};
