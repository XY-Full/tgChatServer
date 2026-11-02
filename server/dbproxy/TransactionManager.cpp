#include "TransactionManager.h"

#include "Log.h"

#include <utility>

TransactionManager::TransactionManager(AsyncMySql& agent, MySQLConnectionPool& pool,
                                       uint64_t default_timeout_ms)
    : agent_(agent), pool_(pool), default_timeout_ms_(default_timeout_ms)
{
}

void TransactionManager::beginAsync(uint64_t timeout_ms, BeginCallback cb)
{
    if (timeout_ms == 0) timeout_ms = default_timeout_ms_;

    // 取连接
    pool_.asyncAcquire(
        [this, timeout_ms, cb = std::move(cb)](std::shared_ptr<MySQLConnection> conn)
        {
            if (!conn)
            {
                cb(0, "failed to acquire connection from pool");
                return;
            }

            // 在事务独占的连接上执行 BEGIN
            agent_.submit(DbOpKind::Raw, "START TRANSACTION", {}, conn,
                          [this, cb = std::move(cb), conn, timeout_ms](DbOpResult r) mutable
                          {
                              if (!r.ok)
                              {
                                  conn.reset();   // 归还
                                  cb(0, r.err);
                                  return;
                              }

                              const uint64_t tx_id = tx_id_gen_.fetch_add(1, std::memory_order_relaxed);
                              const auto     deadline = std::chrono::steady_clock::now() +
                                                        std::chrono::milliseconds(timeout_ms);

                              {
                                  std::lock_guard<std::mutex> lk(mutex_);
                                  active_txs_.emplace(tx_id, TxContext{tx_id, conn, deadline});
                              }

                              ILOG << "TransactionManager: BEGIN tx_id=" << tx_id
                                   << " timeout=" << timeout_ms << "ms";
                              cb(tx_id, "");
                          });
        });
}

std::shared_ptr<MySQLConnection> TransactionManager::getConnection(uint64_t tx_id)
{
    std::lock_guard<std::mutex> lk(mutex_);
    auto it = active_txs_.find(tx_id);
    if (it == active_txs_.end())
    {
        ELOG << "TransactionManager: tx_id=" << tx_id << " not found";
        return nullptr;
    }

    // 检查是否已超时
    if (std::chrono::steady_clock::now() > it->second.deadline)
    {
        ELOG << "TransactionManager: tx_id=" << tx_id << " timed out";
        return nullptr;
    }

    // 事务期间本类一直持有该连接，池不会把它再分配出去
    return it->second.conn;
}

void TransactionManager::finalizeAsync(uint64_t tx_id, bool do_commit, Callback cb)
{
    std::shared_ptr<MySQLConnection> conn;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = active_txs_.find(tx_id);
        if (it == active_txs_.end())
        {
            const std::string err = "tx_id=" + std::to_string(tx_id) + " not found";
            ELOG << "TransactionManager: " << err;
            cb(false, err);
            return;
        }
        conn = std::move(it->second.conn);
        active_txs_.erase(it);   // 之后不允许其他请求再拿到这条连接
    }

    const std::string sql  = do_commit ? "COMMIT" : "ROLLBACK";
    const std::string what = do_commit ? "COMMIT" : "ROLLBACK";

    agent_.submit(DbOpKind::Raw, sql, {}, conn,
                  [cb = std::move(cb), conn, tx_id, what](DbOpResult r) mutable
                  {
                      if (!r.ok)
                      {
                          ELOG << "TransactionManager: " << what << " failed tx_id=" << tx_id
                               << " err=" << r.err;
                      }
                      else
                      {
                          ILOG << "TransactionManager: " << what << " tx_id=" << tx_id;
                      }

                      conn.reset();   // 归还连接
                      cb(r.ok, r.err);
                  });
}

void TransactionManager::commitAsync(uint64_t tx_id, Callback cb)
{
    finalizeAsync(tx_id, true, std::move(cb));
}

void TransactionManager::rollbackAsync(uint64_t tx_id, Callback cb)
{
    finalizeAsync(tx_id, false, std::move(cb));
}

void TransactionManager::checkTimeouts()
{
    const auto now = std::chrono::steady_clock::now();
    std::vector<uint64_t> expired;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (const auto& kv : active_txs_)
        {
            if (now > kv.second.deadline) expired.push_back(kv.first);
        }
    }

    for (auto id : expired)
    {
        ELOG << "TransactionManager: tx_id=" << id << " timed out, auto ROLLBACK";
        rollbackAsync(id, [](bool, const std::string&) {});
    }
}
