// center_manager.h
#ifndef CENTER_MANAGER_H
#define CENTER_MANAGER_H

#include "../core/bus/IBusCommon.h"
#include "../common/Log.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>
#include <atomic>
#include <thread>
#include <chrono>

class CenterManager {
public:
    struct NodeInfo {
        std::string node_id;
        std::string host;
        uint16_t port;
        uint64_t last_heartbeat;
        uint64_t startup_time;
        std::vector<std::string> capabilities;
        IBus::Stats stats;
    };
    
    struct Config {
        std::string bind_host = "0.0.0.0";
        uint16_t port = 8888;
        uint64_t heartbeat_timeout = 30000; // 30秒
        uint64_t cleanup_interval = 10000;  // 10秒
    };
    
    CenterManager(const Config& config);
    ~CenterManager();
    
    bool start();
    void stop();
    
    // 节点管理
    bool register_node(const std::string& node_id, const std::string& host, uint16_t port, 
                      const std::vector<std::string>& capabilities);
    bool update_heartbeat(const std::string& node_id);
    bool unregister_node(const std::string& node_id);
    
    // 服务发现
    std::vector<NodeInfo> get_all_nodes() const;
    std::vector<NodeInfo> get_nodes_by_capability(const std::string& capability) const;
    NodeInfo get_node(const std::string& node_id) const;
    
    // 主题路由（可选）
    bool add_topic_route(const std::string& topic, const std::string& node_id);
    bool remove_topic_route(const std::string& topic, const std::string& node_id);
    std::vector<std::string> get_topic_routes(const std::string& topic) const;
    
private:
    void cleanup_loop();
    void api_server_loop();
    
    Config config_;
    std::atomic<bool> running_{false};
    
    // 节点注册表
    mutable std::shared_mutex nodes_mutex_;
    std::unordered_map<std::string, NodeInfo> nodes_;
    
    // 主题路由表（可选）
    mutable std::shared_mutex routes_mutex_;
    std::unordered_map<std::string, std::vector<std::string>> topic_routes_;
    
    // 工作线程
    std::thread cleanup_thread_;
    std::thread api_thread_;
};

#endif // CENTER_MANAGER_H