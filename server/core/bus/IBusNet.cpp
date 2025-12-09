#include "IBusNet.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "Timer.h"
#include "core.pb.h"
#include "network/MsgWrapper.h"
#include "ss_msg_id.pb.h"
#include <memory>
#include <unistd.h>

IBusNet::~IBusNet()
{
    if (TcpClient_)
    {
        TcpClient_->stop();
    }

    delete LocalBusdShmBuffer_;
    for (const auto &it : LocalServiceMap_)
    {
        delete it.second;
    }
}

void IBusNet::init(std::shared_ptr<Options> opts)
{
    opts_ = opts;

    TcpClient_ = std::make_unique<TcpClient>(
        opts_->center_ip, std::stoi(opts_->center_port), opts_->client_id,
        [this](uint64_t conn_id, std::shared_ptr<PackBase> msg) { this->onRecvMsg(reinterpret_cast<AppMsg &>(*msg)); });

    if (TcpClient_->start())
    {
        has_center_ = true;
    }

    CenterMessageHandlers_ = {
        {SSMsgID::SS_REGIST_TO_CENTER, std::bind(&IBusNet::onCenterRegistRsp, this, std::placeholders::_1)},
        {SSMsgID::SS_UPDATE_SERVICE_STATUS,
         std::bind(&IBusNet::onCenterUpdateServiceStatusRsp, this, std::placeholders::_1)}};

    genServiceInfo();
    regist2Center();
}

std::shared_ptr<ServiceMap> IBusNet::getServiceMap() const
{
    return std::atomic_load(&ServiceMap_);
}

void IBusNet::genServiceInfo()
{
    std::string local_ip = "127.0.0.1";
    std::string local_port = "3099";

    local_service_info_.set_ip_(local_ip);
    local_service_info_.set_port_(std::stoi(local_port));
    local_service_info_.set_is_daemon_(false);
}

void IBusNet::sendMsgToCenter(const google::protobuf::Message &msg)
{
    if (!has_center_)
        return;

    auto pack = Helper::CreateSSPack(msg);

    TcpClient_->send(pack);

    Helper::DeleteSSPack(*pack);
}

// 接收到Center消息时，更新当前路由
void IBusNet::onRecvMsg(const AppMsg &msg)
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

void IBusNet::regist2Center()
{
    while (!ready_)
    {
        ss::RegistToCenter msg;
        ss::RegistToCenter::Request *request = msg.mutable_request();
        request->mutable_local_info_()->CopyFrom(local_service_info_);

        sendMsgToCenter(msg);
        sleep(1);
    }
}

void IBusNet::onCenterRegistRsp(const AppMsg &msg)
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

    // 注册完毕后，开始更新服务状态
    GlobalSpace()->timer_->runEvery(5.0f, std::bind(&IBusNet::updateServiceStatusReq, this));

    ILOG << "Regist to center success";
}

void IBusNet::updateServiceStatusReq()
{
    if (!ready_)
    {
        ELOG << "IBusNet is not ready";
        return;
    }

    ss::UpdateServiceStatus msg;
    ss::UpdateServiceStatus::Request *request = msg.mutable_request();
    request->mutable_local_info_()->CopyFrom(local_service_info_);
    request->set_send_time_(Helper::timeGetTimeUS());
    sendMsgToCenter(msg);
}

void IBusNet::onCenterUpdateServiceStatusRsp(const AppMsg &msg)
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
    for (auto &s : response.service_info_map_())
    {
        (*new_map)[s.id_()].push_back(s);
        // 如果本地没有LocalBusdShmBuffer，则设置一下
        if (!LocalBusdShmBuffer_ && s.is_daemon_() && s.ip_() == local_service_info_.ip_())
        {
            LocalBusdShmBuffer_ = new ShmRingBuffer<AppMsgWrapper>(s.shm_recv_buffer_name_());
        }
    }
    std::atomic_store(&ServiceMap_, new_map);

    ILOG << "Update status success";
}

void IBusNet::broadCast(const AppMsgWrapper &msg)
{
    // 广播包全权交给Busd进行分发
    LocalBusdShmBuffer_->Push(msg);
    // 发送完之后释放内存
    Helper::DeleteSSPack(msg);
}

bool IBusNet::sendMsgTo(const std::string_view &serviceName, const AppMsgWrapper &msg)
{
    std::string local_ip = "127.0.0.1";
    if (RouteCache_.find(serviceName) == RouteCache_.end())
    {
        genRouteCache(serviceName);
    }

    sendMsgByServiceInfo(RouteCache_[serviceName]->info, msg);
    return true;
}

bool IBusNet::sendMsgToGroup(const std::string_view &groupName, const AppMsgWrapper &msg)
{
    // 组播包全权交给Busd进行分发
    LocalBusdShmBuffer_->Push(msg);
    // 发送完之后释放内存
    Helper::DeleteSSPack(msg);
    return true;
}

void IBusNet::genRouteCache(const std::string_view &serviceName)
{
    ss::TraceRouteReq pb_msg;

    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    auto send_time = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    pb_msg.mutable_service_info_()->CopyFrom(local_service_info_);
    pb_msg.set_send_time_(send_time);

    auto pack = Helper::CreateSSPack(pb_msg);

    sendMsgToGroup(serviceName, *pack);
}

void IBusNet::onRecvRouteCacheRsp(AppMsgPtr msg)
{
    ss::TraceRouteRsp response;
    response.ParseFromArray(msg->data_, msg->data_len_);
    // 获取现在的高精度时间
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    auto send_time = std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();

    if (response.err() != SSErrorCode::Error_success)
    {
        ELOG << "Recv route cache failed, error code: " << SSErrorCode_Name(response.err());
        return;
    }

    // 计算延迟
    auto delay = send_time - response.send_time_();
    // 如果路由缓存中不存在这个服务，则缓存一下
    if (RouteCache_.find(response.service_info_().id_()) == RouteCache_.end())
    {
        RouteCache_[response.service_info_().id_()]->delay = delay;
        RouteCache_[response.service_info_().id_()]->info.CopyFrom(response.service_info_());
        ILOG << "New add route cache from " << response.service_info_().id_() << " success, delay: " << delay;
    }
    // 如果路由缓存中存在，但是新服务的延迟更低，则更新
    else if (delay > 0 && delay < RouteCache_[response.service_info_().id_()]->delay)
    {

        RouteCache_[response.service_info_().id_()]->delay = delay;
        RouteCache_[response.service_info_().id_()]->info.CopyFrom(response.service_info_());
        ILOG << "Update route cache from " << response.service_info_().id_() << " success, delay: " << delay;
    }
    else
    {
        ELOG << "Recv route cache from " << response.service_info_().id_() << " failed, delay: " << delay;
        return;
    }
}

void IBusNet::onRecvRouteCacheReq(AppMsgPtr msg)
{
    ss::TraceRouteReq pb_msg;
    pb_msg.ParseFromArray(msg->data_, msg->data_len_);

    ss::TraceRouteRsp rsp_msg;
    rsp_msg.set_err(SSErrorCode::Error_success);
    // 回传对方的send_time
    rsp_msg.set_send_time_(pb_msg.send_time_());
    rsp_msg.mutable_service_info_()->CopyFrom(local_service_info_);

    auto pack = Helper::CreateSSPack(rsp_msg);

    sendMsgByServiceInfo(pb_msg.service_info_(), *pack);
}