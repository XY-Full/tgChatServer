// busd.h
#ifndef BUS_DAEMON_H
#define BUS_DAEMON_H

#include "../core/bus/IBusCommon.h"
#include "../core/shm/shm_ringbuffer.h"
#include "../core/shm/shm_hashmap.h"
#include "../core/shm/shm_slab.h"
#include "../core/shm/shm_spinlock.h"
#include "../core/shm/shm_epoch.h"
#include "Log.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <shared_mutex>

class BusDaemon {
public:
    struct Config {
        // 网络配置
        std::string unix_socket_path = "/var/run/ibus/busd.sock";   // 一台机器仅有一台Busd
        uint16_t tcp_port = 1883;
        
        // 共享内存配置
        std::string shm_base_path = "/dev/shm/ibus";
        size_t shm_segment_size = 64 * 1024 * 1024; // 64MB
        size_t max_shm_segments = 16;
        
        // 性能配置
        uint32_t max_connections = 10000;
        uint32_t max_topics = 100000;
        uint32_t max_message_size = 16 * 1024 * 1024; // 16MB
        uint32_t worker_threads = 4;
        
        // 持久化配置
        bool persistence_enabled = false;
        std::string persistence_path = "/var/lib/ibus";
        size_t persistence_max_size = 1 * 1024 * 1024 * 1024; // 1GB
        
        // 安全配置
        bool authentication_enabled = false;
        std::string auth_config_path = "/etc/ibus/auth.conf";

        // 集群配置
        bool cluster_enabled = false;
        std::string node_id;
        std::string cluster_host;
        uint16_t cluster_port = 1884;
        std::vector<std::string> capabilities;
        
        // 中心管理服务配置
        std::string center_host = "127.0.0.1";
        uint16_t center_port = 8888;
    };
    
    BusDaemon();
    ~BusDaemon();
    
    bool init(const Config& config);
    bool start();
    void stop();
    void wait();
    
    // 管理接口
    IBus::Stats get_stats() const;
    bool reload_config();
    
private:
    // 内部类前向声明
    class Connection;
    class TopicManager;
    class PersistenceManager;
    class ClusterManager;
    class SecurityManager;
    
    Config config_;
    std::atomic<bool> running_{false};
    
    // 核心组件
    std::unique_ptr<TopicManager> topic_manager_;
    std::unique_ptr<PersistenceManager> persistence_manager_;
    std::unique_ptr<ClusterManager> cluster_manager_;
    std::unique_ptr<SecurityManager> security_manager_;
    
    // 网络组件
    int epoll_fd_{-1};
    int unix_listen_fd_{-1};
    int tcp_listen_fd_{-1};
    int cluster_listen_fd_{-1};
    
    // 连接管理
    std::unordered_map<int, std::shared_ptr<Connection>> connections_;
    std::shared_mutex connections_mutex_;
    
    // 工作线程
    std::vector<std::thread> worker_threads_;
    
    // 统计信息
    mutable std::mutex stats_mutex_;
    IBus::Stats stats_;
    
    // 内部方法
    bool setup_network();
    bool setup_shm();
    void worker_loop();
    void accept_connections(int listen_fd, bool is_unix);
    void handle_connection(int fd);
    void cleanup_connection(int fd);
    void update_stats(const IBus::Stats& delta);
};

#endif // BUS_DAEMON_H