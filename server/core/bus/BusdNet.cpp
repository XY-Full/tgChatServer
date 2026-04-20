#include "BusdNet.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "Log.h"
#include "network/AppMsg.h"
#include "network/TcpClient.h"
#include <memory>

void BusdNet::genServiceInfo()
{
    // BusdNet 是daemon服务
    local_service_info_.set_ip_(opts_->local_ip);
    local_service_info_.set_port_(opts_->local_port);
    local_service_info_.set_is_daemon_(true);
    local_service_info_.set_id_("busd_" + opts_->client_id);
    local_service_info_.set_svr_name_("busd");
    
    // 设置共享内存接收缓冲区名称（busd的接收缓冲区）
    std::string shm_name = "/busd_recv_" + opts_->client_id;
    local_service_info_.set_shm_recv_buffer_name_(shm_name);
    
    ILOG << "BusdNet: Generated service info for " << opts_->client_id 
         << ", is_daemon=true, port=" << opts_->local_port;
}

bool BusdNet::sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg)
{
    std::string local_ip = opts_->local_ip;
    std::string remote_ip = info.ip_();

    // 1. 如果目标服务在本机，直接通过共享内存转发
    if (local_ip == remote_ip)
    {
        // 如果还没打开对端服务的ShmRingBuffer，则打开并缓存
        if (LocalServiceMap_.find(info.id_()) == LocalServiceMap_.end())
        {
            LocalServiceMap_[info.id_()] = std::make_unique<ShmRingBuffer<AppMsgWrapper>>(info.shm_recv_buffer_name_());
            ILOG << "BusdNet: Opened local SHM buffer for service: " << info.id_();
        }

        // 直接推送到对端服务的接收缓冲区
        bool result = LocalServiceMap_[info.id_()]->Push(msg);
        if (!result)
        {
            ELOG << "BusdNet: Failed to push message to local service: " << info.id_();
        }

        if (!result)
            ELOG << "BusdNet: Failed to push message to local service: " << info.id_();

        // SHM 路径：调用方 delete_msg=true 时释放 slab
        if (delete_msg)
            Helper::DeleteSSPack(msg);

        return result;
    }

    // 2. 如果目标服务在远程机器，通过TCP转发
    // 2.1 检查是否已经有到目标busd的连接
    std::string remote_daemon_key = remote_ip + ":" + std::to_string(info.port_());
    
    if (RemoteDaemonConnMap_.find(remote_daemon_key) == RemoteDaemonConnMap_.end())
    {
        // 创建新的TCP连接到远程busd
        auto tcp_client = std::make_unique<TcpClient>(
            remote_ip, 
            info.port_(),
            opts_->client_id + "_to_" + remote_daemon_key,
            [this](uint64_t conn_id, std::shared_ptr<AppMsg> response_msg) {
                // 接收到远程busd返回的响应消息，转发回本地服务
                this->onRecvFromRemoteDaemon(conn_id, response_msg);
            }
        );

        if (!tcp_client->start())
        {
            ELOG << "BusdNet: Failed to connect to remote busd: " << remote_daemon_key;
            if (delete_msg)
                Helper::DeleteSSPack(msg);
            return false;
        }

        RemoteDaemonConnMap_[remote_daemon_key] = std::move(tcp_client);
        ILOG << "BusdNet: Established TCP connection to remote busd: " << remote_daemon_key;
    }

    // 2.2 通过TCP连接发送消息
    auto &tcp_client = RemoteDaemonConnMap_[remote_daemon_key];
    
    // 将AppMsgWrapper转换为可发送的智能指针
    auto send_msg = std::make_shared<AppMsgWrapper>();
    send_msg->offset_ = msg.offset_;
    memcpy(send_msg->dst_, msg.dst_, sizeof(send_msg->dst_));

    tcp_client->send(send_msg);

    // TCP 发送异步完成，Connection::send 内部已 memcpy 到 partial_send_buffer_，
    // slab 内存在 send_msg（shared_ptr）析构时由 deleter 归还，不需要额外 DeleteSSPack
    return true;
}

void BusdNet::onRecvFromRemoteDaemon(uint64_t conn_id, std::shared_ptr<AppMsg> app_msg)
{
    // 从远程busd接收到消息，需要转发到本地对应的服务
    std::string dst(app_msg->dst_name_, strnlen(app_msg->dst_name_, sizeof(app_msg->dst_name_)));

    // 将TCP接收到的AppMsg放入共享内存
    auto pack_len = app_msg->header_.pack_len_;
    auto shm_offset = GlobalSpace()->shm_slab_.Alloc(pack_len);
    auto shm_addr = GlobalSpace()->shm_slab_.off2ptr(shm_offset);
    memcpy(shm_addr, app_msg.get(), pack_len);

    AppMsgWrapper wrapper;
    wrapper.offset_ = shm_offset;
    memcpy(wrapper.dst_, app_msg->dst_name_, sizeof(wrapper.dst_));

    // 复用双索引路由（含字母 → svr_name 轮询，全数字/点 → id 精准）
    bool result = sendMsgTo(dst, wrapper);
    if (!result)
    {
        ELOG << "BusdNet: Failed to forward remote message to: " << dst;
        GlobalSpace()->shm_slab_.Free(shm_offset, pack_len);
        return;
    }
    
    DLOG << "BusdNet: Forwarded message from remote daemon to local service: " << dst;
}

void BusdNet::init(std::shared_ptr<Options> opts)
{
    // 先调用父类初始化
    IBusNet::init(opts);

    // Busd特有的初始化逻辑
    ILOG << "BusdNet initialized as daemon service";
    
    // 启动消息转发循环（从LocalBusdShmBuffer_读取并转发）
    forwarder_thread_ = std::thread(&BusdNet::messageForwardLoop, this);
}

BusdNet::~BusdNet()
{
    // 停止转发线程
    running_ = false;
    if (forwarder_thread_.joinable())
    {
        forwarder_thread_.join();
    }

    // 清理所有TCP连接
    for (auto &kv : RemoteDaemonConnMap_)
    {
        kv.second->stop();
    }
    RemoteDaemonConnMap_.clear();
}

void BusdNet::messageForwardLoop()
{
    ILOG << "BusdNet: Message forward loop started";
    running_ = true;

    while (running_)
    {
        // 从本地busd缓冲区读取消息
        if (!LocalBusdShmBuffer_)
        {
            // 缓冲区还未就绪，等待初始化
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        AppMsgWrapper msg;
        if (LocalBusdShmBuffer_->Pop(msg))
        {
            // 解析目标（可以是 svr_name 或 instance id）
            std::string dst(msg.dst_, strnlen(msg.dst_, sizeof(msg.dst_)));

            // 复用双索引路由：含字母 → svr_name 轮询，全数字/点 → id 精准
            bool result = sendMsgTo(dst, msg);
            if (!result)
            {
                ELOG << "BusdNet: Failed to forward message to: " << dst;
                Helper::DeleteSSPack(msg);
            }
        }
        else
        {
            // 无消息时短暂休眠，避免CPU空转
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
    }

    ILOG << "BusdNet: Message forward loop stopped";
}

void BusdNet::broadCast(const AppMsgWrapper &msg)
{
    // BusdNet的广播：发送给所有注册的服务（包括本地和远程）
    auto service_map = getServiceMap();
    if (!service_map)
    {
        ELOG << "BusdNet: Service map not ready for broadcast";
        Helper::DeleteSSPack(msg);
        return;
    }

    ILOG << "BusdNet: Broadcasting message to all services";
    
    size_t total_sent = 0;
    size_t failed_count = 0;
    
    // 遍历所有服务
    for (const auto &service_pair : *service_map)
    {
        const auto &service_instances = service_pair.second;
        
        // 给该服务的所有实例都发送
        for (const auto &service_info : service_instances)
        {
            // 跳过daemon服务（busd自己）
            if (service_info.is_daemon_())
            {
                continue;
            }
            
            // 需要为每个目标复制一份消息，因为sendMsgByServiceInfo会删除
            auto msg_copy_offset = GlobalSpace()->shm_slab_.Alloc(
                reinterpret_cast<AppMsg*>(GlobalSpace()->shm_slab_.off2ptr(msg.offset_))->header_.pack_len_
            );
            auto msg_copy_addr = GlobalSpace()->shm_slab_.off2ptr(msg_copy_offset);
            auto original_msg = reinterpret_cast<AppMsg*>(GlobalSpace()->shm_slab_.off2ptr(msg.offset_));
            memcpy(msg_copy_addr, original_msg, original_msg->header_.pack_len_);
            
            AppMsgWrapper msg_copy;
            msg_copy.offset_ = msg_copy_offset;
            memcpy(msg_copy.dst_, msg.dst_, sizeof(msg_copy.dst_));
            
            bool result = sendMsgByServiceInfo(service_info, msg_copy, true);
            if (result)
            {
                total_sent++;
            }
            else
            {
                failed_count++;
                WLOG << "BusdNet: Failed to broadcast to service: " << service_info.id_();
            }
        }
    }
    
    // 删除原始消息
    Helper::DeleteSSPack(msg);
    
    ILOG << "BusdNet: Broadcast completed, sent: " << total_sent 
         << ", failed: " << failed_count;
}

bool BusdNet::sendMsgToGroup(const std::string_view &groupName, const AppMsgWrapper &msg)
{
    // 组播：发送给指定 svr_name 下的所有实例
    auto name_inst = std::atomic_load(&NameInstances_);
    if (!name_inst)
    {
        ELOG << "BusdNet: NameInstances not ready for group message";
        Helper::DeleteSSPack(msg);
        return false;
    }

    auto it = name_inst->find(std::string(groupName));
    if (it == name_inst->end() || it->second.empty())
    {
        ELOG << "BusdNet: Target service group not found: " << groupName;
        Helper::DeleteSSPack(msg);
        return false;
    }

    // 收集该组所有实例的 ServiceInfo（从精准表里取）
    auto svc_map = getServiceMap();
    if (!svc_map)
    {
        ELOG << "BusdNet: ServiceMap not ready for group message";
        Helper::DeleteSSPack(msg);
        return false;
    }

    std::vector<const ss::ServiceInfo*> targets;
    for (auto &kv : it->second)
    {
        auto map_it = svc_map->find(kv.first);
        if (map_it != svc_map->end() && !map_it->second.empty())
            targets.push_back(&map_it->second[0]);
    }

    if (targets.empty())
    {
        Helper::DeleteSSPack(msg);
        return false;
    }

    ILOG << "BusdNet: Sending group message to '" << groupName
         << "' with " << targets.size() << " instances";

    size_t total_sent = 0, failed_count = 0;

    for (size_t i = 0; i < targets.size(); i++)
    {
        AppMsgWrapper msg_to_send;
        if (i < targets.size() - 1)
        {
            auto original_msg = reinterpret_cast<AppMsg*>(GlobalSpace()->shm_slab_.off2ptr(msg.offset_));
            auto copy_offset  = GlobalSpace()->shm_slab_.Alloc(original_msg->header_.pack_len_);
            memcpy(GlobalSpace()->shm_slab_.off2ptr(copy_offset), original_msg, original_msg->header_.pack_len_);
            msg_to_send.offset_ = copy_offset;
            memcpy(msg_to_send.dst_, msg.dst_, sizeof(msg_to_send.dst_));
        }
        else
        {
            msg_to_send = msg;
        }

        if (sendMsgByServiceInfo(*targets[i], msg_to_send, true))
            total_sent++;
        else
        {
            failed_count++;
            WLOG << "BusdNet: Failed to send group message to: " << targets[i]->id_();
        }
    }

    ILOG << "BusdNet: Group message to '" << groupName << "' completed, sent: "
         << total_sent << ", failed: " << failed_count;

    return failed_count == 0;
}

