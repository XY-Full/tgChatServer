#include "ServiceRegistry.h"
#include <mutex>
#include <set>
#include <shared_mutex>
#include <sstream>

void ServiceRegistry::register_instance(const ServiceInstancePtr &inst,
                                         std::chrono::seconds ttl,
                                         bool is_new_instance)
{
    std::unique_lock lock(mu_);
    auto &m = registry_[inst->svc_name];

    auto it = m.find(inst->id);
    if (it != m.end())
    {
        // 已有实例：仅更新 last_seen 和 expiration，不触发差量
        it->second = inst;
        inst->last_seen = std::chrono::steady_clock::now();
    }
    else
    {
        inst->last_seen = std::chrono::steady_clock::now();
        m[inst->id] = inst;
    }

    expirations_[inst->id] = std::chrono::steady_clock::now() + ttl;

    // 只有真正新注册才通知其他订阅者
    if (is_new_instance)
    {
        DeltaEntry entry{DeltaEntry::Op::UPSERT, inst};
        push_delta_to_others(inst->id, std::move(entry));
    }
}

void ServiceRegistry::deregister_instance(const std::string &svc_name, const std::string &id)
{
    std::unique_lock lock(mu_);
    auto it = registry_.find(svc_name);
    if (it == registry_.end())
        return;

    auto &m = it->second;
    auto mit = m.find(id);
    if (mit == m.end())
        return;

    // 保留实例快照用于差量推送
    ServiceInstancePtr offline_inst = mit->second;

    m.erase(mit);
    expirations_.erase(id);

    if (m.empty())
        registry_.erase(it);

    // 通知其他订阅者该实例下线
    DeltaEntry entry{DeltaEntry::Op::OFFLINE, offline_inst};
    push_delta_to_others(id, std::move(entry));
}

ServiceInstances ServiceRegistry::get_instances(const std::string &svc_name, bool only_healthy)
{
    std::shared_lock lock(mu_);
    ServiceInstances out;

    auto it = registry_.find(svc_name);
    if (it == registry_.end())
        return out;

    for (auto &kv : it->second)
    {
        auto &inst = kv.second;
        if (!only_healthy || inst->healthy)
            out[kv.first] = kv.second;
    }

    return out;
}

std::unordered_map<std::string, ServiceInstances> ServiceRegistry::snapshot()
{
    std::shared_lock lock(mu_);
    return registry_;
}

void ServiceRegistry::cleanup_expired()
{
    std::unique_lock lock(mu_);
    auto now = std::chrono::steady_clock::now();

    std::set<std::string> remove_ids;
    for (auto &kv : expirations_)
    {
        if (kv.second < now)
            remove_ids.insert(kv.first);
    }

    if (remove_ids.empty())
        return;

    for (auto &id : remove_ids)
    {
        ServiceInstancePtr offline_inst;

        for (auto it = registry_.begin(); it != registry_.end(); )
        {
            auto &m = it->second;
            for (auto mit = m.begin(); mit != m.end(); )
            {
                if (mit->second->id == id)
                {
                    offline_inst = mit->second;
                    mit = m.erase(mit);
                }
                else
                    ++mit;
            }
            if (m.empty())
                it = registry_.erase(it);
            else
                ++it;
        }

        expirations_.erase(id);

        // 通知其他订阅者该过期实例下线
        if (offline_inst)
        {
            DeltaEntry entry{DeltaEntry::Op::OFFLINE, offline_inst};
            push_delta_to_others(id, std::move(entry));
        }
    }
}

ServiceInstancePtr ServiceRegistry::get_by_id(const std::string &svc_name, const std::string &id)
{
    std::shared_lock lock(mu_);
    auto it = registry_.find(svc_name);
    if (it == registry_.end())
        return nullptr;
    for (auto &p : it->second)
        if (p.second->id == id)
            return p.second;
    return nullptr;
}

std::string ServiceRegistry::routing_table_string()
{
    std::shared_lock lock(mu_);
    std::ostringstream ss;
    ss << "Routing table: ";
    for (auto &kv : registry_)
    {
        ss << "Service: " << kv.first << " ";
        for (auto &p : kv.second)
            ss << "  - " << p.second->to_string() << " ";
    }
    return ss.str();
}

// ---------- delta map 接口实现 ----------

void ServiceRegistry::subscribe(const std::string &subscriber_id)
{
    std::unique_lock lock(mu_);
    // 若已存在则不覆盖（避免重连时丢失积压的差量）
    delta_map_.emplace(subscriber_id, std::vector<DeltaEntry>{});
}

void ServiceRegistry::unsubscribe(const std::string &subscriber_id)
{
    std::unique_lock lock(mu_);
    delta_map_.erase(subscriber_id);
}

std::vector<DeltaEntry> ServiceRegistry::pop_deltas(const std::string &subscriber_id)
{
    std::unique_lock lock(mu_);
    auto it = delta_map_.find(subscriber_id);
    if (it == delta_map_.end())
        return {};
    std::vector<DeltaEntry> out;
    out.swap(it->second);  // O(1) 取出并清空
    return out;
}

void ServiceRegistry::push_delta_to_others(const std::string &changed_id, DeltaEntry entry)
{
    // 调用前已持有 unique_lock，直接操作 delta_map_
    for (auto &kv : delta_map_)
    {
        if (kv.first == changed_id)
            continue;  // 不推送给变更者自己
        kv.second.push_back(entry);
    }
}
