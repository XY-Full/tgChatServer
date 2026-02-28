#pragma once

#include "ServiceInstance.h"
#include <chrono>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

// 差量变更条目：记录一次服务上线或下线事件
struct DeltaEntry
{
    enum class Op { UPSERT, OFFLINE };

    Op            op;
    ServiceInstancePtr inst;  // UPSERT: 新/更新的实例；OFFLINE: 下线的实例快照
};

class ServiceRegistry
{
public:
    ServiceRegistry() = default;

    // 注册/续约实例。
    // is_new_instance 由调用方传入：true 表示是全新注册（触发差量推送），
    // false 表示仅续约 TTL（不触发差量推送）。
    void register_instance(const ServiceInstancePtr &inst,
                           std::chrono::seconds ttl = std::chrono::seconds(30),
                           bool is_new_instance = false);

    void deregister_instance(const std::string &svc_name, const std::string &id);
    ServiceInstances get_instances(const std::string &svc_name, bool only_healthy = true);
    std::unordered_map<std::string, ServiceInstances> snapshot();
    void cleanup_expired();
    ServiceInstancePtr get_by_id(const std::string &svc_name, const std::string &id);
    std::string routing_table_string();

    // ---------- delta map 接口 ----------

    // 为新订阅者分配 delta 槽位（首次注册时调用）
    void subscribe(const std::string &subscriber_id);

    // 订阅者断开时释放槽位
    void unsubscribe(const std::string &subscriber_id);

    // 取出并清空指定订阅者的所有待消费差量（TcpRegistrar 在每次请求时调用）
    std::vector<DeltaEntry> pop_deltas(const std::string &subscriber_id);

private:
    // 向除 changed_id 之外的所有已订阅实例推送一条差量
    void push_delta_to_others(const std::string &changed_id, DeltaEntry entry);

    std::unordered_map<std::string, ServiceInstances> registry_;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> expirations_;

    // delta_map_: subscriber_id -> 待推送给该订阅者的变更队列
    std::unordered_map<std::string, std::vector<DeltaEntry>> delta_map_;

    mutable std::shared_mutex mu_;
};
