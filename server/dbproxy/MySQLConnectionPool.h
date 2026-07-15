#pragma once

#include "MySQLConnection.h"
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <vector>
#include <cstdint>

class MySQLConnectionPool
{
public:
    explicit MySQLConnectionPool(const MySQLConfig& config,
                                 int pool_size     = 8,
                                 int max_pool_size = 32);
    ~MySQLConnectionPool();

    // 禁止拷贝和移动
    MySQLConnectionPool(const MySQLConnectionPool&)            = delete;
    MySQLConnectionPool& operator=(const MySQLConnectionPool&) = delete;

    // 获取连接（阻塞，超时返回 nullptr）
    // 返回的 shared_ptr 析构时自动将连接归还到池中
    std::shared_ptr<MySQLConnection> acquire(int timeout_ms = 5000);

    // 健康检查：对所有空闲连接调用 ping，断连则重连（在 onTick 中调用）
    void healthCheck();

    // 关闭所有连接并清理
    void shutdown();

    // 当前空闲连接数（调试/监控用）
    int idleCount() const;

    // 当前借出中的连接数
    int activeCount() const;

private:
    // 创建一个新连接（已完成 connect()）
    MySQLConnection* createOne();

    // 将连接归还到空闲队列（由 shared_ptr deleter 调用）
    void release(MySQLConnection* conn);

    MySQLConfig            config_;
    int                    pool_size_;
    int                    max_pool_size_;
    std::queue<MySQLConnection*>   idle_;    // 空闲连接队列
    std::vector<MySQLConnection*>  all_;     // 所有连接（用于 shutdown）
    mutable std::mutex             mutex_;
    std::condition_variable        cv_;
    int                            active_count_ = 0;
    int                            creating_     = 0; // 解锁创建中的连接数，扩容判断时计入
    bool                           shutdown_     = false;
};
