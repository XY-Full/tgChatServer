#include "ServiceInstance.h"
#include <sstream>


std::string ServiceInstance::to_string() const {
std::ostringstream ss;
ss << id << "(" << address << ":" << port << ")" << (healthy.load() ? "[ok]" : "[unhealthy]")
<< " w=" << weight << " conn=" << connections.load();
uint64_t lat = avg_latency_us.load();
if (lat) ss << " lat=" << lat << "us";
return ss.str();
}


void ServiceInstance::update_latency_us(uint64_t sample_us) {
uint64_t old = avg_latency_us.load();
if (old == 0) {
// 首次直接写入
avg_latency_us.store(sample_us);
return;
}
// 使用 CAS 环路更新 EWMA： new = 0.8*old + 0.2*sample
while (true) {
uint64_t cur = avg_latency_us.load();
uint64_t next = (cur * 8 + sample_us * 2) / 10; // 简单固定系数
if (avg_latency_us.compare_exchange_weak(cur, next)) break;
// 否则重试
}
}