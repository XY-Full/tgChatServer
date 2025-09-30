#include "BusNet.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "Timer.h"
#include "core.pb.h"
#include "network/MsgWrapper.h"
#include "ss_msg_id.pb.h"
#include <memory>
#include <unistd.h>

BusNet::~BusNet()
{
    if (TcpClient_)
    {
        TcpClient_->stop();
    }

    delete LocalBusdShmBuffer_;
    for(const auto& it : LocalServiceMap_)
    {
        delete it.second;
    }
}

void BusNet::init(std::shared_ptr<Options> opts)
{
    opts_ = opts;
    TcpClient_ =
        std::make_unique<TcpClient>(opts_->center_ip, std::stoi(opts_->center_port), opts_->client_id, [this](uint64_t conn_id, std::shared_ptr<PackBase> msg) {
            this->onRecvMsg(reinterpret_cast<AppMsg &>(*msg));
        });
    if (TcpClient_->start())
    {
        has_center_ = true;
    }

    CenterMessageHandlers_ = {
        {SSMsgID::SS_REGIST_TO_CENTER, std::bind(&BusNet::onCenterRegistResp, this, std::placeholders::_1)},
        {SSMsgID::SS_UPDATE_SERVICE_STATUS,
         std::bind(&BusNet::onCenterUpdateServiceStatusResp, this, std::placeholders::_1)}};

    regist2Center();

    GlobalSpace()->timer_->runEvery(5.0f, std::bind(&BusNet::UpdateServiceStatusReq, this));
}

std::shared_ptr<ServiceMap> BusNet::GetServiceMap() const
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
    if (!has_center_)
        return;

    auto pack = Helper::CreateSSPack(msg);

    TcpClient_->send(pack);

    Helper::DeleteSSPack(*pack);
}

// 接收到Center消息时，更新当前路由
void BusNet::onRecvMsg(const AppMsg &msg)
{
    auto msg_id = msg.msg_id_;
    auto it = CenterMessageHandlers_.find(msg_id);
    if (it != CenterMessageHandlers_.end())
    {
        it->second(msg);
    }
    else
    {
        ELOG << "No handler found for center message with MSGID: " << msg_id;
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

    LocalBusdShmBuffer_ = new ShmRingBuffer<AppMsgWrapper>(response.local_busd_shm_name());

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
        (*new_map)[s.id()].push_back(s);
    }
    std::atomic_store(&ServiceMap_, new_map);

    ILOG << "Update status success";
}

void BusNet::broadCast(const AppMsgWrapper &msg)
{
    // 广播包全权交给Busd进行分发
    LocalBusdShmBuffer_->Push(msg);
    // 发送完之后释放内存
    Helper::DeleteSSPack(msg);
}

bool BusNet::sendMsgTo(const std::string_view &serviceName, const AppMsgWrapper &msg)
{
    std::string local_ip = "127.0.0.1";
    if (RouteCache_.find(serviceName) == RouteCache_.end())
    {
        genRouteCache(serviceName);
    }

    sendMsgByServiceInfo(RouteCache_[serviceName]->info, msg);
    return true;
}

bool BusNet::sendMsgToGroup(const std::string_view &groupName, const AppMsgWrapper &msg)
{
    // 组播包全权交给Busd进行分发
    LocalBusdShmBuffer_->Push(msg);
    // 发送完之后释放内存
    Helper::DeleteSSPack(msg);
    return true;
}

bool BusNet::sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg)
{
    std::string local_ip = "127.0.0.1";
    std::string remote_ip = info.ip();

    if (local_ip == remote_ip)
    {
        // 如果没有打开对端的ShmRingBuffer，那么打开一哈子
        if (LocalServiceMap_.find(info.id()) == LocalServiceMap_.end())
        {
            // 直接new一个ShmRingBuffer指向对端的ringbuffer
            LocalServiceMap_[info.id()] = new ShmRingBuffer<AppMsgWrapper>(info.shm_recv_buffer_name());
        }

        // 如果对方在本地，则直接推送到对端的ShmRingBuffer中
        LocalServiceMap_[info.id()]->Push(msg);
    }
    else
    {
        // 对方不在本机则直接推送到Busd
        LocalBusdShmBuffer_->Push(msg);
    }
    // 发送完之后释放内存
    if (delete_msg)
        Helper::DeleteSSPack(msg);
    return true;
}

void BusNet::genRouteCache(const std::string_view &serviceName)
{
    ss::TraceRoute pb_msg;

    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    auto send_time = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    pb_msg.mutable_request()->mutable_service_info()->CopyFrom(*genServiceInfo());
    pb_msg.mutable_request()->set_send_time(send_time);
    pb_msg.set_is_req(true);

    auto pack = Helper::CreateSSPack(pb_msg);

    sendMsgToGroup(serviceName, *pack);
}

void BusNet::onRecvRouteCache(AppMsgPtr msg)
{
    ss::TraceRoute pb_msg;
    pb_msg.ParseFromArray(msg->data_, msg->data_len_);
    // 收到远端请求的路由包
    if (pb_msg.is_req())
    {
        ss::TraceRoute rsp_msg;
        rsp_msg.mutable_response()->set_err(SSErrorCode::Error_success);
        // 回传对方的send_time
        rsp_msg.mutable_response()->set_send_time(pb_msg.request().send_time());
        rsp_msg.mutable_response()->mutable_service_info()->CopyFrom(*genServiceInfo());

        auto pack = Helper::CreateSSPack(rsp_msg);

        sendMsgByServiceInfo(pb_msg.request().service_info(), *pack);
    }
    // 收到远端回传的路由包
    else
    {
        // 获取现在的高精度时间
        auto now = std::chrono::high_resolution_clock::now();
        auto duration = now.time_since_epoch();
        auto send_time = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

        auto response = pb_msg.response();
        if (response.err() != SSErrorCode::Error_success)
        {
            ELOG << "Recv route cache failed, error code: " << SSErrorCode_Name(response.err());
            return;
        }

        // 计算延迟
        auto delay = send_time - response.send_time();
        // 如果路由缓存中不存在这个服务，则缓存一下
        if(RouteCache_.find(response.service_info().id()) == RouteCache_.end())
        {
            RouteCache_[response.service_info().id()]->delay = delay;
            RouteCache_[response.service_info().id()]->info.CopyFrom(response.service_info());
            ILOG << "New add route cache from " << response.service_info().id() << " success, delay: " << delay;
        }
        // 如果路由缓存中存在，但是新服务的延迟更低，则更新
        else if(delay > 0 && delay < RouteCache_[response.service_info().id()]->delay)
        {
            
            RouteCache_[response.service_info().id()]->delay = delay;
            RouteCache_[response.service_info().id()]->info.CopyFrom(response.service_info());
            ILOG << "Update route cache from " << response.service_info().id() << " success, delay: " << delay;
        }
        else
        {
            ELOG << "Recv route cache from " << response.service_info().id() << " failed, delay: " << delay;
            return;
        }
    }
}