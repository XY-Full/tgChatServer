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
#include "jsonParser/ConfigManager.h"

class PackBase;
class AppMsg;
class AppMsgWrapper;
class BusNet;

namespace IBus
{

// 回调函数类型
using ResponseHandler = std::function<void(const AppMsg&)>;

class BusClient
{
public:
    explicit BusClient(const ConfigManager& config_manager);
    ~BusClient();

    // 禁止拷贝，允许移动
    BusClient(const BusClient &) = delete;
    BusClient &operator=(const BusClient &) = delete;
    BusClient(BusClient &&) noexcept;
    BusClient &operator=(BusClient &&) noexcept;

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

    // 管理接口
    void GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    static uint64_t now_seq_;
};

} // namespace IBus

#endif // IBUS_CLIENT_H