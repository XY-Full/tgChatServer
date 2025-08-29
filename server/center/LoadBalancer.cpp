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
        auto idx = idx_.fetch_add(1);
        return instances[idx % instances.size()];
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
        std::vector<ServiceInstancePtr> wheel;
        for (auto &p : instances)
        {
            uint32_t w = std::max<uint32_t>(1, p->weight);
            for (uint32_t i = 0; i < w; i++)
                wheel.push_back(p);
        }
        if (wheel.empty())
            return nullptr;
        auto i = idx_.fetch_add(1);
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
            uint64_t c = p->connections.load();
            if (!best || c < best_conn)
            {
                best = p;
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
        if (key.empty())
            return RoundRobinLB().select(instances);
        size_t h = std::hash<std::string>{}(key);
        return instances[h % instances.size()];
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

        // 更新节点集合（新增或移除）
        // 使用实例 id 作为 key
        std::unordered_map<std::string, Node> &nodes = nodes_;
        uint64_t total = 0;
        // 标记活跃节点
        std::unordered_map<std::string, bool> alive;
        for (auto &p : instances)
        {
            auto it = nodes.find(p->id);
            if (it == nodes.end())
            {
                Node n;
                n.weight = std::max<uint32_t>(1, p->weight);
                n.current = 0;
                n.effective_weight = n.weight;
                nodes[p->id] = n;
            }
            else
            {
                // 更新权重
                it->second.weight = std::max<uint32_t>(1, p->weight);
                // 保留 current 与 effective_weight
            }
            total += nodes[p->id].effective_weight;
            alive[p->id] = true;
        }
        // 移除不再存在的节点
        for (auto it = nodes.begin(); it != nodes.end();)
        {
            if (alive.find(it->first) == alive.end())
                it = nodes.erase(it);
            else
                ++it;
        }
        // 选择最大 current
        std::string best_id;
        uint64_t best_current = 0;
        for (auto &p : instances)
        {
            Node &n = nodes[p->id];
            n.current += n.effective_weight;
            if (n.current > best_current || best_id.empty())
            {
                best_current = n.current;
                best_id = p->id;
            }
        }
        if (best_id.empty())
            return nullptr;
        nodes[best_id].current -= total;
        // 返回对应实例指针（从传入的 instances 中查找）
        for (auto &p : instances)
            if (p->id == best_id)
                return p;
        return nullptr;
    }

private:
    struct Node
    {
        uint32_t weight;
        uint64_t current;
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
            uint64_t lat = p->avg_latency_us.load();
            if (lat == 0)
                lat = UINT64_MAX / 2; // 未知延迟视为很高
            if (!best || lat < best_lat)
            {
                best = p;
                best_lat = lat;
            }
        }
        return best;
    }
};

// LBFactory
LBFactory::LBFactory()
{
    register_factory("round_robin", []() { return std::make_shared<RoundRobinLB>(); });
    register_factory("weighted_rr", []() { return std::make_shared<WeightedRRLB>(); });
    register_factory("least_conn", []() { return std::make_shared<LeastConnLB>(); });
    register_factory("cons_hash", []() { return std::make_shared<ConsistentHashLB>(); });
    register_factory("smooth_weighted", []() { return std::make_shared<SmoothWeightedRRLB>(); });
    register_factory("latency_aware", []() { return std::make_shared<LatencyAwareLB>(); });
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