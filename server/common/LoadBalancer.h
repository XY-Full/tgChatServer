#pragma once

#include "ServiceInstance.h"
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

// ── 负载均衡策略枚举 ──────────────────────────────────────────────────────
enum class LBStrategy
{
    RoundRobin,     // 轮询（默认）
    WeightedRR,     // 加权轮询（按 weight 字段）
    LeastConn,      // 最小连接数
    ConsHash,       // 一致性哈希（需传 key）
    SmoothWeighted, // 平滑加权轮询（Nginx 算法）
    LatencyAware,   // 最低延迟优先
    WeightedLoad,   // 按 load_score 加权随机（高负载权重大，冷热分离）
    LeastLoad,      // 最低 load_score 优先（标准负载均衡）
    MostLoad,       // 最高 load_score 优先（精准打满热节点）
};

class ILoadBalancer
{
public:
    virtual ~ILoadBalancer() = default;
    virtual ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") = 0;
};

class LBFactory
{
public:
    LBFactory();
    using Creator = std::function<std::shared_ptr<ILoadBalancer>()>;

    // 按枚举创建（推荐）
    std::shared_ptr<ILoadBalancer> create(LBStrategy strategy);

    // 按字符串创建（兼容旧接口，未知名称退回 RoundRobin）
    std::shared_ptr<ILoadBalancer> create(const std::string &name);

    void register_factory(const std::string &name, Creator c);

private:
    std::unordered_map<std::string, Creator> factory_;
};
