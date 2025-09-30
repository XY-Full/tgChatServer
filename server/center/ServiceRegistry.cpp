#include "ServiceRegistry.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <mutex>

void ServiceRegistry::register_instance(const ServiceInstancePtr &inst, std::chrono::seconds ttl)
{
    std::unique_lock lock(mu_);
    auto &m = registry_[inst->svc_name];

    auto it = m.find(inst->id);
    if (it != m.end())
    {
        // 替换已有实例
        it->second = inst;
        inst->last_seen = std::chrono::steady_clock::now();
    }
    else
    {
        inst->last_seen = std::chrono::steady_clock::now();
        m[inst->id] = inst;
    }

    expirations_[inst->id] = std::chrono::steady_clock::now() + ttl;
}

void ServiceRegistry::deregister_instance(const std::string &svc_name, const std::string &id)
{
    std::unique_lock lock(mu_);
    auto it = registry_.find(svc_name);
    if (it == registry_.end())
        return;

    auto &m = it->second;
    m.erase(id);
    expirations_.erase(id);

    if (m.empty())
        registry_.erase(it);
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
        for (auto it = registry_.begin(); it != registry_.end(); )
        {
            auto &m = it->second;
            
            for (auto mit = m.begin(); mit != m.end(); )
            {
                if (mit->second->id == id)
                    mit = m.erase(mit);
                else
                    ++mit;
            }

            if (m.empty())
                it = registry_.erase(it);
            else
                ++it;
        }

        expirations_.erase(id);
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