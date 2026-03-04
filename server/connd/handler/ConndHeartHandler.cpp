#include "ConndHeartHandler.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "Log.h"
#include "err_code.pb.h"
#include "gate.pb.h"

void ConndHeartHandler::onHeart(const AppMsg& msg)
{
    auto recvMsg = std::make_shared<Heart>();
    recvMsg->ParseFromArray(msg.data_, msg.data_len_);

    auto replyMsg = std::make_shared<Heart>();
    auto response = replyMsg->mutable_response();
    response->set_err(ErrorCode::Error_success);
    response->set_timestamp(static_cast<int64_t>(time(nullptr)));

    uint64_t conn_id = msg.header_.conn_id_;
    DLOG << "ConndHeartHandler: heart from conn_id=" << conn_id;

    // 直接通过 listener 发回客户端，不走 bus->Reply（避免 SS 路由）
    auto pack = Helper::CreateSSPack(*replyMsg);
    if (pack)
    {
        auto* listener = find_listener_(conn_id);
        if (listener)
        {
            listener->send(conn_id, pack);
            DLOG << "ConndHeartHandler: sent heart reply to conn_id=" << conn_id;
        }
        else
        {
            WLOG << "ConndHeartHandler: no listener for conn_id=" << conn_id;
        }
    }
    else
    {
        ELOG << "ConndHeartHandler: CreateSSPack failed for Heart";
    }
}
