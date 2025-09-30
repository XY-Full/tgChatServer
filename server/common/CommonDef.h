// 解包
#define PROCESS_NETPACK_BEGIN(proto_name)\
    auto recvMsg = std::make_shared<PB_T>();\
    recvMsg->ParsePartialFromArray(pPack->data_, pPack->data_len_);\
    ILOG << recvMsg->Utf8DebugString();\
    auto replyMsg = std::make_shared<PB_T>();\
    int64_t playerid = pPack->Head()->uid;\
    auto request = recvMsg->mutable_request();\
    (void)request;\
    auto response = replyMsg->mutable_response();\
    response->set_err(Error_success);\
    do {

#define PROCESS_NETPACK_END()\
    } while(0);\
    if(response->err() != Error_success)\
    {\
        ELOG << ErrorCode_Name((ErrorCode)response->err()) << ", " << replyMsg->ShortDebugString();\
    }\
    else\
    {\
        ILOG << replyMsg->Utf8DebugString();\
    }\
    GlobalSpace()->bus_->Reply(recvMsg, *replyMsg);