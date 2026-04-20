#include "IBusNet.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "Timer.h"
#include "core.pb.h"
#include "network/MsgWrapper.h"
#include "ss_msg_id.pb.h"
#include <cctype>
#include <memory>
#include <unistd.h>

IBusNet::~IBusNet()
{
    if (TcpClient_)
    {
        TcpClient_->stop();
    }
}

void IBusNet::init(std::shared_ptr<Options> opts)
{
    opts_ = opts;
    bool center_opts = true;

    if (opts_->center_ip.empty() || opts_->center_port <= 0)
    {
        WLOG << "Center IP or port is empty, IBusNet will not connect to center";
        center_opts = false;
    }

    if (center_opts)
    {
        TcpClient_ = std::make_unique<TcpClient>(
            opts_->center_ip, opts_->center_port, opts_->client_id,
            [this](uint64_t conn_id, std::shared_ptr<AppMsg> msg) { this->onRecvMsg(*msg); });

        if (TcpClient_->start())
        {
            has_center_ = true;
        }
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
    if (!pack)
    {
        ELOG << "sendMsgToCenter: CreateSSPack failed";
        return;
    }

    // pack 是 shared_ptr<AppMsgWrapper>，TcpClient::send 持有它直到发送完成
    // 不能在 send 后立即 DeleteSSPack，否则 slab 内存提前释放导致发送数据损坏
    TcpClient_->send(pack);
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
    while (!ready_ && has_center_)
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

    // 同时重建两张路由表
    auto new_map       = std::make_shared<ServiceMap>();          // key = instance id
    auto new_instances = std::make_shared<ServiceNameInstances>(); // key = svr_name

    for (auto &s : response.service_info_map_())
    {
        // 下线实例不加入路由表
        if (s.is_offline_())
            continue;

        // 精准路由表（每个 id 唯一）
        (*new_map)[s.id_()].push_back(s);

        // 按名路由表：将 ServiceInfo 映射为 ServiceInstancePtr
        auto &inst_map = (*new_instances)[s.svr_name_()];
        auto  inst_it  = inst_map.find(s.id_());
        if (inst_it == inst_map.end())
        {
            // 新建实例
            auto inst = std::make_shared<ServiceInstance>();
            inst->id          = s.id_();
            inst->svc_name    = s.svr_name_();
            inst->address     = s.ip_();
            inst->port        = static_cast<uint16_t>(s.port_());
            inst->shm_recv_buffer_name = s.shm_recv_buffer_name_();
            inst->connections = s.connections_();
            inst->cpu_percent = s.cpu_percent_();
            inst->load_score  = s.load_score_();
            inst_map[s.id_()] = inst;
        }
        else
        {
            // 更新已有实例的运行时指标
            inst_it->second->connections = s.connections_();
            inst_it->second->cpu_percent = s.cpu_percent_();
            inst_it->second->load_score  = s.load_score_();
        }

        // 发现本机 busd 时建立 SHM 链路
        if (!LocalBusdShmBuffer_ && s.is_daemon_() && s.ip_() == local_service_info_.ip_())
        {
            LocalBusdShmBuffer_ = std::make_unique<ShmRingBuffer<AppMsgWrapper>>(s.shm_recv_buffer_name_());
        }
    }

    std::atomic_store(&ServiceMap_,    new_map);
    std::atomic_store(&NameInstances_, new_instances);

    ILOG << "Update status success, id_map=" << new_map->size()
         << " name_map=" << new_instances->size();
}

bool IBusNet::sendMsgTo(const std::string_view &target, const AppMsgWrapper &msg, LBStrategy strategy)
{
    // 判断寻址类型：含字母 → svr_name（LB 策略），全数字/点 → id（精准）
    bool is_name = false;
    for (unsigned char c : target) {
        if (std::isalpha(c)) { is_name = true; break; }
    }

    if (is_name)
    {
        // ── 按 svr_name 路由，由 LB 策略选实例 ──
        auto instances_map = std::atomic_load(&NameInstances_);
        if (!instances_map)
        {
            ELOG << "IBusNet::sendMsgTo: NameInstances not ready, target=" << target;
            return false;
        }
        auto it = instances_map->find(std::string(target));
        if (it == instances_map->end() || it->second.empty())
        {
            ELOG << "IBusNet::sendMsgTo: service name not found: " << target;
            return false;
        }

        // 用 LB 选择实例（每次调用按传入策略创建 LB）
        auto lb = lb_factory_.create(strategy);
        auto selected = lb ? lb->select(it->second) : nullptr;
        if (!selected)
        {
            ELOG << "IBusNet::sendMsgTo: LB returned nullptr for " << target;
            return false;
        }

        // 根据选中实例的静态信息找到对应的 ServiceInfo
        auto svc_map = getServiceMap();
        if (!svc_map) return false;
        auto svc_it = svc_map->find(selected->id);
        if (svc_it == svc_map->end() || svc_it->second.empty())
        {
            ELOG << "IBusNet::sendMsgTo: selected id not in ServiceMap: " << selected->id;
            return false;
        }
        return sendMsgByServiceInfo(svc_it->second[0], msg);
    }
    else
    {
        // ── 按 instance id 精准路由 ──
        auto svc_map = getServiceMap();
        if (!svc_map)
        {
            ELOG << "IBusNet::sendMsgTo: ServiceMap not ready, target=" << target;
            return false;
        }
        auto it = svc_map->find(std::string(target));
        if (it == svc_map->end() || it->second.empty())
        {
            ELOG << "IBusNet::sendMsgTo: instance id not found: " << target;
            return false;
        }
        return sendMsgByServiceInfo(it->second[0], msg);
    }
}

