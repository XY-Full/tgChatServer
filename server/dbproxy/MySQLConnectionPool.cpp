#include "MySQLConnectionPool.h"
#include "Log.h"

MySQLConnectionPool::MySQLConnectionPool(const MySQLConfig& config, int pool_size, int max_pool_size)
    : config_(config), pool_size_(pool_size), max_pool_size_(max_pool_size)
{
    // 预先创建 pool_size 个连接
    for (int i = 0; i < pool_size_; ++i)
    {
        auto* conn = createOne();
        if (conn)
        {
            idle_.push(conn);
            all_.push_back(conn);
        }
    }
    ILOG << "MySQLConnectionPool: initialized with " << idle_.size()
         << "/" << pool_size_ << " connections";
}

MySQLConnectionPool::~MySQLConnectionPool()
{
    shutdown();
}

MySQLConnection* MySQLConnectionPool::createOne()
{
    auto* conn = new MySQLConnection(config_);
    if (!conn->connect())
    {
        delete conn;
        return nullptr;
    }
    return conn;
}

void MySQLConnectionPool::release(MySQLConnection* conn)
{
    if (!conn) return;
    std::lock_guard<std::mutex> lk(mutex_);
    if (shutdown_)
    {
        conn->close();
        delete conn;
        return;
    }
    --active_count_;
    idle_.push(conn);
    cv_.notify_one();
}

std::shared_ptr<MySQLConnection> MySQLConnectionPool::acquire(int timeout_ms)
{
    std::unique_lock<std::mutex> lk(mutex_);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    while (true)
    {
        if (shutdown_)
        {
            ELOG << "MySQLConnectionPool::acquire: pool is shutting down";
            return nullptr;
        }

        if (!idle_.empty())
        {
            MySQLConnection* raw = idle_.front();
            idle_.pop();
            ++active_count_;
            // custom deleter 自动归还
            return std::shared_ptr<MySQLConnection>(
                raw, [this](MySQLConnection* c) { this->release(c); });
        }

        // 池中无空闲连接，尝试扩容（不超过 max_pool_size）
        int total = static_cast<int>(all_.size());
        if (total < max_pool_size_)
        {
            lk.unlock();
            auto* conn = createOne();
            lk.lock();
            if (conn)
            {
                all_.push_back(conn);
                idle_.push(conn);
                continue; // 重新尝试 acquire
            }
        }

        // 等待有连接归还
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout)
        {
            ELOG << "MySQLConnectionPool::acquire: timed out after " << timeout_ms << "ms";
            return nullptr;
        }
    }
}

void MySQLConnectionPool::healthCheck()
{
    std::vector<MySQLConnection*> to_check;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        // 将所有空闲连接取出检查
        std::queue<MySQLConnection*> tmp;
        while (!idle_.empty())
        {
            to_check.push_back(idle_.front());
            idle_.pop();
        }
    }

    std::vector<MySQLConnection*> healthy;
    for (auto* conn : to_check)
    {
        if (!conn->ping())
        {
            ILOG << "MySQLConnectionPool: reconnecting dead connection";
            if (!conn->reconnect())
            {
                ELOG << "MySQLConnectionPool: reconnect failed, dropping connection";
                // 从 all_ 中移除并删除
                std::lock_guard<std::mutex> lk(mutex_);
                auto it = std::find(all_.begin(), all_.end(), conn);
                if (it != all_.end()) all_.erase(it);
                delete conn;
                continue;
            }
        }
        healthy.push_back(conn);
    }

    // 将健康连接放回队列
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto* conn : healthy)
        idle_.push(conn);
}

void MySQLConnectionPool::shutdown()
{
    std::lock_guard<std::mutex> lk(mutex_);
    if (shutdown_) return;
    shutdown_ = true;
    cv_.notify_all();

    // 关闭并释放所有连接
    for (auto* conn : all_)
    {
        conn->close();
        delete conn;
    }
    all_.clear();
    while (!idle_.empty()) idle_.pop();
    ILOG << "MySQLConnectionPool: shutdown complete";
}

int MySQLConnectionPool::idleCount() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return static_cast<int>(idle_.size());
}

int MySQLConnectionPool::activeCount() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return active_count_;
}
