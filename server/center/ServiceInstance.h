#pragma once


#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>


// 服务实例的数据结构。包含注册时的静态信息以及运行时统计信息。
struct ServiceInstance {
std::string id; // 实例唯一 id，例如 host:port#uuid
std::string svc_name; // 服务名
std::string address; // IP 或主机名
uint16_t port = 0; // 端口
uint32_t weight = 1; // 负载均衡权重
std::map<std::string,std::string> metadata; // 可选元数据


// 运行时字段（线程安全）
std::atomic<bool> healthy{true};
std::atomic<uint64_t> connections{0};
std::chrono::steady_clock::time_point last_seen = std::chrono::steady_clock::now();


// 平均延迟（微秒），用于延迟感知 LB。用原子整型表示微秒整数。
std::atomic<uint64_t> avg_latency_us{0};


std::string to_string() const;


// 使用整数 EWMA 更新平均延迟（微秒）。alpha 取 0.2（即 new = 0.2*sample + 0.8*old），使用定点简化为( old*8 + sample*2)/10
void update_latency_us(uint64_t sample_us);
};


using ServiceInstancePtr = std::shared_ptr<ServiceInstance>;
using ServiceInstances = std::vector<ServiceInstancePtr>;