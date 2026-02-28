#pragma once

#include "ServiceRegistry.h"
#include "network/AppMsg.h"
#include "network/TcpServer.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

#define TTL 15

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
    void onUpdateServiceStatus(uint64_t client_fd, std::shared_ptr<AppMsg> msg);

    ServiceRegistry &reg_;
    uint16_t port_;
    std::atomic<bool> stop_{false};

    std::mutex conn_mu_;
    // fd -> 实例 id（断开时注销用）
    std::unordered_map<int, std::string> fd_to_id_;
    // fd -> 实例指针（续约快速路径）
    std::unordered_map<int, ServiceInstancePtr> fd_to_inst_;
    // 已收到过全量快照的 fd 集合（用于判断是否首次请求）
    std::unordered_set<uint64_t> fd_to_got_full_update_;

    TcpServer server_;

    std::unordered_map<uint64_t, std::function<void(uint64_t, std::shared_ptr<AppMsg>)>> msg_handler_;
};