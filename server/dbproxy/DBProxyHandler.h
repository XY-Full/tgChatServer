#pragma once

#include "MySQLConnectionPool.h"
#include "TransactionManager.h"
#include "network/AppMsg.h"
#include <functional>

// DBProxy 消息处理器
// 向 Bus 注册所有 SS_DB_* 消息，将数据库操作委托给连接池和事务管理器。
class DBProxyHandler
{
public:
    DBProxyHandler(MySQLConnectionPool& pool, TransactionManager& txMgr);
    ~DBProxyHandler() = default;

    // 向 GlobalSpace()->bus_ 注册所有 SS_DB_* 消息处理器（构造函数中调用）
    void registerHandlers();

private:
    // SS_DB_QUERY_REQ = 5  —— SELECT 查询
    void onDBQuery(const AppMsg& msg);

    // SS_DB_EXEC_REQ = 7   —— INSERT/UPDATE/DELETE 执行
    void onDBExec(const AppMsg& msg);

    // SS_DB_TX_BEGIN_REQ = 9  —— 事务开始
    void onDBTxBegin(const AppMsg& msg);

    // SS_DB_TX_COMMIT_REQ = 11  —— 事务提交
    void onDBTxCommit(const AppMsg& msg);

    // SS_DB_TX_ROLLBACK_REQ = 13  —— 事务回滚
    void onDBTxRollback(const AppMsg& msg);

    MySQLConnectionPool& pool_;
    TransactionManager&  txMgr_;
};
