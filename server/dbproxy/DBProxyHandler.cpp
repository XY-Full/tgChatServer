#include "DBProxyHandler.h"
#include "GlobalSpace.h"
#include "bus/IBus.h"
#include "Log.h"
#include "ss_db.pb.h"
#include "ss_msg_id.pb.h"
#include "ss_err_code.pb.h"
#include <functional>

// ── 内部辅助宏（仅在此文件使用）──
// SS 协议的消息处理模板：解析请求、准备响应、统一 Reply
#define SS_PROCESS_BEGIN(MSG_TYPE)                                          \
    auto recvMsg  = std::make_shared<ss::MSG_TYPE>();                       \
    if (!recvMsg->ParseFromArray(msg.data_, msg.data_len_))                 \
    {                                                                       \
        ELOG << #MSG_TYPE ": failed to parse proto message";               \
        return;                                                             \
    }                                                                       \
    DLOG << #MSG_TYPE " req: " << recvMsg->request().ShortDebugString();   \
    auto& request  = recvMsg->request();                                    \
    auto  replyMsg = std::make_shared<ss::MSG_TYPE>();                      \
    auto  response = replyMsg->mutable_response();                          \
    response->set_err(SSErrorCode::Error_success);                          \
    do                                                                      \
    {

#define SS_PROCESS_END()                                                    \
    }                                                                       \
    while (0);                                                              \
    if (response->err() != SSErrorCode::Error_success)                      \
    {                                                                       \
        ELOG << "response err=" << SSErrorCode_Name(response->err())        \
             << " msg=" << response->err_msg();                             \
    }                                                                       \
    GlobalSpace()->bus_->Reply(msg, *replyMsg);

// ────────────────────────────────────────────────────────────────────────

DBProxyHandler::DBProxyHandler(MySQLConnectionPool& pool, TransactionManager& txMgr)
    : pool_(pool), txMgr_(txMgr)
{
    registerHandlers();
}

void DBProxyHandler::registerHandlers()
{
    auto bus = GlobalSpace()->bus_;
    bus->RegistMessage(SSMsgID::SS_DB_QUERY_REQ,
        std::bind(&DBProxyHandler::onDBQuery, this, std::placeholders::_1));
    bus->RegistMessage(SSMsgID::SS_DB_EXEC_REQ,
        std::bind(&DBProxyHandler::onDBExec, this, std::placeholders::_1));
    bus->RegistMessage(SSMsgID::SS_DB_TX_BEGIN_REQ,
        std::bind(&DBProxyHandler::onDBTxBegin, this, std::placeholders::_1));
    bus->RegistMessage(SSMsgID::SS_DB_TX_COMMIT_REQ,
        std::bind(&DBProxyHandler::onDBTxCommit, this, std::placeholders::_1));
    bus->RegistMessage(SSMsgID::SS_DB_TX_ROLLBACK_REQ,
        std::bind(&DBProxyHandler::onDBTxRollback, this, std::placeholders::_1));
    ILOG << "DBProxyHandler: registered 5 SS DB message handlers";
}

// ── SELECT 查询 ──────────────────────────────────────────────────────────
void DBProxyHandler::onDBQuery(const AppMsg& msg)
{
    SS_PROCESS_BEGIN(DBQueryReq)

    const std::string& sql   = request.sql();
    uint64_t           tx_id = request.tx_id();

    std::shared_ptr<MySQLConnection> conn;
    if (tx_id != 0)
    {
        conn = txMgr_.getConnection(tx_id);
        if (!conn)
        {
            response->set_err(SSErrorCode::Error_db_tx_not_found);
            response->set_err_msg("transaction not found or expired: tx_id=" + std::to_string(tx_id));
            break;
        }
    }
    else
    {
        conn = pool_.acquire(5000);
        if (!conn)
        {
            response->set_err(SSErrorCode::Error_db_connection_failed);
            response->set_err_msg("failed to acquire connection from pool");
            break;
        }
    }

    MySQLConnection::QueryResult result;
    std::string err_msg;
    if (!conn->query(sql, result, err_msg))
    {
        response->set_err(SSErrorCode::Error_db_query_failed);
        response->set_err_msg(err_msg);
        break;
    }

    // 填充列元数据
    for (const auto& col_name : result.column_names)
    {
        auto* col = response->add_columns();
        col->set_name(col_name);
    }
    // 补充列类型（index 对应）
    for (size_t i = 0; i < result.column_types.size() && i < (size_t)response->columns_size(); ++i)
    {
        response->mutable_columns(static_cast<int>(i))->set_type(result.column_types[i]);
    }

    // 填充行数据
    for (size_t r = 0; r < result.rows.size(); ++r)
    {
        auto* row = response->add_rows();
        const auto& row_data   = result.rows[r];
        const auto& null_flags = result.null_flags[r];
        for (size_t c = 0; c < row_data.size(); ++c)
        {
            auto* val = row->add_values();
            if (null_flags[c])
            {
                val->set_null_val(true);
            }
            else
            {
                val->set_str_val(row_data[c]);
            }
        }
    }

    SS_PROCESS_END()
}

// ── INSERT / UPDATE / DELETE ─────────────────────────────────────────────
void DBProxyHandler::onDBExec(const AppMsg& msg)
{
    SS_PROCESS_BEGIN(DBExecReq)

    const std::string& sql   = request.sql();
    uint64_t           tx_id = request.tx_id();

    std::shared_ptr<MySQLConnection> conn;
    if (tx_id != 0)
    {
        conn = txMgr_.getConnection(tx_id);
        if (!conn)
        {
            response->set_err(SSErrorCode::Error_db_tx_not_found);
            response->set_err_msg("transaction not found or expired: tx_id=" + std::to_string(tx_id));
            break;
        }
    }
    else
    {
        conn = pool_.acquire(5000);
        if (!conn)
        {
            response->set_err(SSErrorCode::Error_db_connection_failed);
            response->set_err_msg("failed to acquire connection from pool");
            break;
        }
    }

    MySQLConnection::ExecResult result;
    std::string err_msg;
    if (!conn->execute(sql, result, err_msg))
    {
        response->set_err(SSErrorCode::Error_db_exec_failed);
        response->set_err_msg(err_msg);
        break;
    }

    response->set_affected_rows(result.affected_rows);
    response->set_last_insert_id(result.last_insert_id);

    SS_PROCESS_END()
}

// ── 事务开始 ─────────────────────────────────────────────────────────────
void DBProxyHandler::onDBTxBegin(const AppMsg& msg)
{
    SS_PROCESS_BEGIN(DBTxBeginReq)

    std::string err_msg;
    uint64_t tx_id = txMgr_.begin(request.timeout_ms(), err_msg);
    if (tx_id == 0)
    {
        response->set_err(SSErrorCode::Error_db_connection_failed);
        response->set_err_msg(err_msg);
        break;
    }
    response->set_tx_id(tx_id);

    SS_PROCESS_END()
}

// ── 事务提交 ─────────────────────────────────────────────────────────────
void DBProxyHandler::onDBTxCommit(const AppMsg& msg)
{
    SS_PROCESS_BEGIN(DBTxCommitReq)

    std::string err_msg;
    if (!txMgr_.commit(request.tx_id(), err_msg))
    {
        response->set_err(SSErrorCode::Error_db_exec_failed);
        response->set_err_msg(err_msg);
        break;
    }

    SS_PROCESS_END()
}

// ── 事务回滚 ─────────────────────────────────────────────────────────────
void DBProxyHandler::onDBTxRollback(const AppMsg& msg)
{
    SS_PROCESS_BEGIN(DBTxRollbackReq)

    std::string err_msg;
    if (!txMgr_.rollback(request.tx_id(), err_msg))
    {
        response->set_err(SSErrorCode::Error_db_exec_failed);
        response->set_err_msg(err_msg);
        break;
    }

    SS_PROCESS_END()
}
