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
    // CRITICAL FIX #1: No need to manually delete - smart pointers handle cleanup
    if (TcpClient_)
    {
        TcpClient_->stop();
    }
    // LocalBusdShmBuffer_ and LocalServiceMap_ will be automatically cleaned up
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
        // CRITICAL FIX #1: Use smart pointers instead of raw new
        if (!LocalBusdShmBuffer_ && s.is_daemon_() && s.ip_() == local_service_info_.ip_())
        {
            LocalBusdShmBuffer_ = std::make_unique<ShmRingBuffer<AppMsgWrapper>>(s.shm_recv_buffer_name_());
        }
    }
    std::atomic_store(&ServiceMap_, new_map);

    ILOG << "Update status success";
}

bool IBusNet::sendMsgTo(const std::string_view &serviceName, const AppMsgWrapper &msg)
{
    // 先检查路由缓存
    auto cache_it = RouteCache_.find(serviceName);
    if (cache_it != RouteCache_.end() && cache_it->second)
    {
        // 使用缓存的路由
        sendMsgByServiceInfo(cache_it->second->info, msg);
        return true;
    }

    // 路由缓存不存在，从服务表中查找
    auto service_map = getServiceMap();
    if (!service_map)
    {
        ELOG << "IBusNet::sendMsgTo: Service map not ready";
        Helper::DeleteSSPack(msg);
        return false;
    }

    auto service_it = service_map->find(serviceName);
    if (service_it == service_map->end() || service_it->second.empty())
    {
        ELOG << "IBusNet::sendMsgTo: Service not found: " << serviceName;
        Helper::DeleteSSPack(msg);
        
        // 发起路由缓存请求（异步）
        genRouteCache(serviceName);
        return false;
    }

    // 找到服务，使用第一个实例（后续可以优化为选择最优实例）
    const auto &target_service = service_it->second[0];
    sendMsgByServiceInfo(target_service, msg);
    
    // 同时发起路由缓存请求，下次会更快（异步优化）
    genRouteCache(serviceName);
    
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

void IBusNet::onRecvRouteCacheRsp(const AppMsg &msg)
{
    ss::TraceRouteRsp response;
    response.ParseFromArray(msg.data_, msg.data_len_);
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
    
    std::string service_id = response.service_info_().id_();
    
    // 如果路由缓存中不存在这个服务，则缓存一下
    auto cache_it = RouteCache_.find(service_id);
    if (cache_it == RouteCache_.end())
    {
        // 创建新的缓存条目
        auto new_cache = std::make_shared<ServiceRouteCache>();
        new_cache->delay = delay;
        new_cache->info.CopyFrom(response.service_info_());
        RouteCache_[service_id] = new_cache;
        
        ILOG << "New add route cache from " << service_id << " success, delay: " << delay;
    }
    // 如果路由缓存中存在，但是新服务的延迟更低，则更新
    else if (delay > 0 && cache_it->second && delay < cache_it->second->delay)
    {
        cache_it->second->delay = delay;
        cache_it->second->info.CopyFrom(response.service_info_());
        ILOG << "Update route cache from " << service_id << " success, delay: " << delay;
    }
    else
    {
        DLOG << "Route cache from " << service_id << " not updated, current delay: " 
             << (cache_it->second ? cache_it->second->delay : 0) << ", new delay: " << delay;
    }
}

void IBusNet::onRecvRouteCacheReq(const AppMsg &msg)
{
    ss::TraceRouteReq pb_msg;
    pb_msg.ParseFromArray(msg.data_, msg.data_len_);

    ss::TraceRouteRsp rsp_msg;
    rsp_msg.set_err(SSErrorCode::Error_success);
    // 回传对方的send_time
    rsp_msg.set_send_time_(pb_msg.send_time_());
    rsp_msg.mutable_service_info_()->CopyFrom(local_service_info_);

    auto pack = Helper::CreateSSPack(rsp_msg);

    sendMsgByServiceInfo(pb_msg.service_info_(), *pack);
}