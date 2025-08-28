#pragma once

#include "ServiceInstance.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <functional>

class ILoadBalancer {
public:
    virtual ~ILoadBalancer() = default;
    virtual ServiceInstancePtr select(const ServiceInstances& instances, const std::string& key = "") = 0;
};

class LBFactory {
public:
    LBFactory();
    using Creator = std::function<std::shared_ptr<ILoadBalancer>()>;
    void register_factory(const std::string& name, Creator c);
    std::shared_ptr<ILoadBalancer> create(const std::string& name);
private:
    std::unordered_map<std::string, Creator> factory_;
};