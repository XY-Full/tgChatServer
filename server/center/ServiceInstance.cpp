#include "ServiceInstance.h"
#include <sstream>

std::string ServiceInstance::to_string() const
{
    std::ostringstream ss;
    ss << "id: " << id << "---(" << address << ":" << port << ")---" << (healthy ? "[ok]" : "[unhealthy]") << " w=" << weight
       << " conn=" << connections;
    uint64_t lat = avg_latency_us;
    if (lat)
        ss << " lat=" << lat << "us";
    return ss.str();
}

void ServiceInstance::update_latency_us(uint64_t sample_us)
{
    uint64_t old = avg_latency_us;
    if (old == 0)
    {
        // 首次直接写入
        avg_latency_us = sample_us;
        return;
    }
    
    // 使用固定系数的 EWMA 更新
    avg_latency_us = (avg_latency_us * 8 + sample_us * 2) / 10;
}