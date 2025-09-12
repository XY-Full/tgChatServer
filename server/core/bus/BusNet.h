#include "Helper.h"
#include "network/TcpClient.h"
#include "IBus.h"
#include "ss_base.pb.h"
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
    std::string local_port = "3089";
    std::string center_ip = "";
    std::string center_port = "";
    // =======配置参数=======

    std::string client_id = "";

    Options(const ConfigManager& config_manager)
    {
        region_id = config_manager.getValue<uint8_t>("region_id", 0);
        zone_id = config_manager.getValue<uint8_t>("zone_id", 0);
        service_id = config_manager.getValue<uint8_t>("service_id", 0);
        instance_id = config_manager.getValue<uint8_t>("instance_id", 0);

        local_ring_size = config_manager.getValue<size_t>("local_ring_size", 1 << 20);

        client_id = std::to_string(region_id) + "." + std::to_string(zone_id) + "." + std::to_string(service_id) + "." + std::to_string(instance_id);
    }
};


namespace ss
{
class ServiceInfo;
}
class BusClient;

using CenterMessageHandler = std::function<void(const AppMsg &)>;
using ServiceMap = std::unordered_map<std::string_view, std::vector<ss::ServiceInfo>>;

struct ServiceRouteCache
{
    uint64_t delay = 0;
    ss::ServiceInfo info;
};

class BusNet
{
public:
    ~BusNet();

    void init(std::shared_ptr<Options> opts);

    void broadCast(const AppMsgWrapper &msg);

    bool sendMsgTo(const std::string_view &serviceName, const AppMsgWrapper &msg);

    bool sendMsgToGroup(const std::string_view& groupName, const AppMsgWrapper &msg);

    std::shared_ptr<ServiceMap> GetServiceMap() const;

    // 生成当前服务信息
    std::shared_ptr<ss::ServiceInfo> genServiceInfo();

    // 接受路由缓存包
    void onRecvRouteCache(AppMsgPtr msg);
    
private:
    void sendMsgToCenter(const google::protobuf::Message &msg);

    // 生成路由缓存
    void genRouteCache(const std::string_view& serviceName);

    void onRecvMsg(const AppMsg &msg);

    bool sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg = true);

    // 向center注册
    void regist2Center();
    // 注册回调函数
    void onCenterRegistResp(const AppMsg &msg);

    // 更新服务状态
    void UpdateServiceStatusReq();
    // 更新服务状态回调函数（返回完整服务表）
    void onCenterUpdateServiceStatusResp(const AppMsg &msg);

private:
    // 本进程的SendBuffer，对应为本机器Busd的ReadBuffer
    ShmRingBuffer<AppMsgWrapper> *LocalBusdShmBuffer_;

    // 全服务表
    std::shared_ptr<ServiceMap> ServiceMap_;

    // 路由缓存
    std::unordered_map<std::string_view, std::shared_ptr<ServiceRouteCache>> RouteCache_;

    // 本机的所有服务对端共享内存通道 map<服务名, ShmRingBuffer>
    std::unordered_map<std::string, ShmRingBuffer<AppMsgWrapper> *> LocalServiceMap_;

    std::unique_ptr<TcpClient> TcpClient_;

    bool ready_ = false;
    bool has_center_ = false;
    std::unordered_map<uint32_t, CenterMessageHandler> CenterMessageHandlers_;
    std::shared_ptr<Options> opts_;
};