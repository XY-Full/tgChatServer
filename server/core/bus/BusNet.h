#include "../../../common/Helper.h"
#include "IBus.h"
#include <unordered_map>
#include "../network/TcpClient.h"

using CenterMessageHandler = std::function<void(const AppMsg&)>;

class BusNet
{
public:
    BusNet();
    ~BusNet();

    void init();

    void broadCast();

    void sendMsgTo(std::string serviceName);

    void sendMsgToGroup(std::string groupName);

    void onRecvMsg(const AppMsg& msg);

    void regist2Center();

    void onCenterRegistResp(const AppMsg& msg);

private:
    // 本进程的SendBuffer，对应为本机器Busd的ReadBuffer
    ShmRingBuffer<uint32_t>* Send2RemoteBuffer_;

    // map<服务组名称, vector<pair{服务名, ip}>>
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> ServiceMap_;

    // 路由缓存 map<服务组名称, pair{服务名, ip}>
    std::unordered_map<std::string, std::pair<std::string, std::string>> RouteCache_;

    // 本机的所有服务对端共享内存通道 map<服务名, ShmRingBuffer>
    std::unordered_map<std::string, ShmRingBuffer<uint32_t>*> LocalServiceMap_;

    std::unique_ptr<TcpClient> TcpClient_;

    bool ready_ = false;
    std::unordered_map<uint32_t, CenterMessageHandler> CenterMessageHandlers_;
};