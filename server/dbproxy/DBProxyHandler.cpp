#include "DBProxyHandler.h"
#include "GlobalSpace.h"
#include "bus/IBus.h"
#include "Log.h"
#include "ss_db.pb.h"
#include "ss_msg_id.pb.h"
#include "ss_err_code.pb.h"
#include <chrono>
#include <functional>
#include <utility>

namespace
{

ParamList toParams(const google::protobuf::RepeatedPtrField<ss::DBValue>& values)
{
    ParamList out;
    out.reserve(static_cast<size_t>(values.size()));
    for (const auto& v : values)
    {
        switch (v.value_case())
        {
        case ss::DBValue::kIntVal:
            out.emplace_back(static_cast<int64_t>(v.int_val()));
            break;
        case ss::DBValue::kDoubleVal:
            out.emplace_back(v.double_val());
            break;
        case ss::DBValue::kStrVal:
            out.emplace_back(v.str_val());
            break;
        case ss::DBValue::kBlobVal:
            out.emplace_back(std::vector<uint8_t>(v.blob_val().begin(), v.blob_val().end()));
            break;
        default:
            out.emplace_back();
            break;
        }
    }
    return out;
}

} // namespace

DBProxyHandler::DBProxyHandler(MySQLConnectionPool& pool, TransactionManager& txMgr,
                               AsyncMySql& agent, DbIoLoop& loop, DbWorkerPool& workers)
    : pool_(pool), txMgr_(txMgr), agent_(agent), loop_(loop), workers_(workers)
{
    registerHandlers();
}

void DBProxyHandler::registerHandlers()
{
    auto bus = GlobalSpace()->bus_;
    bus->SSRegistMessage(SSMsgID::SS_DB_QUERY_REQ,
        std::bind(&DBProxyHandler::onDBQuery, this, std::placeholders::_1));
    bus->SSRegistMessage(SSMsgID::SS_DB_EXEC_REQ,
        std::bind(&DBProxyHandler::onDBExec, this, std::placeholders::_1));
    bus->SSRegistMessage(SSMsgID::SS_DB_TX_BEGIN_REQ,
        std::bind(&DBProxyHandler::onDBTxBegin, this, std::placeholders::_1));
    bus->SSRegistMessage(SSMsgID::SS_DB_TX_COMMIT_REQ,
        std::bind(&DBProxyHandler::onDBTxCommit, this, std::placeholders::_1));
    bus->SSRegistMessage(SSMsgID::SS_DB_TX_ROLLBACK_REQ,
        std::bind(&DBProxyHandler::onDBTxRollback, this, std::placeholders::_1));
    ILOG << "DBProxyHandler: registered 5 SS DB message handlers";
}

// ── SELECT ────────────────────────────────────────────────────
void DBProxyHandler::onDBQuery(const AppMsg& msg)
{
    auto req = std::make_shared<ss::DBQueryReq>();
    if (!req->ParseFromArray(msg.data_, msg.data_len_))
    {
        ELOG << "DBQueryReq: failed to parse proto message";
        return;
    }

    const std::string sql   = req->request().sql();
    const uint64_t    tx_id = req->request().tx_id();
    ParamList         params = toParams(req->request().params());
    const AppMsg      header = msg; // Reply 只读包头，浅拷贝即可跨线程

    std::shared_ptr<MySQLConnection> tx_conn;
    if (tx_id != 0)
    {
        tx_conn = txMgr_.getConnection(tx_id);
        if (!tx_conn)
        {
            auto err_reply = std::make_shared<ss::DBQueryReq>();
            err_reply->mutable_response()->set_err(SSErrorCode::Error_db_tx_not_found);
            err_reply->mutable_response()->set_err_msg(
                "transaction not found or expired: tx_id=" + std::to_string(tx_id));
            workers_.post([header, err_reply] { GlobalSpace()->bus_->Reply(header, *err_reply); });
            return;
        }
    }

    loop_.post(
        [this, header, sql, params = std::move(params), tx_conn = std::move(tx_conn)]() mutable
        {
            agent_.submit(DbOpKind::Query, sql, std::move(params), std::move(tx_conn),
                          [this, header](DbOpResult r)
                          {
                              // 回调已在 DbWorkerPool 线程上：直接组装回包并 Reply
                              ss::DBQueryReq reply;
                              auto* resp = reply.mutable_response();
                              resp->set_err(SSErrorCode::Error_success);

                              if (!r.ok)
                              {
                                  resp->set_err(SSErrorCode::Error_db_query_failed);
                                  resp->set_err_msg(r.err);
                              }
                              else
                              {
                                  for (const auto& name : r.column_names)
                                      resp->add_columns()->set_name(name);

                                  for (size_t i = 0; i < r.rows.size(); ++i)
                                  {
                                      auto* row = resp->add_rows();
                                      const bool has_nulls = i < r.null_flags.size();
                                      for (size_t c = 0; c < r.rows[i].size(); ++c)
                                      {
                                          auto* val = row->add_values();
                                          if (has_nulls && r.null_flags[i][c]) val->set_null_val(true);
                                          else                                 val->set_str_val(r.rows[i][c]);
                                      }
                                  }
                              }

                              GlobalSpace()->bus_->Reply(header, reply);
                          });
        });
}

// ── INSERT / UPDATE / DELETE ──────────────────────────────────
void DBProxyHandler::onDBExec(const AppMsg& msg)
{
    auto req = std::make_shared<ss::DBExecReq>();
    if (!req->ParseFromArray(msg.data_, msg.data_len_))
    {
        ELOG << "DBExecReq: failed to parse proto message";
        return;
    }

    const std::string sql   = req->request().sql();
    const uint64_t    tx_id = req->request().tx_id();
    ParamList         params = toParams(req->request().params());
    const AppMsg      header = msg;

    std::shared_ptr<MySQLConnection> tx_conn;
    if (tx_id != 0)
    {
        tx_conn = txMgr_.getConnection(tx_id);
        if (!tx_conn)
        {
            auto err_reply = std::make_shared<ss::DBExecReq>();
            err_reply->mutable_response()->set_err(SSErrorCode::Error_db_tx_not_found);
            err_reply->mutable_response()->set_err_msg(
                "transaction not found or expired: tx_id=" + std::to_string(tx_id));
            workers_.post([header, err_reply] { GlobalSpace()->bus_->Reply(header, *err_reply); });
            return;
        }
    }

    loop_.post(
        [this, header, sql, params = std::move(params), tx_conn = std::move(tx_conn)]() mutable
        {
            agent_.submit(DbOpKind::Exec, sql, std::move(params), std::move(tx_conn),
                          [this, header](DbOpResult r)
                          {
                              ss::DBExecReq reply;
                              auto* resp = reply.mutable_response();
                              resp->set_err(SSErrorCode::Error_success);

                              if (!r.ok)
                              {
                                  resp->set_err(SSErrorCode::Error_db_exec_failed);
                                  resp->set_err_msg(r.err);
                              }
                              else
                              {
                                  resp->set_affected_rows(r.affected_rows);
                                  resp->set_last_insert_id(r.last_insert_id);
                              }

                              GlobalSpace()->bus_->Reply(header, reply);
                          });
        });
}

// ── 事务开始 ──
void DBProxyHandler::onDBTxBegin(const AppMsg& msg)
{
    auto req = std::make_shared<ss::DBTxBeginReq>();
    if (!req->ParseFromArray(msg.data_, msg.data_len_))
    {
        ELOG << "DBTxBeginReq: failed to parse proto message";
        return;
    }

    const uint64_t timeout_ms = req->request().timeout_ms();
    const AppMsg   header     = msg;

    txMgr_.beginAsync(timeout_ms,
        [header](uint64_t tx_id, const std::string& err)
        {
            ss::DBTxBeginReq reply;
            auto* resp = reply.mutable_response();
            resp->set_err(SSErrorCode::Error_success);

            if (tx_id == 0)
            {
                resp->set_err(SSErrorCode::Error_db_connection_failed);
                resp->set_err_msg(err);
            }
            else
            {
                resp->set_tx_id(tx_id);
            }

            GlobalSpace()->bus_->Reply(header, reply);
        });
}

// ── 事务提交 ───
void DBProxyHandler::onDBTxCommit(const AppMsg& msg)
{
    auto req = std::make_shared<ss::DBTxCommitReq>();
    if (!req->ParseFromArray(msg.data_, msg.data_len_))
    {
        ELOG << "DBTxCommitReq: failed to parse proto message";
        return;
    }

    const uint64_t tx_id = req->request().tx_id();
    const AppMsg   header = msg;

    txMgr_.commitAsync(tx_id,
        [header](bool ok, const std::string& err)
        {
            ss::DBTxCommitReq reply;
            auto* resp = reply.mutable_response();
            resp->set_err(SSErrorCode::Error_success);

            if (!ok)
            {
                resp->set_err(SSErrorCode::Error_db_exec_failed);
                resp->set_err_msg(err);
            }

            GlobalSpace()->bus_->Reply(header, reply);
        });
}

// ── 事务回滚 ──
void DBProxyHandler::onDBTxRollback(const AppMsg& msg)
{
    auto req = std::make_shared<ss::DBTxRollbackReq>();
    if (!req->ParseFromArray(msg.data_, msg.data_len_))
    {
        ELOG << "DBTxRollbackReq: failed to parse proto message";
        return;
    }

    const uint64_t tx_id = req->request().tx_id();
    const AppMsg   header = msg;

    txMgr_.rollbackAsync(tx_id,
        [header](bool ok, const std::string& err)
        {
            ss::DBTxRollbackReq reply;
            auto* resp = reply.mutable_response();
            resp->set_err(SSErrorCode::Error_success);

            if (!ok)
            {
                resp->set_err(SSErrorCode::Error_db_exec_failed);
                resp->set_err_msg(err);
            }

            GlobalSpace()->bus_->Reply(header, reply);
        });
}
