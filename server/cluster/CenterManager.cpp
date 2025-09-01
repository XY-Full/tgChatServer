// center_manager.cpp
#include "CenterManager.h"
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../common/Log.h"
#include <sstream>
#include <iostream>

CenterManager::CenterManager(const Config& config) : config_(config) {
    ILOG << "CenterManager created with port: " << config.port;
}

CenterManager::~CenterManager() {
    stop();
}

bool CenterManager::start() {
    if (running_.exchange(true)) {
        WLOG << "CenterManager is already running";
        return false;
    }
    
    // 启动清理线程
    cleanup_thread_ = std::thread(&CenterManager::cleanup_loop, this);
    
    // 启动API服务器线程
    api_thread_ = std::thread(&CenterManager::api_server_loop, this);
    
    ILOG << "CenterManager started successfully";
    return true;
}

void CenterManager::stop() {
    if (!running_.exchange(false)) return;
    
    if (cleanup_thread_.joinable()) cleanup_thread_.join();
    if (api_thread_.joinable()) api_thread_.join();
    
    ILOG << "CenterManager stopped";
}

bool CenterManager::register_node(const std::string& node_id, const std::string& host, 
                                 uint16_t port, const std::vector<std::string>& capabilities) {
    std::unique_lock lock(nodes_mutex_);
    
    NodeInfo info;
    info.node_id = node_id;
    info.host = host;
    info.port = port;
    info.capabilities = capabilities;
    info.last_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    info.startup_time = info.last_heartbeat;
    
    nodes_[node_id] = info;
    
    ILOG << "Node registered: " << node_id << " (" << host << ":" << port << ")";
    return true;
}

bool CenterManager::update_heartbeat(const std::string& node_id) {
    std::unique_lock lock(nodes_mutex_);
    
    auto it = nodes_.find(node_id);
    if (it == nodes_.end()) {
        WLOG << "Cannot update heartbeat for unknown node: " << node_id;
        return false;
    }
    
    it->second.last_heartbeat = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    
    DLOG << "Heartbeat updated for node: " << node_id;
    return true;
}

bool CenterManager::unregister_node(const std::string& node_id) {
    std::unique_lock lock(nodes_mutex_);
    
    if (nodes_.erase(node_id) > 0) {
        ILOG << "Node unregistered: " << node_id;
        return true;
    }
    
    WLOG << "Cannot unregister unknown node: " << node_id;
    return false;
}

std::vector<CenterManager::NodeInfo> CenterManager::get_all_nodes() const {
    std::shared_lock lock(nodes_mutex_);
    
    std::vector<NodeInfo> result;
    for (const auto& pair : nodes_) {
        result.push_back(pair.second);
    }
    
    return result;
}

std::vector<CenterManager::NodeInfo> CenterManager::get_nodes_by_capability(const std::string& capability) const {
    std::shared_lock lock(nodes_mutex_);
    
    std::vector<NodeInfo> result;
    for (const auto& pair : nodes_) {
        for (const auto& cap : pair.second.capabilities) {
            if (cap == capability) {
                result.push_back(pair.second);
                break;
            }
        }
    }
    
    return result;
}

CenterManager::NodeInfo CenterManager::get_node(const std::string& node_id) const {
    std::shared_lock lock(nodes_mutex_);
    
    auto it = nodes_.find(node_id);
    if (it != nodes_.end()) {
        return it->second;
    }
    
    throw std::runtime_error("Node not found: " + node_id);
}

void CenterManager::cleanup_loop() {
    ILOG << "Starting cleanup loop";
    
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.cleanup_interval));
        
        uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        
        std::vector<std::string> to_remove;
        
        {
            std::unique_lock lock(nodes_mutex_);
            for (const auto& pair : nodes_) {
                if (now - pair.second.last_heartbeat > config_.heartbeat_timeout) {
                    to_remove.push_back(pair.first);
                }
            }
            
            for (const auto& node_id : to_remove) {
                nodes_.erase(node_id);
                ILOG << "Removed inactive node: " << node_id;
            }
        }
    }
    
    ILOG << "Exiting cleanup loop";
}

void CenterManager::api_server_loop() {
    ILOG << "Starting API server on port: " << config_.port;
    
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        ELOG << "Failed to create server socket: " << strerror(errno);
        return;
    }
    
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = inet_addr(config_.bind_host.c_str());
    address.sin_port = htons(config_.port);
    
    if (bind(server_fd, (sockaddr*)&address, sizeof(address)) < 0) {
        ELOG << "Failed to bind server socket: " << strerror(errno);
        close(server_fd);
        return;
    }
    
    if (listen(server_fd, 10) < 0) {
        ELOG << "Failed to listen on server socket: " << strerror(errno);
        close(server_fd);
        return;
    }
    
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            ELOG << "Accept error: " << strerror(errno);
            break;
        }
        
        // 处理客户端请求（简化实现）
        // 在实际实现中，应该解析HTTP请求并返回JSON响应
        char buffer[1024];
        ssize_t n = read(client_fd, buffer, sizeof(buffer));
        
        if (n > 0) {
            std::string response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n\r\n";
            response += "{\"status\":\"ok\",\"message\":\"Request processed\"}";
            write(client_fd, response.c_str(), response.length());
        }
        
        close(client_fd);
    }
    
    close(server_fd);
    ILOG << "Exiting API server loop";
}