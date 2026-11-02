#pragma once

#include "MySQLConnection.h"
#include <condition_variable>
#include <functional>
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

    // 获取一个连接，若没有可用连接会阻塞等待，返回的 shared_ptr 析构时自动将连接归还到池中
    std::shared_ptr<MySQLConnection> acquire(int timeout_ms = 5000);

    // 尝试获取一个连接
    std::shared_ptr<MySQLConnection> tryAcquire();

    // 取不到时排队，等有连接归还再回调
    using AcquireCallback = std::function<void(std::shared_ptr<MySQLConnection>)>;
    // 异步获取一个连接
    void asyncAcquire(AcquireCallback cb);

    // 健康检查：对所有空闲连接调用 ping，断连则重连
    void healthCheck();

    // 关闭所有连接并清理
    void shutdown();

    // 当前空闲连接数
    int idleCount() const;

    // 当前借出中的连接数
    int activeCount() const;

    // 连接配置
    const MySQLConfig& config() const { return config_; }

private:
    // 创建一个新连接
    MySQLConnection* createOne();

    // 将连接归还到空闲队列
    void release(MySQLConnection* conn);

    // 从空闲队列取一条；调用方必须已持有 mutex_
    std::shared_ptr<MySQLConnection> takeIdleLocked();

    MySQLConfig            config_;
    int                    pool_size_;
    int                    max_pool_size_;
    std::queue<MySQLConnection*>   idle_;    // 空闲连接队列
    std::vector<MySQLConnection*>  all_;     // 所有连接（用于 shutdown）
    mutable std::mutex             mutex_;
    std::condition_variable        cv_;
    int                            active_count_ = 0;

    // 异步等待队列：连接归还时优先唤醒等待队列里的用户
    std::queue<AcquireCallback>    waiters_;
    int                            creating_     = 0; // 解锁创建中的连接数，扩容判断时计入
    bool                           shutdown_     = false;
};
