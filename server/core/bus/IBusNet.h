#pragma once
#include "Helper.h"
#include "IBus.h"
#include "LoadBalancer.h"
#include "ServiceInstance.h"
#include "network/TcpClient.h"
#include "ss_base.pb.h"
#include <atomic>
#include <memory>
#include <unordered_map>

#define EXTRACT_APPMSG_WRAPPER(msg) (reinterpret_cast<AppMsg *>(msg.offset_))

struct Options
{
    // =======配置参数=======
    uint8_t region_id = 0;
    uint8_t zone_id = 0;
    uint8_t service_id = 0;
    uint8_t instance_id = 0;
    size_t local_ring_size = 1 << 20; // 1MB
    std::string local_ip = "127.0.0.1";
    int32_t local_port = 3089;
    std::string center_ip = "";
    int32_t center_port = 0;
    // =======配置参数=======

    std::string client_id = "";

    Options(const ConfigManager &config_manager)
    {
        region_id = config_manager.getValue<uint8_t>("region_id", 0);
        zone_id = config_manager.getValue<uint8_t>("zone_id", 0);
        service_id = config_manager.getValue<uint8_t>("service_id", 0);
        instance_id = config_manager.getValue<uint8_t>("instance_id", 0);

        local_ring_size = config_manager.getValue<size_t>("local_ring_size", 1 << 20);

        local_ip = config_manager.getValue<std::string>("local_ip", "127.0.0.1");
        local_port = config_manager.getValue<int32_t>("local_port", 3089);

        center_ip = config_manager.getValue<std::string>("center_ip", "");
        center_port = config_manager.getValue<int32_t>("center_port", 0);

        client_id = std::to_string(region_id) + "." + std::to_string(zone_id) + "." + std::to_string(service_id) + "." +
                    std::to_string(instance_id);
    }
};

namespace ss
{
class ServiceInfo;
}
class BusClient;

using CenterMessageHandler = std::function<void(const AppMsg &)>;
// key = 实例 id（如 "0.1.3.0"）→ 精准路由
using ServiceMap = std::unordered_map<std::string, std::vector<ss::ServiceInfo>>;
// key = 服务名（如 "LogicService"）→ 按组路由（由 LB 策略选实例）
// 内元素为 ServiceInstances（map<id, ServiceInstancePtr>）
using ServiceNameInstances = std::unordered_map<std::string, ServiceInstances>;


class IBusNet
{
public:
    virtual ~IBusNet();

    virtual void init(std::shared_ptr<Options> opts);

    virtual void broadCast(const AppMsgWrapper &msg) = 0;

    virtual bool sendMsgTo(const std::string_view &serviceName, const AppMsgWrapper &msg,
                           LBStrategy strategy = LBStrategy::RoundRobin);

    virtual bool sendMsgToGroup(const std::string_view &groupName, const AppMsgWrapper &msg) = 0;

    std::shared_ptr<ServiceMap> getServiceMap() const;

    // 生成当前服务信息（需要子类实现以设置is_daemon标志）
    virtual void genServiceInfo();

protected:
    void sendMsgToCenter(const google::protobuf::Message &msg);

    void onRecvMsg(const AppMsg &msg);

    virtual bool sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg = true) = 0;

    // 向center注册
    void regist2Center();

    // center注册的回调函数
    void onCenterRegistRsp(const AppMsg &msg);

    // 更新服务状态
    void updateServiceStatusReq();
    // 更新服务状态回调函数（返回完整服务表）
    void onCenterUpdateServiceStatusRsp(const AppMsg &msg);

protected:
    // CRITICAL FIX #1: Change raw pointers to smart pointers for RAII
    // Local process SendBuffer, corresponding to local machine Busd's ReadBuffer
    std::unique_ptr<ShmRingBuffer<AppMsgWrapper>> LocalBusdShmBuffer_;

    // Full service table
    std::shared_ptr<ServiceMap> ServiceMap_;

    // 服务名实例表（key=svr_name → ServiceInstances，供 LB 策略选实例）
    std::shared_ptr<ServiceNameInstances> NameInstances_;
    LBFactory lb_factory_;

    // Local machine's all service peer shared memory channels map<service_name, ShmRingBuffer>
    std::unordered_map<std::string, std::unique_ptr<ShmRingBuffer<AppMsgWrapper>>> LocalServiceMap_;

    std::unique_ptr<TcpClient> TcpClient_;

    bool ready_ = false;
    bool has_center_ = false;
    std::unordered_map<uint32_t, CenterMessageHandler> CenterMessageHandlers_;
    std::shared_ptr<Options> opts_;
    ss::ServiceInfo local_service_info_;
};