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
    constexpr double alpha = 0.2;  // 新样本的权重
    if (avg_latency_us == 0)
    {
        avg_latency_us = sample_us;
        return;
    }

    avg_latency_us = static_cast<uint64_t>(
        alpha * sample_us + (1.0 - alpha) * avg_latency_us
    );
}