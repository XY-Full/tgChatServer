#pragma once

#include "MySQLConnectionPool.h"
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>

// TransactionManager 管理跨请求的 MySQL 事务。
// 每个事务绑定一个从池中独占获取的连接，以 tx_id 标识。
// 调用方通过 tx_id 在后续 Query/Exec 请求中复用同一连接。
class TransactionManager
{
public:
    explicit TransactionManager(MySQLConnectionPool& pool,
                                uint64_t default_timeout_ms = 30000);
    ~TransactionManager() = default;

    // 开始事务：从池中获取连接并执行 BEGIN，返回 tx_id（0 表示失败）
    uint64_t begin(uint64_t timeout_ms, std::string& err_msg);

    // 获取事务绑定的连接（用于在事务内执行 SQL），nullptr 表示 tx_id 不存在或已超时
    std::shared_ptr<MySQLConnection> getConnection(uint64_t tx_id);

    // 提交事务：COMMIT + 归还连接
    bool commit(uint64_t tx_id, std::string& err_msg);

    // 回滚事务：ROLLBACK + 归还连接
    bool rollback(uint64_t tx_id, std::string& err_msg);

    // 超时检查（在 onTick 中调用）：超时的事务自动 ROLLBACK 并释放连接
    void checkTimeouts();

private:
    struct TxContext
    {
        uint64_t                              tx_id;
        std::shared_ptr<MySQLConnection>      conn;
        std::chrono::steady_clock::time_point deadline;
    };

    bool finalize(uint64_t tx_id, bool do_commit, std::string& err_msg);

    MySQLConnectionPool&                          pool_;
    std::unordered_map<uint64_t, TxContext>       active_txs_;
    std::mutex                                    mutex_;
    std::atomic<uint64_t>                         tx_id_gen_{1};
    uint64_t                                      default_timeout_ms_;
};
