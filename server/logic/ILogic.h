#pragma once
#include "bus/IBus.h"
#include "Log.h"
#include "err_code.pb.h"
#include "msg_id.pb.h"

#define PROCESS_NETPACK_BEGIN(MSG_TYPE)                                                                                \
    auto recvMsg = std::make_shared<MSG_TYPE>();                                                                       \
    if (!recvMsg->ParseFromArray(msg.data_, msg.data_len_))                                                            \
    {                                                                                                                  \
        ELOG << "parse " << #MSG_TYPE << " failed, msg_id=" << msg.msg_id_                                             \
             << " conn_id=" << msg.header_.conn_id_ << " data_len=" << msg.data_len_;                                  \
        return;                                                                                                        \
    }                                                                                                                  \
    /* TODO(account): 账号体系完善后，由 connd 透传鉴权后的玩家ID，此处暂以 conn_id 占位 */                                  \
    int64_t uid = static_cast<int64_t>(msg.header_.conn_id_);                                                          \
    (void)uid;                                                                                                         \
    ILOG << recvMsg->Utf8DebugString();                                                                                \
    auto request = recvMsg->mutable_request();                                                                         \
    auto replyMsg = std::make_shared<MSG_TYPE>();                                                                      \
    auto response = replyMsg->mutable_response();                                                                      \
    response->set_err(ErrorCode::Error_success);                                                                       \
    do                                                                                                                 \
    {

#define PROCESS_NETPACK_END()                                                                                          \
    }                                                                                                                  \
    while (0)                                                                                                          \
        ;                                                                                                              \
    if (response->err() != ErrorCode::Error_success)                                                                   \
    {                                                                                                                  \
        ELOG << ErrorCode_Name(response->err()) << ", " << replyMsg->ShortDebugString();                               \
    }                                                                                                                  \
    else                                                                                                               \
    {                                                                                                                  \
        ILOG << replyMsg->Utf8DebugString();                                                                           \
    }                                                                                                                  \
    GlobalSpace()->bus_->Reply(msg, *replyMsg);

// 业务逻辑接口，强制实现注册方法
class ILogic
{
public:
    ILogic()
    {
    }
    virtual ~ILogic() = default;
    virtual void registerHandlers() {};
};