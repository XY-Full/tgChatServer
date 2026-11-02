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

    AcquireCallback waiter;
    std::shared_ptr<MySQLConnection> handoff;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (shutdown_)
        {
            conn->close();
            delete conn;
            return;
        }
        --active_count_;

        if (conn->isBroken())
        {
            // 作废的连接不再回池，摘除并销毁，下次取连接时按需新建
            for (auto it = all_.begin(); it != all_.end(); ++it)
                if (*it == conn) { all_.erase(it); break; }
            conn->close();
            delete conn;
            cv_.notify_one();
            return;
        }

        idle_.push(conn);
        cv_.notify_one();

        // 有异步等待者时直接把这条连接交给队首
        if (!waiters_.empty())
        {
            waiter = std::move(waiters_.front());
            waiters_.pop();
            handoff = takeIdleLocked();
        }
    }

    // 回调必须在锁外执行，waiter 内部会投递任务，且可能再次取连接
    if (waiter) waiter(std::move(handoff));
}

std::shared_ptr<MySQLConnection> MySQLConnectionPool::takeIdleLocked()
{
    if (idle_.empty()) return nullptr;

    MySQLConnection* raw = idle_.front();
    idle_.pop();
    ++active_count_;
    return std::shared_ptr<MySQLConnection>(raw, [this](MySQLConnection* c) { this->release(c); });
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

        auto conn = takeIdleLocked();
        if (conn) return conn;

        // 池中无空闲连接，尝试扩容
        const int total = static_cast<int>(all_.size()) + creating_;
        if (total < max_pool_size_)
        {
            ++creating_;
            lk.unlock();
            auto* raw = createOne();
            lk.lock();
            --creating_;
            if (raw)
            {
                if (shutdown_)
                {
                    raw->close();
                    delete raw;
                    return nullptr;
                }
                all_.push_back(raw);
                idle_.push(raw);
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

std::shared_ptr<MySQLConnection> MySQLConnectionPool::tryAcquire()
{
    std::unique_lock<std::mutex> lk(mutex_);

    if (shutdown_) return nullptr;

    auto conn = takeIdleLocked();
    if (conn) return conn;

    const int total = static_cast<int>(all_.size()) + creating_;
    if (total < max_pool_size_)
    {
        ++creating_;
        lk.unlock();
        auto* raw = createOne();
        lk.lock();
        --creating_;
        if (!raw) return nullptr;
        if (shutdown_) { raw->close(); delete raw; return nullptr; }

        all_.push_back(raw);
        return takeIdleLocked();
    }

    return nullptr;
}

void MySQLConnectionPool::asyncAcquire(AcquireCallback cb)
{
    std::shared_ptr<MySQLConnection> conn;
    bool rejected = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (shutdown_)
        {
            rejected = true;          // 正在关闭：直接回空
        }
        else
        {
            conn = takeIdleLocked();
            if (!conn) waiters_.push(std::move(cb));  // 没有空闲：排队等归还
        }
    }

    if (conn)          cb(std::move(conn));
    else if (rejected) cb(nullptr);
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
