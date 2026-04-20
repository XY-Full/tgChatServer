// IBus.h
#ifndef IBUS_CLIENT_H
#define IBUS_CLIENT_H


#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include "MsgDispatcher.h"
#include "app/ConfigManager.h"
#include "LoadBalancer.h"

class PackBase;
class AppMsg;
class AppMsgWrapper;
class BusNet;

using AppMsgPtr = std::shared_ptr<AppMsg>;

namespace IBus
{

// 回调函数类型
using ResponseHandler = std::function<void(const AppMsg&)>;

class BusClient
{
public:
    BusClient() = delete;
    explicit BusClient(const ConfigManager& config_manager, bool is_daemon = false);
    ~BusClient();

    // 禁止拷贝，允许移动
    BusClient(const BusClient &) = delete;
    BusClient &operator=(const BusClient &) = delete;
    BusClient(BusClient &&) noexcept = delete;
    BusClient &operator=(BusClient &&) noexcept = delete;

    // 生命周期
    bool Start();
    void Stop();
    bool WaitReady(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    
    bool SendToNode(const std::string& service_name, const google::protobuf::Message &message);          // 发送消息给某个node
    bool RegistMessage(uint32_t msg_id, const MessageHandler &handler);   // 注册事件
    bool UnregistMessage(uint32_t msg_id);

    // 请求/响应
    AppMsgPtr Request(const std::string& service_name, const google::protobuf::Message &message);
    bool Reply(const AppMsg& req_msg, const google::protobuf::Message &msg);

    /**
     * @brief 将已有 AppMsg 原样透传到目标服务，不做 proto 序列化。
     *        用于 connd 将客户端 CS 消息转发给 logic / account，
     *        以及将下行回包转发给客户端 listener。
     */
    bool ForwardRawAppMsg(const std::string& service_name, const AppMsg& msg,
                          LBStrategy strategy = LBStrategy::RoundRobin);

    /**
     * @brief 精准路由到指定实例 ID（如 "0.1.3.0"），用于需要指定某个具体服务实例的场景。
     *        与 ForwardRawAppMsg 的区别：service_name 走 svr_name 轮询，instance_id 走精准路由。
     */
    bool ForwardRawAppMsgToId(const std::string& instance_id, const AppMsg& msg);

    // 管理接口
    void GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    static uint64_t now_seq_;
};

} // namespace IBus

#endif // IBUS_CLIENT_H