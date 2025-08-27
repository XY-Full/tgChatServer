// busd.cpp
#include "Busd.h"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

// 连接元数据
struct ConnMeta {
    int fd;
    bool is_unix;
    uint64_t id;
    uint64_t last_active;
    std::string client_id;
    std::string username;
    std::atomic<bool> authenticated{false};
};

// 路由条目
struct RouteEntry {
    uint64_t conn_id;
    uint32_t shm_id;
    uint8_t qos;
    uint8_t flags;
};

// 共享内存仲裁
struct ShmRef {
    std::atomic<uint32_t> ref_cnt{0};
    uint64_t last_touch{0};
};

// 全局容器
static ShmHashMap<std::string, std::vector<RouteEntry>> g_routes;
static ShmHashMap<uint64_t, ConnMeta> g_conns;
static ShmHashMap<uint32_t, ShmRef> g_shm_refs;
static std::atomic<uint64_t> g_conn_seq{1};

// 帧头定义
#pragma pack(push, 1)
struct FrameHeader {
    uint32_t magic = 0x20250427;
    uint8_t version = 0x01;
    uint8_t type;
    uint16_t flags;
    uint32_t body_len;
    uint64_t seq_id;
    uint32_t crc32;
};
#pragma pack(pop)

enum FrameType : uint8_t {
    kPub = 0x01,
    kSub,
    kUnsub,
    kReq,
    kResp,
    kHeartbeat,
    kAuth,
    kConnect,
    kDisconnect
};

class ShmArbiter {
public:
    static uint32_t open(const std::string& name, size_t size) {
        uint32_t id = std::hash<std::string>{}(name);
        auto& ref = g_shm_refs[id];
        if (++ref.ref_cnt == 1) {
            ShmRingBuffer::create(name.c_str(), size);
        }
        ref.last_touch = now_us();
        return id;
    }

    static void close(uint32_t id) {
        auto it = g_shm_refs.find(id);
        if (it != g_shm_refs.end() && --it->second.ref_cnt == 0) {
            std::thread([id] {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                g_shm_refs.erase(id);
            }).detach();
        }
    }

private:
    static uint64_t now_us() {
        return std::chrono::duration_cast<std::chrono::microseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
};

// BusDaemon实现
BusDaemon::BusDaemon() {
    ILOG << "BusDaemon constructor";
}

BusDaemon::~BusDaemon() {
    stop();
}

bool BusDaemon::init(const Config& config) {
    config_ = config;
    
    // 初始化共享内存
    if (!setup_shm()) {
        ELOG << "Failed to setup shared memory";
        return false;
    }
    
    // 初始化网络
    if (!setup_network()) {
        ELOG << "Failed to setup network";
        return false;
    }
    
    ILOG << "BusDaemon initialized successfully";
    return true;
}

bool BusDaemon::setup_network() {
    // 创建epoll实例
    epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd_ == -1) {
        ELOG << "Failed to create epoll instance: " << strerror(errno);
        return false;
    }
    
    // 设置UNIX域套接字
    if (!config_.unix_socket_path.empty()) {
        unix_listen_fd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
        if (unix_listen_fd_ == -1) {
            ELOG << "Failed to create UNIX socket: " << strerror(errno);
            return false;
        }
        
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, config_.unix_socket_path.c_str(), sizeof(addr.sun_path) - 1);
        unlink(config_.unix_socket_path.c_str());
        
        if (bind(unix_listen_fd_, (sockaddr*)&addr, sizeof(addr)) == -1) {
            ELOG << "Failed to bind UNIX socket: " << strerror(errno);
            return false;
        }
        
        if (listen(unix_listen_fd_, 128) == -1) {
            ELOG << "Failed to listen on UNIX socket: " << strerror(errno);
            return false;
        }
        
        // 添加到epoll
        epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = unix_listen_fd_;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, unix_listen_fd_, &ev) == -1) {
            ELOG << "Failed to add UNIX socket to epoll: " << strerror(errno);
            return false;
        }
        
        ILOG << "Listening on UNIX socket: " << config_.unix_socket_path;
    }
    
    // 设置TCP套接字
    tcp_listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (tcp_listen_fd_ == -1) {
        ELOG << "Failed to create TCP socket: " << strerror(errno);
        return false;
    }
    
    int opt = 1;
    setsockopt(tcp_listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(tcp_listen_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));
    
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(config_.tcp_port);
    addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(tcp_listen_fd_, (sockaddr*)&addr, sizeof(addr)) == -1) {
        ELOG << "Failed to bind TCP socket: " << strerror(errno);
        return false;
    }
    
    if (listen(tcp_listen_fd_, 512) == -1) {
        ELOG << "Failed to listen on TCP socket: " << strerror(errno);
        return false;
    }
    
    // 添加到epoll
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = tcp_listen_fd_;
    if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, tcp_listen_fd_, &ev) == -1) {
        ELOG << "Failed to add TCP socket to epoll: " << strerror(errno);
        return false;
    }
    
    ILOG << "Listening on TCP port: " << config_.tcp_port;
    return true;
}

bool BusDaemon::setup_shm() {
    // 创建共享内存目录
    if (mkdir(config_.shm_base_path.c_str(), 0755) == -1 && errno != EEXIST) {
        ELOG << "Failed to create shared memory directory: " << strerror(errno);
        return false;
    }
    
    ILOG << "Shared memory setup at: " << config_.shm_base_path;
    return true;
}

bool BusDaemon::start() {
    if (running_.exchange(true)) {
        WLOG << "BusDaemon is already running";
        return false;
    }
    
    // 启动工作线程
    for (uint32_t i = 0; i < config_.worker_threads; ++i) {
        worker_threads_.emplace_back(&BusDaemon::worker_loop, this);
        ILOG << "Started worker thread " << i;
    }
    
    ILOG << "BusDaemon started with " << config_.worker_threads << " worker threads";
    return true;
}

void BusDaemon::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    
    // 关闭监听套接字
    if (unix_listen_fd_ != -1) close(unix_listen_fd_);
    if (tcp_listen_fd_ != -1) close(tcp_listen_fd_);
    if (cluster_listen_fd_ != -1) close(cluster_listen_fd_);
    if (epoll_fd_ != -1) close(epoll_fd_);
    
    // 等待工作线程结束
    for (auto& thread : worker_threads_) {
        if (thread.joinable()) thread.join();
    }
    
    // 清理所有连接
    {
        std::unique_lock lock(connections_mutex_);
        connections_.clear();
    }
    
    ILOG << "BusDaemon stopped";
}

void BusDaemon::wait() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void BusDaemon::worker_loop() {
    constexpr int max_events = 1024;
    epoll_event events[max_events];
    
    while (running_) {
        int n = epoll_wait(epoll_fd_, events, max_events, 100);
        if (n == -1) {
            if (errno == EINTR) continue;
            ELOG << "epoll_wait error: " << strerror(errno);
            break;
        }
        
        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;
            
            if (fd == unix_listen_fd_ || fd == tcp_listen_fd_) {
                accept_connections(fd, fd == unix_listen_fd_);
            } else if (events[i].events & EPOLLIN) {
                handle_connection(fd);
            } else if (events[i].events & (EPOLLERR | EPOLLHUP)) {
                cleanup_connection(fd);
            }
        }
    }
}

void BusDaemon::accept_connections(int listen_fd, bool is_unix) {
    while (running_) {
        sockaddr_storage addr;
        socklen_t addr_len = sizeof(addr);
        int fd = accept(listen_fd, (sockaddr*)&addr, &addr_len);
        
        if (fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            ELOG << "accept error: " << strerror(errno);
            continue;
        }
        
        // 设置非阻塞
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        
        // 添加到epoll
        epoll_event ev{};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = fd;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) == -1) {
            ELOG << "Failed to add client socket to epoll: " << strerror(errno);
            close(fd);
            continue;
        }
        
        // 创建连接对象
        uint64_t conn_id = g_conn_seq++;
        auto conn = std::make_shared<Connection>(fd, is_unix, conn_id);
        
        {
            std::unique_lock lock(connections_mutex_);
            connections_[fd] = conn;
        }
        
        ILOG << "New connection accepted, fd: " << fd << ", id: " << conn_id;
    }
}

void BusDaemon::handle_connection(int fd) {
    std::shared_ptr<Connection> conn;
    {
        std::shared_lock lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        conn = it->second;
    }
    
    char buffer[8192];
    ssize_t n;
    
    while ((n = read(fd, buffer, sizeof(buffer))) > 0) {
        if (!conn->process_data(buffer, n)) {
            ELOG << "Failed to process data from connection " << conn->id();
            cleanup_connection(fd);
            return;
        }
    }
    
    if (n == 0 || (n == -1 && errno != EAGAIN && errno != EWOULDBLOCK)) {
        cleanup_connection(fd);
    }
}

void BusDaemon::cleanup_connection(int fd) {
    std::shared_ptr<Connection> conn;
    {
        std::unique_lock lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end()) return;
        conn = it->second;
        connections_.erase(it);
    }
    
    epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    close(fd);
    
    ILOG << "Connection closed, fd: " << fd << ", id: " << conn->id();
}

IBus::Stats BusDaemon::get_stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

bool BusDaemon::reload_config() {
    // 实现配置重载逻辑
    ILOG << "Configuration reloaded";
    return true;
}