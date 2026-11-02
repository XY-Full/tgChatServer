#include "AsyncMySql.h"

#include "Log.h"

#include <mariadb/errmsg.h>

#include <algorithm>
#include <cstring>

// 单次 socket 等待上限：超时后以 MYSQL_WAIT_TIMEOUT 唤醒，由 MariaDB 侧报错
static constexpr unsigned long kMaxColumnBuffer = 4 * 1024 * 1024;

struct AsyncMySql::Op
{
    DbOpKind     kind   = DbOpKind::Exec;
    std::string  sql;
    ParamList    params;
    Callback     cb;

    std::shared_ptr<MySQLConnection> conn;
    MYSQL_STMT*                      stmt = nullptr;
    bool                             stmt_is_new = false;

    // 参数绑定
    std::vector<MYSQL_BIND>    param_binds;
    std::vector<unsigned long> param_lengths;
    std::vector<my_bool>       param_nulls;

    // 结果绑定
    MYSQL_RES*                    meta = nullptr;
    std::vector<std::vector<char>> buffers;
    std::vector<MYSQL_BIND>       result_binds;
    std::vector<unsigned long>    result_lengths;
    std::vector<my_bool>          result_nulls;

    DbOpResult result;

    int waiting_fd = 0;   // 正在等待事件的 socket fd，用完后需要注意取消注册
};

AsyncMySql::AsyncMySql(MySQLConnectionPool& pool, DbIoLoop& loop, DbWorkerPool& workers,
                       int socket_wait_ms)
    : pool_(pool), loop_(loop), workers_(workers), socket_wait_ms_(socket_wait_ms)
{
}

void AsyncMySql::submit(DbOpKind kind, std::string sql, ParamList params,
                        std::shared_ptr<MySQLConnection> tx_conn, Callback cb)
{
    // 业务限流：在飞操作数超过上限时直接拒绝，避免无界排队把延迟堆到业务侧
    if (max_inflight_ > 0 && inflight_.load() >= max_inflight_)
    {
        DbOpResult rejected;
        rejected.ok  = false;
        rejected.err = "too many concurrent db operations (limit=" + std::to_string(max_inflight_) + ")";
        ELOG << "AsyncMySql: " << rejected.err << " sql=" << sql;
        cb(std::move(rejected));
        return;
    }

    auto op = std::make_shared<Op>();
    op->kind   = kind;
    op->sql    = std::move(sql);
    op->params = std::move(params);
    op->cb     = std::move(cb);
    op->conn   = std::move(tx_conn);

    ++inflight_;

    // 真正的执行统一放到 IO 循环线程上
    loop_.post([this, op] { begin(op); });
}

void AsyncMySql::begin(OpPtr op)
{
    if (op->kind == DbOpKind::Raw && op->conn)
    {
        doRawQuery(op, 0);
        return;
    }

    if (op->conn)
    {
        doPrepare(op, 0);
        return;
    }

    auto conn = pool_.tryAcquire();
    if (conn)
    {
        op->conn = std::move(conn);
        if (op->kind == DbOpKind::Raw) doRawQuery(op, 0);
        else                           doPrepare(op, 0);
        return;
    }

    // 如果暂时没有空闲连接，则异步等待获取连接
    pool_.asyncAcquire(
        [this, op](std::shared_ptr<MySQLConnection> conn)
        {
            if (!conn)
            {
                loop_.post([this, op] { finish(op, false, "failed to acquire db connection"); });
                return;
            }
            op->conn = std::move(conn);
            loop_.post([this, op]
            {
                if (op->kind == DbOpKind::Raw) doRawQuery(op, 0);
                else                           doPrepare(op, 0);
            });
        });
}

void AsyncMySql::waitFd(const OpPtr& op, int mask, std::function<void(int status)> resume)
{
    op->waiting_fd = op->conn->socketFd();

    // 协作式调度，库方只给自己需要的操作掩码，调用方根据掩码进行监听，当监听到对应行为时，继续调用库方的cont类函数
    int wait_ms = socket_wait_ms_;
    // 库方需要超时，则给定时器
    if (mask & MYSQL_WAIT_TIMEOUT)
    {
        const unsigned int lib_ms = mysql_get_timeout_value_ms(op->conn->raw());
        if (lib_ms > 0) wait_ms = static_cast<int>(lib_ms);
    }

    // 掩码原样注册，库要哪些事件就等哪些；不带 TIMEOUT 就不挂定时器
    // 到点后把收到的 status 交给各个 _cont，由库判定是否超时
    loop_.waitSocket(op->waiting_fd, mask, wait_ms, std::move(resume));
}

void AsyncMySql::doPrepare(OpPtr op, int status)
{
    int ret = 0;
    int rc  = 0;

    if (status == 0)
    {
        // 一条连接同时只能被一个操作持有
        op->conn->markBusy();

        MYSQL_STMT* cached = op->conn->findStatement(op->sql);
        if (cached)
        {
            op->stmt = cached;
            doExecute(op, 0);
            return;
        }

        op->stmt = mysql_stmt_init(op->conn->raw());
        if (!op->stmt)
        {
            finish(op, false, "mysql_stmt_init failed");
            return;
        }
        op->stmt_is_new = true;
        rc = mysql_stmt_prepare_start(&ret, op->stmt, op->sql.c_str(),
                                      static_cast<unsigned long>(op->sql.size()));
    }
    else
    {
        rc = mysql_stmt_prepare_cont(&ret, op->stmt, status);
    }

    if (rc != 0)
    {
        waitFd(op, rc, [this, op](int st) { doPrepare(op, st); });
        return;
    }

    if (ret != 0)
    {
        finish(op, false, mysql_stmt_error(op->stmt));
        return;
    }

    op->conn->cacheStatement(op->sql, op->stmt);
    op->stmt_is_new = false;
    doExecute(op, 0);
}

void AsyncMySql::doExecute(OpPtr op, int status)
{
    if (status == 0)
    {
        std::string err;
        if (!bindStmtParams(op->stmt, op->params, op->param_binds, op->param_lengths, op->param_nulls, err))
        {
            finish(op, false, err);
            return;
        }
    }

    int ret = 0;
    const int rc = (status == 0) ? mysql_stmt_execute_start(&ret, op->stmt)
                                 : mysql_stmt_execute_cont(&ret, op->stmt, status);

    if (rc != 0)
    {
        waitFd(op, rc, [this, op](int st) { doExecute(op, st); });
        return;
    }

    if (ret != 0)
    {
        finish(op, false, mysql_stmt_error(op->stmt));
        return;
    }

    op->result.affected_rows  = static_cast<uint64_t>(mysql_stmt_affected_rows(op->stmt));
    op->result.last_insert_id = static_cast<uint64_t>(mysql_stmt_insert_id(op->stmt));

    if (op->kind != DbOpKind::Query)
    {
        finish(op, true, "");
        return;
    }

    // 结果缓冲按列长度分配
    op->meta = mysql_stmt_result_metadata(op->stmt);
    if (!op->meta)
    {
        finish(op, false, "statement returns no result set");
        return;
    }

    const unsigned int ncols  = mysql_num_fields(op->meta);
    MYSQL_FIELD*       fields = mysql_fetch_fields(op->meta);

    op->buffers.resize(ncols);
    op->result_binds.assign(ncols, MYSQL_BIND{});
    op->result_lengths.assign(ncols, 0);
    op->result_nulls.assign(ncols, 0);

    for (unsigned int i = 0; i < ncols; ++i)
    {
        op->result.column_names.emplace_back(fields[i].name);

        unsigned long cap = fields[i].length + 1;
        if (cap > kMaxColumnBuffer) cap = kMaxColumnBuffer;
        op->buffers[i].resize(cap);

        MYSQL_BIND& b = op->result_binds[i];
        std::memset(&b, 0, sizeof(b));
        b.buffer_type   = MYSQL_TYPE_STRING;
        b.buffer        = op->buffers[i].data();
        b.buffer_length = cap;
        b.length        = &op->result_lengths[i];
        b.is_null       = &op->result_nulls[i];
    }

    if (ncols > 0 && mysql_stmt_bind_result(op->stmt, op->result_binds.data()) != 0)
    {
        finish(op, false, mysql_stmt_error(op->stmt));
        return;
    }

    doStoreResult(op, 0);
}

void AsyncMySql::doStoreResult(OpPtr op, int status)
{
    int ret = 0;
    const int rc = (status == 0) ? mysql_stmt_store_result_start(&ret, op->stmt)
                                 : mysql_stmt_store_result_cont(&ret, op->stmt, status);

    if (rc != 0)
    {
        waitFd(op, rc, [this, op](int st) { doStoreResult(op, st); });
        return;
    }

    if (ret != 0)
    {
        finish(op, false, mysql_stmt_error(op->stmt));
        return;
    }

    // 行已在客户端缓冲区，取行只是 memcpy：放在 IO 线程完成，
    // 这样连接在取行后立刻归还，DB 并发只由连接池决定，不受工作线程数量影响
    collectRows(op);
}

void AsyncMySql::collectRows(OpPtr op)
{
    // 运行在 IO 线程：连接由本 op 独占，且随后立即归还
    while (true)
    {
        const int fr = mysql_stmt_fetch(op->stmt);
        if (fr == MYSQL_NO_DATA) break;
        if (fr != 0 && fr != MYSQL_DATA_TRUNCATED)
        {
            mysql_stmt_free_result(op->stmt);
            if (op->meta) { mysql_free_result(op->meta); op->meta = nullptr; }
            finish(op, false, mysql_stmt_error(op->stmt));
            return;
        }

        const size_t ncols = op->buffers.size();
        std::vector<std::string> row(ncols);
        std::vector<bool>        nulls(ncols, false);
        for (size_t i = 0; i < ncols; ++i)
        {
            if (op->result_nulls[i])
            {
                nulls[i] = true;
                continue;
            }
            const unsigned long len = std::min(op->result_lengths[i],
                                               static_cast<unsigned long>(op->buffers[i].size()));
            row[i].assign(op->buffers[i].data(), len);
        }
        op->result.rows.push_back(std::move(row));
        op->result.null_flags.push_back(std::move(nulls));
    }

    mysql_stmt_free_result(op->stmt);
    finish(op, true, "");
}

void AsyncMySql::doRawQuery(OpPtr op, int status)
{
    int ret = 0;
    const int rc = (status == 0)
                       ? mysql_real_query_start(&ret, op->conn->raw(), op->sql.c_str(),
                                                static_cast<unsigned long>(op->sql.size()))
                       : mysql_real_query_cont(&ret, op->conn->raw(), status);

    if (rc != 0)
    {
        waitFd(op, rc, [this, op](int st) { doRawQuery(op, st); });
        return;
    }

    if (ret != 0)
    {
        finish(op, false, mysql_error(op->conn->raw()));
        return;
    }

    op->result.affected_rows  = static_cast<uint64_t>(mysql_affected_rows(op->conn->raw()));
    op->result.last_insert_id = static_cast<uint64_t>(mysql_insert_id(op->conn->raw()));
    finish(op, true, "");
}

void AsyncMySql::finish(OpPtr op, bool ok, std::string err)
{
    // 若本操作还在等事件，必须先取消注册：连接随后可能被销毁、fd 也会被复用
    if (op->waiting_fd > 0)
    {
        loop_.cancelWait(op->waiting_fd);
        op->waiting_fd = 0;
    }

    if (op->meta)
    {
        mysql_free_result(op->meta);
        op->meta = nullptr;
    }
    if (op->stmt && op->stmt_is_new)
    {
        mysql_stmt_close(op->stmt);
        op->stmt = nullptr;
        op->stmt_is_new = false;
    }

    op->result.ok  = ok;
    op->result.err = std::move(err);

    if (!ok)
    {
        if (op->conn)
        {
            op->conn->dropStatement(op->sql);
            // 连接级错误（服务端断开 / KILL / 网络中断）时把连接标记作废，避免回池后继续使用
            // 失败后主动探活，只要连接已经不可用就作废回收
            // 客户端级错误（errno >= 2000）通常已说明连接断了
            // SQL 级错误（errno >= 1000）则以 ping 结果为准
            bool dead = (mysql_errno(op->conn->raw()) >= CR_MIN_ERROR);
            if (!dead) dead = !op->conn->ping();
            if (dead) op->conn->markBroken();
        }
        ELOG << "AsyncMySql: op failed: " << op->result.err << " sql=" << op->sql;
    }

    // 先归还连接，再把CPU密集任务交给工作线程：
    // 工作线程池只影响回包延迟，不影响数据库并发
    if (op->conn)
    {
        op->conn->markIdle();
        op->conn.reset();
    }
    --inflight_;

    if (op->cb)
    {
        auto cb = std::move(op->cb);
        workers_.post([cb = std::move(cb), r = std::move(op->result)]() mutable { cb(std::move(r)); });
    }
}
