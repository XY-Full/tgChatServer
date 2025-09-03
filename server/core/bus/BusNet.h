#include "../../../common/Helper.h"
#include "../network/TcpClient.h"
#include "IBus.h"
#include <memory>
#include <unordered_map>

#define EXTRACT_APPMSG_WRAPPER(msg) (reinterpret_cast<AppMsg *>(msg.offset_))

namespace ss
{
class ServiceInfo;
}

using CenterMessageHandler = std::function<void(const AppMsg &)>;
using ServiceMap = std::unordered_map<std::string, std::vector<ss::ServiceInfo>>;

class BusNet
{
public:
    BusNet();
    ~BusNet();

    void init(std::string shm_name);

    void broadCast(const AppMsgWrapper &msg);

    void sendMsgTo(const std::string& serviceName, const AppMsgWrapper &msg);

    void sendMsgToGroup(const std::string& groupName, const AppMsgWrapper &msg);

    std::shared_ptr<ServiceMap> GetServiceMap() const;

    // 生成当前服务信息
    std::shared_ptr<ss::ServiceInfo> genServiceInfo();

    void sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg);

private:
    void sendMsgToCenter(const google::protobuf::Message &msg);

    void genRouteCache(const std::string& serviceName);

    void onRecvMsg(const AppMsg &msg);

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
    std::unordered_map<std::string, ss::ServiceInfo> RouteCache_;

    // 本机的所有服务对端共享内存通道 map<服务名, ShmRingBuffer>
    std::unordered_map<std::string, ShmRingBuffer<AppMsgWrapper> *> LocalServiceMap_;

    std::unique_ptr<TcpClient> TcpClient_;

    bool ready_ = false;
    std::unordered_map<uint32_t, CenterMessageHandler> CenterMessageHandlers_;
};