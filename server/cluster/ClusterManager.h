// cluster_manager.h
#ifndef CLUSTER_MANAGER_H
#define CLUSTER_MANAGER_H

#include "../core/bus/IBusCommon.h"
#include "../common/Log.h"
#include "RegistryClient.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>

class ClusterManager {
public:
    struct Config {
        std::string node_id;
        std::string host;
        uint16_t port;
        uint16_t cluster_port = 1884;
        std::vector<std::string> capabilities;
        RegistryClient::Config registry_config;
    };
    
    ClusterManager(const Config& config);
    ~ClusterManager();
    
    bool start();
    void stop();
    
    // 节点间通信
    bool send_to_node(const std::string& node_id, const IBus::Message& msg);
    bool broadcast(const IBus::Message& msg);
    
    // 节点管理
    void add_node(const std::string& node_id, const std::string& host, uint16_t port);
    void remove_node(const std::string& node_id);
    
    // 主题路由
    bool add_topic_route(const std::string& topic, const std::string& node_id);
    bool remove_topic_route(const std::string& topic, const std::string& node_id);
    
private:
    void discovery_loop();
    void cluster_server_loop();
    bool connect_to_node(const std::string& node_id, const std::string& host, uint16_t port);
    
    Config config_;
    std::atomic<bool> running_{false};
    
    // 节点连接管理
    mutable std::mutex connections_mutex_;
    std::unordered_map<std::string, int> node_connections_;
    
    // 主题路由
    mutable std::mutex routes_mutex_;
    std::unordered_map<std::string, std::vector<std::string>> topic_routes_;
    
    // 组件
    std::unique_ptr<RegistryClient> registry_client_;
    
    // 工作线程
    std::thread discovery_thread_;
    std::thread cluster_server_thread_;
    
    // 集群服务器套接字
    int cluster_server_fd_{-1};
};

#endif // CLUSTER_MANAGER_H