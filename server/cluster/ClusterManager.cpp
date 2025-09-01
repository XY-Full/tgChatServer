// cluster_manager.cpp
#include "ClusterManager.h"
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

ClusterManager::ClusterManager(const Config& config) : config_(config) {
    ILOG << "ClusterManager created for node: " << config.node_id;
}

ClusterManager::~ClusterManager() {
    stop();
}

bool ClusterManager::start() {
    if (running_.exchange(true)) {
        WLOG << "ClusterManager is already running";
        return false;
    }
    
    // 初始化注册客户端
    config_.registry_config.node_id = config_.node_id;
    config_.registry_config.node_host = config_.host;
    config_.registry_config.node_port = config_.port;
    config_.registry_config.capabilities = config_.capabilities;
    
    registry_client_ = std::make_unique<RegistryClient>(config_.registry_config);
    if (!registry_client_->start()) {
        ELOG << "Failed to start registry client";
        running_ = false;
        return false;
    }
    
    // 启动发现线程
    discovery_thread_ = std::thread(&ClusterManager::discovery_loop, this);
    
    // 启动集群服务器
    cluster_server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (cluster_server_fd_ < 0) {
        ELOG << "Failed to create cluster server socket: " << strerror(errno);
        running_ = false;
        return false;
    }
    
    int opt = 1;
    setsockopt(cluster_server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.cluster_port);
    
    if (bind(cluster_server_fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ELOG << "Failed to bind cluster server socket: " << strerror(errno);
        close(cluster_server_fd_);
        running_ = false;
        return false;
    }
    
    if (listen(cluster_server_fd_, 10) < 0) {
        ELOG << "Failed to listen on cluster server socket: " << strerror(errno);
        close(cluster_server_fd_);
        running_ = false;
        return false;
    }
    
    // 启动集群服务器线程
    cluster_server_thread_ = std::thread(&ClusterManager::cluster_server_loop, this);
    
    ILOG << "ClusterManager started successfully, listening on port: " << config_.cluster_port;
    return true;
}

void ClusterManager::stop() {
    if (!running_.exchange(false)) return;
    
    if (registry_client_) registry_client_->stop();
    
    if (discovery_thread_.joinable()) discovery_thread_.join();
    if (cluster_server_thread_.joinable()) cluster_server_thread_.join();
    
    // 关闭所有节点连接
    {
        std::lock_guard lock(connections_mutex_);
        for (auto& pair : node_connections_) {
            close(pair.second);
        }
        node_connections_.clear();
    }
    
    if (cluster_server_fd_ != -1) {
        close(cluster_server_fd_);
        cluster_server_fd_ = -1;
    }
    
    ILOG << "ClusterManager stopped";
}

bool ClusterManager::send_to_node(const std::string& node_id, const IBus::Message& msg) {
    std::lock_guard lock(connections_mutex_);
    
    auto it = node_connections_.find(node_id);
    if (it == node_connections_.end()) {
        // 尝试发现节点并连接
        auto nodes = registry_client_->discover_nodes();
        for (const auto& node : nodes) {
            if (node.first == node_id) {
                if (connect_to_node(node.first, node.second)) {
                    it = node_connections_.find(node_id);
                    break;
                }
            }
        }
        
        if (it == node_connections_.end()) {
            ELOG << "Cannot send to unknown node: " << node_id;
            return false;
        }
    }
    
    // 序列化并发送消息（简化实现）
    // 在实际实现中，应该使用协议缓冲区或其他序列化方式
    std::string serialized = "SERIALIZED_MESSAGE"; // 伪代码
    
    if (write(it->second, serialized.c_str(), serialized.length()) < 0) {
        ELOG << "Failed to send message to node: " << node_id << ", error: " << strerror(errno);
        close(it->second);
        node_connections_.erase(it);
        return false;
    }
    
    DLOG << "Message sent to node: " << node_id;
    return true;
}

bool ClusterManager::broadcast(const IBus::Message& msg) {
    auto nodes = registry_client_->discover_nodes();
    bool success = true;
    
    for (const auto& node : nodes) {
        if (node.first != config_.node_id) { // 不发送给自己
            if (!send_to_node(node.first, msg)) {
                success = false;
            }
        }
    }
    
    return success;
}

void ClusterManager::discovery_loop() {
    ILOG << "Starting discovery loop";
    
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        
        // 发现新节点并建立连接
        auto nodes = registry_client_->discover_nodes();
        for (const auto& node : nodes) {
            if (node.first != config_.node_id) {
                std::lock_guard lock(connections_mutex_);
                if (node_connections_.find(node.first) == node_connections_.end()) {
                    connect_to_node(node.first, node.second);
                }
            }
        }
    }
    
    ILOG << "Exiting discovery loop";
}

void ClusterManager::cluster_server_loop() {
    ILOG << "Starting cluster server loop";
    
    while (running_) {
        sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(cluster_server_fd_, (sockaddr*)&client_addr, &client_len);
        
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            ELOG << "Accept error: " << strerror(errno);
            break;
        }
        
        // 处理集群连接（简化实现）
        // 在实际实现中，应该解析消息并路由到本地总线
        char buffer[4096];
        ssize_t n = read(client_fd, buffer, sizeof(buffer));
        
        if (n > 0) {
            DLOG << "Received cluster message, length: " << n;
            // 处理消息...
        }
        
        close(client_fd);
    }
    
    ILOG << "Exiting cluster server loop";
}

bool ClusterManager::connect_to_node(const std::string& node_id, const std::string& host, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ELOG << "Failed to create socket for node " << node_id << ": " << strerror(errno);
        return false;
    }
    
    // 设置非阻塞
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
    
    // 异步连接
    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        if (errno != EINPROGRESS) {
            ELOG << "Failed to connect to node " << node_id << ": " << strerror(errno);
            close(sock);
            return false;
        }
        
        // 等待连接完成
        fd_set set;
        FD_ZERO(&set);
        FD_SET(sock, &set);
        
        timeval timeout{};
        timeout.tv_sec = 5;
        
        if (select(sock + 1, nullptr, &set, nullptr, &timeout) <= 0) {
            ELOG << "Connection timeout to node " << node_id;
            close(sock);
            return false;
        }
        
        int error = 0;
        socklen_t len = sizeof(error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);
        
        if (error != 0) {
            ELOG << "Connection failed to node " << node_id << ": " << strerror(error);
            close(sock);
            return false;
        }
    }
    
    // 恢复阻塞模式
    fcntl(sock, F_SETFL, flags);
    
    std::lock_guard lock(connections_mutex_);
    node_connections_[node_id] = sock;
    
    ILOG << "Connected to node: " << node_id << " (" << host << ":" << port << ")";
    return true;
}