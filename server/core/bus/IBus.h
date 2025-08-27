// IBus.h
#ifndef IBUS_CLIENT_H
#define IBUS_CLIENT_H

#include "IBusCommon.h"
#include "../shm/shm_ringbuffer.h"
#include "../shm/shm_hashmap.h"
#include "../shm/shm_slab.h"
#include "../../common/Log.h"

#include <memory>
#include <string>
#include <functional>
#include <vector>
#include <chrono>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

namespace IBus {

class BusClient {
public:
    struct Options {
        std::string client_id;
        std::string busd_addr = "127.0.0.1:5555";
        size_t local_ring_size = 1 << 20;   // 1MB
        size_t max_remote_msg = 1 << 16;    // 64KB
        bool enable_trace = false;
        std::chrono::milliseconds reconnect_interval{100};
    };

    using MessageHandler = std::function<void(const Message&)>;

    explicit BusClient(const Options& opts);
    ~BusClient();

    // 禁止拷贝，允许移动
    BusClient(const BusClient&) = delete;
    BusClient& operator=(const BusClient&) = delete;
    BusClient(BusClient&&) noexcept;
    BusClient& operator=(BusClient&&) noexcept;

    // 生命周期
    bool Start();
    void Stop();
    bool WaitReady(std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // 发布/订阅
    bool Publish(const std::string& topic, const void* data, size_t len,
                 Message::Flags flags = Message::Flags::NONE);
    bool Subscribe(const std::string& topic, const MessageHandler& handler);
    bool Unsubscribe(const std::string& topic);

    // 请求/响应
    using ResponseHandler = std::function<void(const Message&, ErrorCode)>;
    uint64_t Request(const std::string& topic, const void* data, size_t len,
                     const ResponseHandler& handler,
                     std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool Reply(uint64_t req_id, const void* data, size_t len);

    // 管理接口
    Stats GetStats() const;
    void SetLogLevel(LogLevel level);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace IBus

#endif // IBUS_CLIENT_H