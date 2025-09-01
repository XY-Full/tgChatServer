// registry_client.h
#ifndef REGISTRY_CLIENT_H
#define REGISTRY_CLIENT_H

#include "../core/bus/IBusCommon.h"
#include "../common/Log.h"
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <chrono>

class RegistryClient {
public:
    struct Config {
        std::string center_host = "127.0.0.1";
        uint16_t center_port = 8888;
        std::string node_id;
        std::string node_host;
        uint16_t node_port;
        uint64_t heartbeat_interval = 10000; // 10秒
        std::vector<std::string> capabilities;
    };
    
    RegistryClient(const Config& config);
    ~RegistryClient();
    
    bool start();
    void stop();
    
    // 服务发现
    std::vector<std::pair<std::string, uint16_t>> discover_nodes();
    std::vector<std::pair<std::string, uint16_t>> discover_nodes_by_capability(const std::string& capability);
    
    // 主题路由（可选）
    bool register_topic(const std::string& topic);
    bool unregister_topic(const std::string& topic);
    
private:
    void heartbeat_loop();
    bool send_request(const std::string& method, const std::string& path, 
                     const std::string& body, std::string& response);
    
    Config config_;
    std::atomic<bool> running_{false};
    std::thread heartbeat_thread_;
};

#endif // REGISTRY_CLIENT_H