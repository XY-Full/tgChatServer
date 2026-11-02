#pragma once

// ─────────────────────────────────────────────────────────────────────────
// DBProxy：数据库访问代理服务
//
// 三个互相独立的并发控制维度
//
//  1. IO 并发 —— DbIoLoop + 非阻塞客户端 API
//     一次查询只占一条连接、不占线程；等待由 epoll 驱动。与线程数无关
//     换驱动只需替换 AsyncMySql：它对上层只暴露 提交 SQL + 参数 -> 结果
//
//  2. 业务并发 —— CoroutineScheduler（协程调度器）
//     消息处理跑在协程里，handler 提交异步操作后立即返回，不阻塞调度线程
//
//  3. DB 并发 —— 连接池大小 + MySQL 容量 + 业务限流
//     db_pool_size / db_max_pool_size 决定同时在飞的数据库操作数
//     db_max_inflight 可额外限流
//     DbWorkerPool 只做回包这类 CPU 活，不限制数据库并发，结果取完后连接立即归还，
//     工作线程忙只会延迟回包，不会占住连接
//
// 事务：BEGIN 到 COMMIT/ROLLBACK 全程独占一条连接，中间协程挂起也不会放回池里
// ─────────────────────────────────────────────────────────────────────────

#include "AsyncMySql.h"
#include "DBProxyHandler.h"
#include "DbIoLoop.h"
#include "DbWorkerPool.h"
#include "MySQLConnectionPool.h"
#include "TransactionManager.h"
#include "app/IApp.h"
#include "bus/IBus.h"
#include "GlobalSpace.h"
#include "Log.h"
#include <memory>

class DBProxyApp : public IApp
{
public:
    DBProxyApp() : IApp("DBProxyService") {}

    static DBProxyApp& getInstance()
    {
        static DBProxyApp instance;
        return instance;
    }

    virtual bool onInit() override final
    {
        // 1. 读取数据库配置
        auto& cfg = getContext();
        MySQLConfig db_cfg;
        db_cfg.host             = cfg.getValue<std::string>("db_host",     "127.0.0.1");
        db_cfg.port             = static_cast<uint16_t>(cfg.getValue<int>("db_port", 3306));
        db_cfg.user             = cfg.getValue<std::string>("db_user",     "root");
        db_cfg.password         = cfg.getValue<std::string>("db_password", "");
        db_cfg.database         = cfg.getValue<std::string>("db_name",     "");
        db_cfg.connect_timeout_ms = cfg.getValue<int>("db_connect_timeout_ms", 5000);

        int pool_size     = cfg.getValue<int>("db_pool_size",     8);
        int max_pool_size = cfg.getValue<int>("db_max_pool_size", 32);
        uint64_t tx_timeout_ms = static_cast<uint64_t>(cfg.getValue<int>("db_tx_timeout_ms", 30000));

        // 2. 初始化连接池和事务管理器
        pool_     = std::make_unique<MySQLConnectionPool>(db_cfg, pool_size, max_pool_size);
        if (pool_->idleCount() == 0)
        {
            ELOG << "DBProxyApp: failed to establish any db connection ("
                 << db_cfg.host << ":" << db_cfg.port << "/" << db_cfg.database
                 << "), refusing to start";
            return false;
        }
        // 异步执行器
        agent_  = std::make_unique<AsyncMySql>(*pool_, *loop_, *workers_);
        agent_->setMaxInflight(cfg.getValue<int>("db_max_inflight", 0));

        tx_mgr_   = std::make_unique<TransactionManager>(*agent_, *pool_, tx_timeout_ms);

        // 3. 启动 DB 事件循环与worker线程池
        loop_ = std::make_unique<DbIoLoop>();
        if (!loop_->start())
        {
            ELOG << "DBProxyApp: failed to start db io loop";
            return false;
        }

        const int worker_count = cfg.getValue<int>("db_worker_threads", max_pool_size);
        workers_ = std::make_unique<DbWorkerPool>(worker_count);
        workers_->start();

        // 4. 注册所有 SS DB 消息处理器
        handler_  = std::make_unique<DBProxyHandler>(*pool_, *tx_mgr_, *agent_, *loop_, *workers_);

        // 5. 启动 Bus
        if (!GlobalSpace()->bus_->Start())
        {
            ELOG << "DBProxyApp: bus start failed";
            return false;
        }

        ILOG << "DBProxyApp: initialized. pool=" << pool_->idleCount() << "/" << pool_size
             << " max=" << max_pool_size << " db=" << db_cfg.database;
        return true;
    }

    virtual void onTick(uint32_t /*delta_ms*/) override final
    {
        // 定期健康检查和事务超时清理
        static uint32_t tick_counter = 0;
        ++tick_counter;

        // 每 30 秒进行一次连接池健康检查
        if (tick_counter % 30 == 0 && pool_)
            pool_->healthCheck();

        // 每秒检查事务超时
        if (tx_mgr_)
            tx_mgr_->checkTimeouts();
    }

    virtual void onCleanup() override final
    {
        handler_.reset();
        agent_.reset();
        if (workers_) workers_->stop();
        if (loop_)    loop_->stop();
        tx_mgr_.reset();
        if (pool_) pool_->shutdown();
        ILOG << "DBProxyApp: cleanup complete";
    }

    virtual bool onReload() override final
    {
        ILOG << "DBProxyApp: reloading configuration (connection pool size unchanged)";
        return true;
    }

    virtual bool onMessageLoop() override final
    {
        return true;
    }

private:
    std::unique_ptr<MySQLConnectionPool> pool_;
    std::unique_ptr<TransactionManager>  tx_mgr_;
    std::unique_ptr<DbIoLoop>            loop_;    // 非阻塞 MySQL 的事件循环
    std::unique_ptr<DbWorkerPool>        workers_; // 取行 / 组装回包 / 事务控制
    std::unique_ptr<AsyncMySql>          agent_;   // 异步执行器
    std::unique_ptr<DBProxyHandler>      handler_;
};
