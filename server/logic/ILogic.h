#pragma once
#include "Busd.h"
#include "err_code.pb.h"
#include "msg_id.pb.h"
#include "Log.h"

#define PROCESS_NETPACK_BEGIN(MSG_TYPE) \
    int64_t uid = pPack.uid;\
    auto recvMsg = std::make_shared<MSG_TYPE>();\
    recvMsg->ParseFromString(pPack.msg);\
    ILOG << recvMsg->Utf8DebugString();\
    auto request = recvMsg->mutable_request();\
    auto replyMsg = std::make_shared<MSG_TYPE>();\
    auto response = replyMsg->mutable_response();\
    response->set_err(ErrorCode::Error_success);\
    do {

#define PROCESS_NETPACK_END() \
    } while(0);\
    if(response->err() != ErrorCode::Error_success)\
    {\
        ELOG << ErrorCode_Name(response->err()) << ", " << replyMsg->ShortDebugString();\
    }\
    else\
    {\
        ILOG << replyMsg->Utf8DebugString();\
    }\
	busd_->replyToClient(pPack, *replyMsg);


// 业务逻辑接口，强制实现注册方法
class ILogic 
{
public:
    ILogic(Busd* busd) : busd_(busd) {}
    virtual ~ILogic() = default;
    virtual void registerHandlers(){};

protected:
    Busd* busd_;
};