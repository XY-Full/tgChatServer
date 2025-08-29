// IBus.h
#ifndef IBUS_CLIENT_H
#define IBUS_CLIENT_H

#include "../../common/Log.h"
#include "../shm/shm_hashmap.h"
#include "../shm/shm_ringbuffer.h"
#include "../shm/shm_slab.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <google/protobuf/message.h>
#include <google/protobuf/descriptor.h>
#include "../network/AppMsg.h"

namespace IBus
{

// 回调函数类型
using MessageHandler = std::function<void(const google::protobuf::Message&)>;
using ResponseHandler = std::function<void(const google::protobuf::Message&)>;

class BusClient
{
public:
    struct Options
    {
        std::string client_id;
        std::string busd_addr = "127.0.0.1:5555";
        size_t local_ring_size = 1 << 20; // 1MB
        size_t max_remote_msg = 1 << 16;  // 64KB
        bool enable_trace = false;
        std::chrono::milliseconds reconnect_interval{100};
    };

    explicit BusClient(const Options &opts);
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

    // 发布/订阅
    bool Publish(PackBase& pack);
    bool Subscribe(const std::string &topic, const MessageHandler &handler);
    bool Unsubscribe(const std::string &topic);

    // 请求/响应
    uint64_t Request(const std::string &topic, const void *data, size_t len, const ResponseHandler &handler,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool Reply(uint64_t req_id, const void *data, size_t len);

    // 管理接口
    void GetStats() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    static uint64_t now_seq_;
};

} // namespace IBus

#endif // IBUS_CLIENT_H