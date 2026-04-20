#include "LoadBalancer.h"
#include <atomic>
#include <limits>
#include <mutex>
#include <random>

// 轮询
class RoundRobinLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        if (instances.empty())
            return nullptr;

        // 如果调用方传了 key，优先按 key 找
        if (!key.empty()) {
            auto it = instances.find(key);
            if (it != instances.end())
                return it->second;
            return nullptr; // key 不存在
        }

        // 否则走轮询逻辑
        auto idx = idx_.fetch_add(1);
        auto it = instances.begin();
        std::advance(it, idx % instances.size());  // 把迭代器往前走 N 步
        return it->second;
    }

private:
    std::atomic<uint64_t> idx_{0};
};

// 简单加权轮询（构造轮盘）
class WeightedRRLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        if (instances.empty())
            return nullptr;

        // 构造轮盘
        std::vector<ServiceInstancePtr> wheel;
        for (auto &kv : instances)
        {
            auto &instance = kv.second;
            uint32_t w = std::max<uint32_t>(1, instance->weight);
            for (uint32_t i = 0; i < w; i++)
                wheel.push_back(instance);
        }

        if (wheel.empty())
            return nullptr;

        // 轮询索引
        auto i = idx_.fetch_add(1, std::memory_order_relaxed);
        return wheel[i % wheel.size()];
    }

private:
    std::atomic<uint64_t> idx_{0};
};

// 最小连接数
class LeastConnLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        if (instances.empty())
            return nullptr;
        ServiceInstancePtr best = nullptr;
        uint64_t best_conn = std::numeric_limits<uint64_t>::max();
        for (auto &p : instances)
        {
            uint64_t c = p.second->connections;
            if (!best || c < best_conn)
            {
                best = p.second;
                best_conn = c;
            }
        }
        return best;
    }
};

// 简单一致性哈希
class ConsistentHashLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        if (instances.empty())
            return nullptr;

        // key 为空则退化为轮询
        if (key.empty())
            return RoundRobinLB().select(instances);

        // 计算哈希
        size_t h = std::hash<std::string>{}(key);
        size_t idx = h % instances.size();

        // 迭代到对应位置
        auto it = instances.begin();
        std::advance(it, idx);
        return it->second;
    }
};

// 平滑加权轮询（Smooth Weighted Round-Robin）
// 算法参考：Nginx 的 smooth weighted round robin
class SmoothWeightedRRLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        std::unique_lock lock(mu_);
        if (instances.empty())
            return nullptr;

        uint64_t total = 0;
        std::unordered_map<std::string, bool> alive;

        // 更新节点集合（新增或修改权重）
        for (auto &kv : instances)
        {
            auto &p = kv.second;
            auto it = nodes_.find(p->id);
            if (it == nodes_.end())
            {
                Node n;
                n.weight = std::max<uint32_t>(1, p->weight);
                n.current = 0;
                n.effective_weight = n.weight;
                nodes_[p->id] = n;
            }
            else
            {
                it->second.weight = std::max<uint32_t>(1, p->weight);
                // 保留 current 与 effective_weight
            }
            total += nodes_[p->id].effective_weight;
            alive[p->id] = true;
        }

        // 移除不再存在的节点
        for (auto it = nodes_.begin(); it != nodes_.end();)
        {
            if (alive.find(it->first) == alive.end())
                it = nodes_.erase(it);
            else
                ++it;
        }

        // 两步选择（Nginx 平滑加权轮询）：
        // 第一步：所有节点 current += effective_weight
        for (auto &kv : instances)
        {
            nodes_[kv.second->id].current += nodes_[kv.second->id].effective_weight;
        }

        // 第二步：找 current 最大的节点（与遍历顺序无关）
        std::string best_id;
        int64_t best_current = std::numeric_limits<int64_t>::min();
        for (auto &kv : nodes_)
        {
            if (kv.second.current > best_current)
            {
                best_current = kv.second.current;
                best_id = kv.first;
            }
        }

        if (best_id.empty())
            return nullptr;

        nodes_[best_id].current -= total;

        // 返回对应实例指针
        auto it = instances.find(best_id);
        if (it != instances.end())
            return it->second;

        return nullptr;
    }

private:
    struct Node
    {
        uint32_t weight;
        int64_t current;          // 有符号：被选中后 current -= total，可为负
        uint32_t effective_weight;
    };
    std::mutex mu_;
    std::unordered_map<std::string, Node> nodes_;
};

// 延迟感知 LB：选择 avg_latency_us 最小的实例；若 avg_latency_us 为 0（未知），视为大值
class LatencyAwareLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        if (instances.empty())
            return nullptr;
        ServiceInstancePtr best = nullptr;
        uint64_t best_lat = UINT64_MAX;
        for (auto &p : instances)
        {
            uint64_t lat = p.second->avg_latency_us;
            if (lat == 0)
                lat = UINT64_MAX / 2; // 未知延迟视为很高
            if (!best || lat < best_lat)
            {
                best = p.second;
                best_lat = lat;
            }
        }
        return best;
    }
};

// 按 load_score 加权随机路由（高负载权重大 = 冷热分离：优先打满热节点）
// load_score 为 0 时赋最小权重 1，避免饥饿
class WeightedLoadLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        if (instances.empty())
            return nullptr;

        // 构造权重轮盘：权重 = load_score，为 0 则用 1
        std::vector<std::pair<ServiceInstancePtr, uint32_t>> wheel;
        uint64_t total = 0;
        for (auto &kv : instances)
        {
            uint32_t w = kv.second->load_score > 0 ? kv.second->load_score : 1u;
            wheel.push_back({kv.second, w});
            total += w;
        }

        if (total == 0)
            return instances.begin()->second;

        // 线程安全的随机数（每次调用独立引擎，避免锁）
        thread_local std::mt19937_64 rng{std::random_device{}()};
        std::uniform_int_distribution<uint64_t> dist(0, total - 1);
        uint64_t roll = dist(rng);

        uint64_t acc = 0;
        for (auto &p : wheel)
        {
            acc += p.second;
            if (roll < acc)
                return p.first;
        }
        return wheel.back().first;
    }
};

// 最低负载路由（load_score 最小者优先，即负载最轻的节点）
class LeastLoadLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        if (instances.empty())
            return nullptr;
        ServiceInstancePtr best = nullptr;
        uint32_t best_score = std::numeric_limits<uint32_t>::max();
        for (auto &p : instances)
        {
            if (!best || p.second->load_score < best_score)
            {
                best = p.second;
                best_score = p.second->load_score;
            }
        }
        return best;
    }
};

// 最高负载路由（load_score 最大者优先，精准打满最热节点）
class MostLoadLB : public ILoadBalancer
{
public:
    ServiceInstancePtr select(const ServiceInstances &instances, const std::string &key = "") override
    {
        if (instances.empty())
            return nullptr;
        ServiceInstancePtr best = nullptr;
        uint32_t best_score = 0;
        for (auto &p : instances)
        {
            if (!best || p.second->load_score > best_score)
            {
                best = p.second;
                best_score = p.second->load_score;
            }
        }
        return best;
    }
};

// LBFactory
LBFactory::LBFactory()
{
    register_factory("round_robin",    []() { return std::make_shared<RoundRobinLB>(); });
    register_factory("weighted_rr",    []() { return std::make_shared<WeightedRRLB>(); });
    register_factory("least_conn",     []() { return std::make_shared<LeastConnLB>(); });
    register_factory("cons_hash",      []() { return std::make_shared<ConsistentHashLB>(); });
    register_factory("smooth_weighted",[]() { return std::make_shared<SmoothWeightedRRLB>(); });
    register_factory("latency_aware",  []() { return std::make_shared<LatencyAwareLB>(); });
    // 负载感知策略
    register_factory("weighted_load",  []() { return std::make_shared<WeightedLoadLB>(); });  // 高负载权重大（冷热分离）
    register_factory("least_load",     []() { return std::make_shared<LeastLoadLB>(); });     // 最低负载优先
    register_factory("most_load",      []() { return std::make_shared<MostLoadLB>(); });      // 最高负载优先（精准打满）
}

void LBFactory::register_factory(const std::string &name, Creator c)
{
    factory_[name] = c;
}

std::shared_ptr<ILoadBalancer> LBFactory::create(const std::string &name)
{
    auto it = factory_.find(name);
    if (it == factory_.end())
        return factory_["round_robin"]();
    return it->second();
}

std::shared_ptr<ILoadBalancer> LBFactory::create(LBStrategy strategy)
{
    switch (strategy)
    {
    case LBStrategy::RoundRobin:      return factory_["round_robin"]();
    case LBStrategy::WeightedRR:      return factory_["weighted_rr"]();
    case LBStrategy::LeastConn:       return factory_["least_conn"]();
    case LBStrategy::ConsHash:        return factory_["cons_hash"]();
    case LBStrategy::SmoothWeighted:  return factory_["smooth_weighted"]();
    case LBStrategy::LatencyAware:    return factory_["latency_aware"]();
    case LBStrategy::WeightedLoad:    return factory_["weighted_load"]();
    case LBStrategy::LeastLoad:       return factory_["least_load"]();
    case LBStrategy::MostLoad:        return factory_["most_load"]();
    default:                          return factory_["round_robin"]();
    }
}