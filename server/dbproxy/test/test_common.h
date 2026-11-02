#pragma once

// dbproxy 集成测试公共头：需要一台可用的 MySQL（默认 127.0.0.1:3306，root / CHANGE_ME_production_db_password）
// 可用环境变量覆盖：TEST_DB_HOST / TEST_DB_PORT / TEST_DB_USER / TEST_DB_PASS / TEST_DB_NAME

#include "AsyncMySql.h"
#include "DbIoLoop.h"
#include "DbWorkerPool.h"
#include "MySQLConnection.h"
#include "MySQLConnectionPool.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

inline std::string envOr(const char* key, const char* def)
{
    const char* v = std::getenv(key);
    return (v && *v) ? std::string(v) : std::string(def);
}

inline MySQLConfig testConfig()
{
    MySQLConfig cfg;
    cfg.host              = envOr("TEST_DB_HOST", "127.0.0.1");
    cfg.port              = static_cast<uint16_t>(std::stoi(envOr("TEST_DB_PORT", "3306")));
    cfg.user              = envOr("TEST_DB_USER", "root");
    cfg.password          = envOr("TEST_DB_PASS", "CHANGE_ME_production_db_password");
    cfg.database          = envOr("TEST_DB_NAME", "tgchat");
    cfg.connect_timeout_ms = 5000;
    return cfg;
}

// 一个测试用的执行环境：IO 循环 + 工作线程池 + 异步执行器 + 连接池
struct TestEnv
{
    MySQLConnectionPool pool;
    DbIoLoop            loop;
    DbWorkerPool        workers;
    AsyncMySql          agent;

    explicit TestEnv(int pool_size = 4, int max_pool = 8, int wait_ms = 10000,
                     int read_timeout_ms = 0)
        : pool(makeConfig(read_timeout_ms), pool_size, max_pool)
        , workers(2)
        , agent(pool, loop, workers, wait_ms)
    {
        if (!loop.start()) throw std::runtime_error("DbIoLoop start failed");
        workers.start();
    }

    static MySQLConfig makeConfig(int read_timeout_ms)
    {
        MySQLConfig cfg = testConfig();
        cfg.read_timeout_ms = read_timeout_ms;
        return cfg;
    }

    ~TestEnv()
    {
        loop.stop();
        workers.stop();
        pool.shutdown();
    }

    // 提交一次异步操作，返回结果的 future 式句柄
    struct Future
    {
        std::atomic<bool>       done{false};
        DbOpResult              result;
    };

    std::shared_ptr<Future> submit(DbOpKind kind, const std::string& sql, ParamList params = {},
                                   std::shared_ptr<MySQLConnection> tx_conn = nullptr)
    {
        auto f = std::make_shared<Future>();
        loop.post([this, kind, sql, params = std::move(params), tx_conn = std::move(tx_conn), f]() mutable
        {
            agent.submit(kind, sql, std::move(params), std::move(tx_conn),
                         [f](DbOpResult r) { f->result = std::move(r); f->done.store(true); });
        });
        return f;
    }

    bool waitFor(std::shared_ptr<Future> f, int timeout_ms = 15000)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!f->done.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return f->done.load();
    }
};

// 可跳过集成测试：连不上数据库时 GTEST_SKIP
inline bool dbAvailable()
{
    MySQLConfig cfg = testConfig();
    MySQLConnection conn(cfg);
    return conn.connect();
}
