#include "BusNet.h"
#include "../../../../public/proto_files/core.pb.h"
#include "../../../../public/proto_files/ss_msg_id.pb.h"
#include "../../../common/GlobalSpace.h"
#include <memory>
#include <unistd.h>

void BusNet::init()
{
    std::string center_ip = "127.0.0.1";
    std::string center_port = "3098";

    TcpClient_ = std::make_unique<TcpClient>(center_ip, std::stoi(center_port),
                                             std::bind(&BusNet::onRecvMsg, this, std::placeholders::_1));
    TcpClient_->start();

    CenterMessageHandlers_ = {
        {SSMsgID::SS_REGIST_TO_CENTER, std::bind(&BusNet::onCenterRegistResp, this, std::placeholders::_1)}};

    regist2Center();

    GlobalSpace()->timer_->runEvery(5.0f, std::bind(&BusNet::UpdateServiceStatusReq, this));
}

std::shared_ptr<ServiceMap> BusNet::GetMap() const
{
    return std::atomic_load(&ServiceMap_);
}

std::shared_ptr<ss::ServiceInfo> BusNet::genServiceInfo()
{
    std::string local_ip = "127.0.0.1";
    std::string local_port = "3099";

    auto service_info = std::make_shared<ss::ServiceInfo>();
    service_info->set_ip(local_ip);
    service_info->set_port(std::stoi(local_port));

    return service_info;
}

void BusNet::sendMsgToCenter(const google::protobuf::Message &msg)
{
    auto offset = Helper::CreateSSPack(msg);
    auto pack_addr = reinterpret_cast<char *>(GlobalSpace()->shm_slab_.off2ptr(offset));
    auto pack_size = reinterpret_cast<AppMsg *>(pack_addr)->header_.pack_len_;

    TcpClient_->send(pack_addr, pack_size);

    Helper::DeleteSSPack(offset);
}

// 接收到Center消息时，更新当前路由
void BusNet::onRecvMsg(const AppMsg &msg)
{
    auto it = CenterMessageHandlers_.find(msg.msg_id_);
    if (it != CenterMessageHandlers_.end())
    {
        it->second(msg);
    }
    else
    {
        ELOG << "No handler found for center message with ID: " << msg.msg_id_;
    }
}

void BusNet::regist2Center()
{
    while (!ready_)
    {
        ss::RegistToCenter msg;
        ss::RegistToCenter::Request *request = msg.mutable_request();
        request->mutable_local_info()->CopyFrom(*genServiceInfo());

        sendMsgToCenter(msg);
        sleep(1);
    }
}

void BusNet::onCenterRegistResp(const AppMsg &msg)
{
    ss::RegistToCenter regist_response;
    regist_response.ParseFromArray(msg.data_, msg.data_len_);
    auto response = regist_response.response();
    if (response.err() != SSErrorCode::Error_success)
    {
        ELOG << "Regist to center failed, error code: " << SSErrorCode_Name(response.err());
        return;
    }
    ready_ = true;

    LocalBusdShmBuffer_ = new ShmRingBuffer<uint32_t>(response.local_busd_shm_name());

    auto init_map = std::make_shared<ServiceMap>();
    std::atomic_store(&ServiceMap_, init_map);

    ILOG << "Regist to center success";
}

void BusNet::UpdateServiceStatusReq()
{
    if (!ready_)
    {
        ELOG << "BusNet is not ready";
        return;
    }

    ss::UpdateServiceStatus msg;
    ss::UpdateServiceStatus::Request *request = msg.mutable_request();
    request->mutable_local_info()->CopyFrom(*genServiceInfo());
    sendMsgToCenter(msg);
}

void BusNet::onCenterUpdateServiceStatusResp(const AppMsg &msg)
{
    ss::UpdateServiceStatus update_service_status;
    update_service_status.ParseFromArray(msg.data_, msg.data_len_);
    auto response = update_service_status.response();
    if (response.err() != SSErrorCode::Error_success)
    {
        ELOG << "Update status failed, error code: " << SSErrorCode_Name(response.err());
        return;
    }

    auto new_map = std::make_shared<ServiceMap>();
    for (auto &s : response.service_info_map())
    {
        (*new_map)[s.name()].push_back(s);
    }
    std::atomic_store(&ServiceMap_, new_map);

    ILOG << "Update status success";
}

void BusNet::broadCast(const AppMsg &msg)
{
    std::string local_ip = "127.0.0.1";    
    for (const auto &[group_name, group_service_info] : *ServiceMap_)
    {
        for(const auto &service_info : group_service_info)
        {
            if(service_info.ip() == local_ip)
            {
                LocalBusdShmBuffer_->Push();
            }
        }
    }
}

void BusNet::sendMsgTo(std::string serviceName, const AppMsg &msg)
{
}

void BusNet::sendMsgToGroup(std::string groupName, const AppMsg &msg)
{
}
