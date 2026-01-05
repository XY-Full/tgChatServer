#pragma once
#include "IBusNet.h"
#include "network/TcpClient.h"
#include <atomic>
#include <memory>
#include <thread>
#include <unordered_map>

class BusdNet : public IBusNet
{
public:
    BusdNet() = default;
    ~BusdNet();

    virtual void init(std::shared_ptr<Options> opts) override;
    
    virtual void genServiceInfo() override;
    
    virtual bool sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg = true) override;
    
    virtual void broadCast(const AppMsgWrapper &msg) override;
    
    virtual bool sendMsgToGroup(const std::string_view &groupName, const AppMsgWrapper &msg) override;

private:
    // 接收来自远程busd的消息并转发到本地服务
    void onRecvFromRemoteDaemon(uint64_t conn_id, std::shared_ptr<PackBase> pack);

    // 消息转发循环（从LocalBusdShmBuffer_读取并转发）
    void messageForwardLoop();

    // 到远程busd的TCP连接映射 map<"ip:port", TcpClient>
    std::unordered_map<std::string, std::unique_ptr<TcpClient>> RemoteDaemonConnMap_;

    // 转发线程
    std::thread forwarder_thread_;
    std::atomic<bool> running_{false};

    // 负载均衡计数器（简单轮询）
    std::atomic<size_t> load_balance_counter_{0};
};
