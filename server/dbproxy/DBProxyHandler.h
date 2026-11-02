#pragma once

#include "AsyncMySql.h"
#include "DbIoLoop.h"
#include "DbWorkerPool.h"
#include "MySQLConnectionPool.h"
#include "TransactionManager.h"
#include "network/AppMsg.h"
#include <functional>

// DBProxy 消息处理器
class DBProxyHandler
{
public:
    DBProxyHandler(MySQLConnectionPool& pool, TransactionManager& txMgr,
                   AsyncMySql& agent, DbIoLoop& loop, DbWorkerPool& workers);
    ~DBProxyHandler() = default;

    void registerHandlers();

private:
    void onDBQuery(const AppMsg& msg);
    void onDBExec(const AppMsg& msg);
    void onDBTxBegin(const AppMsg& msg);
    void onDBTxCommit(const AppMsg& msg);
    void onDBTxRollback(const AppMsg& msg);

    MySQLConnectionPool& pool_;
    TransactionManager&  txMgr_;
    AsyncMySql&          agent_;
    DbIoLoop&            loop_;
    DbWorkerPool&        workers_;
};
