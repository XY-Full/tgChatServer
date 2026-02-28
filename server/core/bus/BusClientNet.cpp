#include "BusClientNet.h"
#include "Helper.h"
#include "Log.h"

void BusClientNet::genServiceInfo()
{
    // BusClientNet 是普通服务，不是daemon
    local_service_info_.set_ip_(opts_->local_ip);
    local_service_info_.set_port_(opts_->local_port);
    local_service_info_.set_is_daemon_(false);  // 关键：标记为非daemon
    local_service_info_.set_id_(opts_->client_id);
    local_service_info_.set_svr_name_("service_" + std::to_string(opts_->service_id));
    
    // 设置共享内存接收缓冲区名称
    std::string shm_name = "/ibus_" + opts_->client_id;
    local_service_info_.set_shm_recv_buffer_name_(shm_name);
    
    ILOG << "BusClientNet: Generated service info for " << opts_->client_id 
         << ", is_daemon=false";
}

bool BusClientNet::sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg)
{
    std::string local_ip = "127.0.0.1";
    std::string remote_ip = info.ip_();

    if (local_ip == remote_ip)
    {
        // CRITICAL FIX #1: Use smart pointers instead of raw new
        if (LocalServiceMap_.find(info.id_()) == LocalServiceMap_.end())
        {
            LocalServiceMap_[info.id_()] = std::make_unique<ShmRingBuffer<AppMsgWrapper>>(info.shm_recv_buffer_name_());
            ILOG << "BusClientNet: Opened local SHM buffer for service: " << info.id_();
        }

        // If target is local, push directly to peer's ShmRingBuffer
        if (!LocalServiceMap_[info.id_()]->Push(msg)) {
            ELOG << "Failed to push message to local service: " << info.id_();
            return false;
        }
    }
    else
    {
        // Remote target: push to Busd
        if (!LocalBusdShmBuffer_)
        {
            ELOG << "BusClientNet: LocalBusdShmBuffer not initialized";
            return false;
        }
        if (!LocalBusdShmBuffer_->Push(msg)) {
            ELOG << "Failed to push message to LocalBusdShmBuffer";
            return false;
        }
    }
    
    // SHM Push 完成后不需要释放：调用方的 shared_ptr 析构时 deleter 会归还 slab
    return true;
}

void BusClientNet::broadCast(const AppMsgWrapper &msg)
{
    if (!LocalBusdShmBuffer_)
    {
        ELOG << "BusClientNet: LocalBusdShmBuffer not initialized for broadcast";
        return;
    }
    
    LocalBusdShmBuffer_->Push(msg);
    DLOG << "BusClientNet: Broadcast message forwarded to local Busd";
}

bool BusClientNet::sendMsgToGroup(const std::string_view &groupName, const AppMsgWrapper &msg)
{
    if (!LocalBusdShmBuffer_)
    {
        ELOG << "BusClientNet: LocalBusdShmBuffer not initialized for group message";
        return false;
    }
    
    LocalBusdShmBuffer_->Push(msg);
    DLOG << "BusClientNet: Group message to '" << groupName << "' forwarded to local Busd";
    return true;
}
