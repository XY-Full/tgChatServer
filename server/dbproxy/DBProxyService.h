#pragma once

#include "DBProxyHandler.h"
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
        // 1. 启动 Bus（向 Center 注册并建立 SS 通信）
        GlobalSpace()->bus_->Start();

        // 2. 读取数据库配置
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

        // 3. 初始化连接池和事务管理器
        pool_     = std::make_unique<MySQLConnectionPool>(db_cfg, pool_size, max_pool_size);
        tx_mgr_   = std::make_unique<TransactionManager>(*pool_, tx_timeout_ms);

        // 4. 注册所有 SS DB 消息处理器
        handler_  = std::make_unique<DBProxyHandler>(*pool_, *tx_mgr_);

        ILOG << "DBProxyApp: initialized. pool=" << pool_size
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
    std::unique_ptr<DBProxyHandler>      handler_;
};
