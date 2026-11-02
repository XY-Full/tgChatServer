#pragma once

#include "DbIoLoop.h"
#include "DbWorkerPool.h"
#include "MySQLConnection.h"
#include "MySQLConnectionPool.h"

#include <cstdint>
#include <functional>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct DbOpResult
{
    bool        ok  = false;
    std::string err;

    // Query
    std::vector<std::string>              column_names;
    std::vector<std::vector<std::string>> rows;
    std::vector<std::vector<bool>>        null_flags;

    // Exec
    uint64_t affected_rows  = 0;
    uint64_t last_insert_id = 0;
};

enum class DbOpKind
{
    Query, // SELECT：预处理 + 取结果集
    Exec,  // INSERT / UPDATE / DELETE
    Raw,   // 文本协议单条语句（事务控制）
};

/**
 * @brief 基于 MariaDB 非阻塞 API 的异步执行器（方案 3 的 "3"）
 *
 * 状态机：prepare → bind → execute → store_result，每一步若返回 MYSQL_WAIT_*
 * 就把 socket 事件交给 DbIoLoop，条件满足后从断点继续；期间只占用一个数据库连接，
 * 不占用任何线程。
 *
 * store_result 完成后（行已在客户端缓冲区），把"取行 + 组装结果"交给 DbWorkerPool，
 * 避免在 IO 线程上做 CPU 活 —— 这就是 3+2 里的 "2"。
 */
class AsyncMySql
{
public:
    AsyncMySql(MySQLConnectionPool& pool, DbIoLoop& loop, DbWorkerPool& workers,
               int socket_wait_ms = kDefaultSocketWaitMs);

    // 单次 socket 等待上限（超时后以 MYSQL_WAIT_TIMEOUT 唤醒，由库侧报错）
    static constexpr int kDefaultSocketWaitMs = 10000;

    using Callback = std::function<void(DbOpResult)>;

    // 提交一次异步操作；tx_conn 非空时复用该连接（事务内）。
    // 可从任意线程调用：内部会把执行投递到 IO 循环线程。
    void submit(DbOpKind kind, std::string sql, ParamList params,
                std::shared_ptr<MySQLConnection> tx_conn, Callback cb);

    // 在飞操作数上限（业务限流维度）；0 表示不限
    void setMaxInflight(int n) { max_inflight_ = n; }
    int  inflight() const { return inflight_.load(); }

private:
    struct Op;
    using OpPtr = std::shared_ptr<Op>;

    void begin(OpPtr op);
    void doPrepare(OpPtr op, int status);
    void doExecute(OpPtr op, int status);
    void doStoreResult(OpPtr op, int status);
    void doRawQuery(OpPtr op, int status);

    // 在工作线程上取行并收尾（store_result 之后 fetch 是纯本地操作）
    void collectRows(OpPtr op);
    void finish(OpPtr op, bool ok, std::string err);
    void waitFd(const OpPtr& op, int mask, std::function<void(int status)> resume);

    MySQLConnectionPool& pool_;
    DbIoLoop&            loop_;
    DbWorkerPool&        workers_;
    int                  socket_wait_ms_;

    std::atomic<int>     inflight_{0};   // 当前在飞操作数（DB 并发观测 + 限流）
    int                  max_inflight_ = 0;

};
