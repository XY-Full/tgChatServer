#include "TransactionManager.h"
#include "Log.h"

TransactionManager::TransactionManager(MySQLConnectionPool& pool, uint64_t default_timeout_ms)
    : pool_(pool), default_timeout_ms_(default_timeout_ms)
{
}

uint64_t TransactionManager::begin(uint64_t timeout_ms, std::string& err_msg)
{
    if (timeout_ms == 0) timeout_ms = default_timeout_ms_;

    auto conn = pool_.acquire(5000 /* 获取连接超时 5s */);
    if (!conn)
    {
        err_msg = "failed to acquire connection from pool";
        return 0;
    }

    if (!conn->beginTransaction(err_msg))
    {
        return 0;
    }

    uint64_t tx_id = tx_id_gen_.fetch_add(1, std::memory_order_relaxed);
    auto deadline  = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    std::lock_guard<std::mutex> lk(mutex_);
    active_txs_.emplace(tx_id, TxContext{tx_id, std::move(conn), deadline});

    ILOG << "TransactionManager: BEGIN tx_id=" << tx_id << " timeout=" << timeout_ms << "ms";
    return tx_id;
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

    return it->second.conn;
}

bool TransactionManager::finalize(uint64_t tx_id, bool do_commit, std::string& err_msg)
{
    std::shared_ptr<MySQLConnection> conn;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = active_txs_.find(tx_id);
        if (it == active_txs_.end())
        {
            err_msg = "tx_id=" + std::to_string(tx_id) + " not found";
            ELOG << "TransactionManager: " << err_msg;
            return false;
        }
        conn = std::move(it->second.conn);
        active_txs_.erase(it);
    }

    bool ok = do_commit ? conn->commit(err_msg) : conn->rollback(err_msg);
    // conn 析构时自动归还连接到池
    if (ok)
    {
        ILOG << "TransactionManager: " << (do_commit ? "COMMIT" : "ROLLBACK")
             << " tx_id=" << tx_id;
    }
    else
    {
        ELOG << "TransactionManager: " << (do_commit ? "COMMIT" : "ROLLBACK")
             << " failed tx_id=" << tx_id << " err=" << err_msg;
    }
    return ok;
}

bool TransactionManager::commit(uint64_t tx_id, std::string& err_msg)
{
    return finalize(tx_id, true, err_msg);
}

bool TransactionManager::rollback(uint64_t tx_id, std::string& err_msg)
{
    return finalize(tx_id, false, err_msg);
}

void TransactionManager::checkTimeouts()
{
    auto now = std::chrono::steady_clock::now();
    std::vector<uint64_t> expired;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto& [id, ctx] : active_txs_)
        {
            if (now > ctx.deadline)
                expired.push_back(id);
        }
    }

    for (auto id : expired)
    {
        ELOG << "TransactionManager: tx_id=" << id << " timed out, auto ROLLBACK";
        std::string err;
        rollback(id, err);
    }
}
