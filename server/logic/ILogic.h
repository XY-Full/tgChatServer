#pragma once
#include "bus/IBus.h"
#include "Log.h"
#include "err_code.pb.h"
#include "msg_id.pb.h"

#define PROCESS_NETPACK_BEGIN(MSG_TYPE)                                                                                \
    int64_t uid = 1;                                                                                           \
    auto recvMsg = std::make_shared<MSG_TYPE>();                                                                       \
    recvMsg->ParseFromString(msg.data_);                                                                               \
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