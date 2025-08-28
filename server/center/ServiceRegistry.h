#pragma once


#include "ServiceInstance.h"
#include <chrono>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>


class ServiceRegistry {
public:
ServiceRegistry() = default;
void register_instance(const ServiceInstancePtr& inst, std::chrono::seconds ttl = std::chrono::seconds(30));
void deregister_instance(const std::string& svc_name, const std::string& id);
ServiceInstances get_instances(const std::string& svc_name, bool only_healthy = true);
std::unordered_map<std::string, ServiceInstances> snapshot();
void cleanup_expired();
ServiceInstancePtr get_by_id(const std::string& svc_name, const std::string& id);
std::string routing_table_string();
private:
std::unordered_map<std::string, ServiceInstances> registry_;
std::unordered_map<std::string, std::chrono::steady_clock::time_point> expirations_;
mutable std::shared_mutex mu_;
};