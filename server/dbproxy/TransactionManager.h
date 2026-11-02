#pragma once

#include "AsyncMySql.h"
#include "MySQLConnection.h"
#include "MySQLConnectionPool.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * @brief 跨请求事务管理
 *
 * 事务从 BEGIN 到 COMMIT/ROLLBACK 全程独占一条连接：连接在 BEGIN 成功后由本类持有
 * shared_ptr，既不在池的空闲队列里，也不会因为中间协程挂起、其它请求排队而被分配出去
 *
 * BEGIN / COMMIT / ROLLBACK 都通过 AsyncMySql 的 Raw 路径执行，不占用任何线程
 * （与查询同一套非阻塞机制）
 */
class TransactionManager
{
public:
    explicit TransactionManager(AsyncMySql& agent, MySQLConnectionPool& pool,
                                uint64_t default_timeout_ms = 30000);
    ~TransactionManager() = default;

    using BeginCallback = std::function<void(uint64_t tx_id, const std::string& err)>;
    using Callback      = std::function<void(bool ok, const std::string& err)>;

    // 成功后连接归该事务独占
    void beginAsync(uint64_t timeout_ms, BeginCallback cb);

    // 取事务独占的连接
    std::shared_ptr<MySQLConnection> getConnection(uint64_t tx_id);

    // 异步 COMMIT / ROLLBACK；完成后连接归还池，失败则作废重建
    void commitAsync(uint64_t tx_id, Callback cb);
    void rollbackAsync(uint64_t tx_id, Callback cb);

    // 超时检查，超时事务自动 ROLLBACK
    void checkTimeouts();

private:
    struct TxContext
    {
        uint64_t                              tx_id;
        std::shared_ptr<MySQLConnection>      conn;
        std::chrono::steady_clock::time_point deadline;
    };

    void finalizeAsync(uint64_t tx_id, bool do_commit, Callback cb);

    AsyncMySql&                              agent_;
    MySQLConnectionPool&                     pool_;
    std::unordered_map<uint64_t, TxContext>  active_txs_;
    std::mutex                                mutex_;
    std::atomic<uint64_t>                     tx_id_gen_{1};
    uint64_t                                  default_timeout_ms_;
};
