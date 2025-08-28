#pragma once

#include "ServiceRegistry.h"
#include <atomic>
#include <thread>
#include <unordered_map>
#include <mutex>

class TcpRegistrar {
public:
    TcpRegistrar(ServiceRegistry& reg, uint16_t port = 9090);
    ~TcpRegistrar();
    void start();
    void stop();
private:
    void accept_loop();
    void handle_client(int client_fd);

    ServiceRegistry& reg_;
    uint16_t port_;
    std::thread thr_;
    std::atomic<bool> stop_{false};

    // 记录连接 fd -> 注册的实例 id（便于断开时注销）
    std::mutex conn_mu_;
    std::unordered_map<int, std::string> fd_to_id_;
    // 保存 fd -> 实例指针 以便在心跳时可续约
    std::unordered_map<int, ServiceInstancePtr> fd_to_inst_;
};