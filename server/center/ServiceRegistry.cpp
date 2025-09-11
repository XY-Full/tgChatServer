#include "ServiceRegistry.h"
#include <algorithm>
#include <set>
#include <sstream>

void ServiceRegistry::register_instance(const ServiceInstancePtr &inst, std::chrono::seconds ttl)
{
    std::unique_lock lock(mu_);
    auto &vec = registry_[inst->svc_name];
    bool replaced = false;
    for (auto &e : vec)
    {
        if (e->id == inst->id)
        {
            *e = *inst;
            e->last_seen = std::chrono::steady_clock::now();
            replaced = true;
            break;
        }
    }
    if (!replaced)
    {
        inst->last_seen = std::chrono::steady_clock::now();
        vec.push_back(inst);
    }
    expirations_[inst->id] = std::chrono::steady_clock::now() + ttl;
}

void ServiceRegistry::deregister_instance(const std::string &svc_name, const std::string &id)
{
    std::unique_lock lock(mu_);
    auto it = registry_.find(svc_name);
    if (it == registry_.end())
        return;
    auto &vec = it->second;
    vec.erase(std::remove_if(vec.begin(), vec.end(), [&](auto &p) { return p->id == id; }), vec.end());
    expirations_.erase(id);
    if (vec.empty())
        registry_.erase(it);
}

ServiceInstances ServiceRegistry::get_instances(const std::string &svc_name, bool only_healthy)
{
    std::shared_lock lock(mu_);
    ServiceInstances out;
    auto it = registry_.find(svc_name);
    if (it == registry_.end())
        return out;
    for (auto &p : it->second)
    {
        if (!only_healthy || p->healthy.load())
            out.push_back(p);
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
        for (auto it = registry_.begin(); it != registry_.end();)
        {
            auto &vec = it->second;
            vec.erase(std::remove_if(vec.begin(), vec.end(), [&](auto &p) { return p->id == id; }), vec.end());
            if (vec.empty())
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
        if (p->id == id)
            return p;
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
            ss << "  - " << p->to_string() << " ";
    }
    return ss.str();
}