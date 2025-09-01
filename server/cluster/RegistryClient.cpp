// registry_client.cpp
#include "RegistryClient.h"
#include <arpa/inet.h>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sstream>

RegistryClient::RegistryClient(const Config& config) : config_(config) {
    ILOG << "RegistryClient created for node: " << config.node_id;
}

RegistryClient::~RegistryClient() {
    stop();
}

bool RegistryClient::start() {
    if (running_.exchange(true)) {
        WLOG << "RegistryClient is already running";
        return false;
    }
    
    // 注册节点
    std::stringstream body;
    body << "{\"node_id\":\"" << config_.node_id 
         << "\",\"host\":\"" << config_.node_host 
         << "\",\"port\":" << config_.node_port 
         << ",\"capabilities\":[";
    
    for (size_t i = 0; i < config_.capabilities.size(); ++i) {
        if (i > 0) body << ",";
        body << "\"" << config_.capabilities[i] << "\"";
    }
    body << "]}";
    
    std::string response;
    if (!send_request("POST", "/register", body.str(), response)) {
        ELOG << "Failed to register node";
        running_ = false;
        return false;
    }
    
    // 启动心跳线程
    heartbeat_thread_ = std::thread(&RegistryClient::heartbeat_loop, this);
    
    ILOG << "RegistryClient started successfully";
    return true;
}

void RegistryClient::stop() {
    if (!running_.exchange(false)) return;
    
    // 取消注册节点
    std::stringstream body;
    body << "{\"node_id\":\"" << config_.node_id << "\"}";
    
    std::string response;
    send_request("POST", "/unregister", body.str(), response);
    
    if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
    
    ILOG << "RegistryClient stopped";
}

std::vector<std::pair<std::string, uint16_t>> RegistryClient::discover_nodes() {
    std::string response;
    if (!send_request("GET", "/nodes", "", response)) {
        ELOG << "Failed to discover nodes";
        return {};
    }
    
    // 解析响应（简化实现）
    // 在实际实现中，应该解析JSON响应
    std::vector<std::pair<std::string, uint16_t>> nodes;
    nodes.emplace_back("192.168.1.100", 1883);
    nodes.emplace_back("192.168.1.101", 1883);
    
    return nodes;
}

std::vector<std::pair<std::string, uint16_t>> RegistryClient::discover_nodes_by_capability(const std::string& capability) {
    std::string response;
    if (!send_request("GET", "/nodes?capability=" + capability, "", response)) {
        ELOG << "Failed to discover nodes by capability: " << capability;
        return {};
    }
    
    // 解析响应（简化实现）
    std::vector<std::pair<std::string, uint16_t>> nodes;
    if (capability == "persistence") {
        nodes.emplace_back("192.168.1.102", 1883);
    }
    
    return nodes;
}

void RegistryClient::heartbeat_loop() {
    ILOG << "Starting heartbeat loop";
    
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(config_.heartbeat_interval));
        
        std::stringstream body;
        body << "{\"node_id\":\"" << config_.node_id << "\"}";
        
        std::string response;
        if (!send_request("POST", "/heartbeat", body.str(), response)) {
            WLOG << "Failed to send heartbeat";
        } else {
            DLOG << "Heartbeat sent successfully";
        }
    }
    
    ILOG << "Exiting heartbeat loop";
}

bool RegistryClient::send_request(const std::string& method, const std::string& path, 
                                 const std::string& body, std::string& response) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ELOG << "Failed to create socket: " << strerror(errno);
        return false;
    }
    
    sockaddr_in server_addr{};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(config_.center_port);
    inet_pton(AF_INET, config_.center_host.c_str(), &server_addr.sin_addr);
    
    if (connect(sock, (sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        ELOG << "Failed to connect to center: " << strerror(errno);
        close(sock);
        return false;
    }
    
    // 构建HTTP请求
    std::stringstream request;
    request << method << " " << path << " HTTP/1.1\r\n";
    request << "Host: " << config_.center_host << ":" << config_.center_port << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Content-Length: " << body.length() << "\r\n";
    request << "Connection: close\r\n\r\n";
    request << body;
    
    // 发送请求
    if (write(sock, request.str().c_str(), request.str().length()) < 0) {
        ELOG << "Failed to send request: " << strerror(errno);
        close(sock);
        return false;
    }
    
    // 读取响应
    char buffer[4096];
    ssize_t n;
    response.clear();
    
    while ((n = read(sock, buffer, sizeof(buffer))) > 0) {
        response.append(buffer, n);
    }
    
    close(sock);
    
    // 检查响应状态（简化实现）
    if (response.find("200 OK") == std::string::npos) {
        ELOG << "Request failed, response: " << response;
        return false;
    }
    
    return true;
}