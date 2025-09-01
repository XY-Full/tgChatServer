#include "BusNet.h"
#include <memory>
#include <unistd.h>
#include "../../../../public/proto_files/core.pb.h"
#include "../../../../public/proto_files/ss_msg_id.pb.h"
#include "../../../common/GlobalSpace.h"

void BusNet::init()
{
    std::string center_ip = "127.0.0.1";
    std::string center_port = "3098";

    TcpClient_ = std::make_unique<TcpClient>(center_ip, std::stoi(center_port), std::bind(&BusNet::onRecvMsg, this, std::placeholders::_1));
    TcpClient_->start();

    CenterMessageHandlers_ = {
        {SSMsgID::SS_REGIST_TO_CENTER, std::bind(&BusNet::onCenterRegistResp, this, std::placeholders::_1)}
    };
}

void BusNet::regist2Center()
{
    while(!ready_)
    {
        std::string local_ip = "127.0.0.1";
        std::string local_port = "3099";

        ss::RegistToCenter msg;
        ss::RegistToCenter::Request* request = msg.mutable_request();
        request->mutable_local_info()->set_ip(local_ip);
        request->mutable_local_info()->set_port(std::stoi(local_port));

        auto offset = Helper::CreateSSPack(msg);
        auto pack_addr = reinterpret_cast<char*>(GlobalSpace()->shm_slab_.off2ptr(offset));
        auto pack_size = reinterpret_cast<AppMsg*>(pack_addr)->header_.pack_len_;

        TcpClient_->send(pack_addr, pack_size);

        Helper::DeleteSSPack(offset);
        sleep(1);
    }
}

// 接收到Center消息时，更新当前路由
void BusNet::onRecvMsg(const AppMsg &msg)
{
    auto it = CenterMessageHandlers_.find(msg.msg_id_);
    if(it != CenterMessageHandlers_.end())
    {
        it->second(msg);
    }
    else
    {
        ELOG << "No handler found for center message with ID: " << msg.msg_id_;
    }
}

void BusNet::onCenterRegistResp(const AppMsg &msg)
{
    ss::RegistToCenter regist_response;
    regist_response.ParseFromArray(msg.data_, msg.data_len_);
    auto response = regist_response.response();
    if(response.err() != SSErrorCode::Error_success)
    {
        ELOG << "Regist to center failed, error code: " << SSErrorCode_Name(response.err());
        return;
    }
    ready_ = true;
    ILOG << "Regist to center success";
}