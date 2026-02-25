#pragma once

#include "ServiceRegistry.h"
#include "network/AppMsg.h"
#include "network/TcpServer.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

#define TTL 300

class TcpRegistrar
{
public:
    TcpRegistrar(ServiceRegistry &reg, uint16_t port = 9090);
    ~TcpRegistrar();
    void start();
    void stop();

private:
    void accept_loop();
    
    void handleClient(uint64_t client_fd, std::shared_ptr<AppMsg> msg);
    void handleDisconnect(uint64_t client_fd);

    void onRegist(uint64_t client_fd, std::shared_ptr<AppMsg> msg);
    void onHeartbeat(uint64_t client_fd, std::shared_ptr<AppMsg> msg);

    ServiceRegistry &reg_;
    uint16_t port_;
    std::atomic<bool> stop_{false};

    // 记录连接 fd -> 注册的实例 id（便于断开时注销）
    std::mutex conn_mu_;
    std::unordered_map<int, std::string> fd_to_id_;
    // 保存 fd -> 实例指针 以便在心跳时可续约
    std::unordered_map<int, ServiceInstancePtr> fd_to_inst_;

    TcpServer server_;

    std::unordered_map<uint64_t, std::function<void(uint64_t, std::shared_ptr<AppMsg>)>> msg_handler_;
};