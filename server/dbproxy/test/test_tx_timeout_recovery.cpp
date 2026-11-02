/**
 * test_tx_timeout_recovery.cpp
 * 4) 事务跨挂起期间连接独占
 * 5) 查询超时 / 取消 / 断线恢复
 */

#include <gtest/gtest.h>

#include "test_common.h"

#include "TransactionManager.h"

namespace
{

class TxAndFailureTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!dbAvailable()) GTEST_SKIP() << "MySQL 不可用，跳过集成测试";
    }
};

} // namespace

// ── 4) 事务期间连接独占：其他操作跑到别的连接上，事务连接保持不变且仍可用 ──
// 事务独占的强断言：BEGIN 之后，其它并发操作绝不能拿到事务那条连接（用 CONNECTION_ID() 验证）
TEST_F(TxAndFailureTest, TxConnectionNeverLeakedToOtherOps)
{
    TestEnv env(2, 2);                 // 只有 2 条连接：事务占 1 条，其它操作只能抢剩下 1 条
    TransactionManager tx_mgr(env.agent, env.pool, 30000);

    uint64_t tx_id = 0;
    std::atomic<bool> begun{false};
    std::string begin_err;
    tx_mgr.beginAsync(30000, [&](uint64_t id, const std::string& err)
    {
        tx_id     = id;
        begin_err = err;
        begun.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!begun.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));

    ASSERT_TRUE(begun.load()) << "BEGIN 未完成";
    ASSERT_NE(tx_id, 0u) << begin_err;

    auto tx_conn = tx_mgr.getConnection(tx_id);
    ASSERT_NE(tx_conn, nullptr);
    const unsigned long tx_tid = tx_conn->threadId();

    // 事务挂起期间并发跑 12 个操作，每个都回报自己用的连接 id
    std::vector<std::shared_ptr<TestEnv::Future>> fs;
    for (int i = 0; i < 12; ++i)
        fs.push_back(env.submit(DbOpKind::Query, "SELECT CONNECTION_ID(), SLEEP(0.05)", {}));

    int checked = 0;
    for (auto& f : fs)
    {
        ASSERT_TRUE(env.waitFor(f, 30000));
        ASSERT_TRUE(f->result.ok) << f->result.err;
        ASSERT_FALSE(f->result.rows.empty());
        const unsigned long used = std::stoul(f->result.rows[0][0]);
        EXPECT_NE(used, tx_tid) << "事务连接被其它操作拿到（tx_tid=" << tx_tid << "）";
        ++checked;
    }
    EXPECT_EQ(checked, 12);

    // 事务连接仍是同一条，且事务依旧可用
    auto tx_conn2 = tx_mgr.getConnection(tx_id);
    ASSERT_NE(tx_conn2, nullptr);
    EXPECT_EQ(tx_conn2->threadId(), tx_tid);

    std::atomic<bool> done{false};
    tx_mgr.rollbackAsync(tx_id, [&](bool ok, const std::string&) { done.store(true); EXPECT_TRUE(ok); });
    while (!done.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(done.load());
    EXPECT_EQ(MySQLConnection::busyViolations(), 0);
}

TEST_F(TxAndFailureTest, TransactionKeepsConnectionExclusive)
{
    TestEnv env(4, 4);
    TransactionManager tx_mgr(env.agent, env.pool, 30000);

    // 必须等 DROP 完成再 CREATE：两个操作会并发各拿一条连接，否则可能撞上"表已存在"
    auto drop_first = env.submit(DbOpKind::Exec, "DROP TABLE IF EXISTS t_tx_probe");
    ASSERT_TRUE(env.waitFor(drop_first));

    auto create = env.submit(DbOpKind::Exec, "CREATE TABLE t_tx_probe (id INT PRIMARY KEY, v INT)");
    ASSERT_TRUE(env.waitFor(create));
    ASSERT_TRUE(create->result.ok) << create->result.err;

    uint64_t tx_id = 0;
    std::atomic<bool> begun{false};
    std::string begin_err;
    tx_mgr.beginAsync(30000, [&](uint64_t id, const std::string& e) { tx_id = id; begin_err = e; begun.store(true); });
    const auto begin_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!begun.load() && std::chrono::steady_clock::now() < begin_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_NE(tx_id, 0u) << begin_err;

    auto tx_conn = tx_mgr.getConnection(tx_id);
    ASSERT_NE(tx_conn, nullptr);
    const unsigned long tx_thread_id = tx_conn->threadId();

    // 事务挂起期间，跑一批普通操作（应占用其它连接）
    std::vector<std::shared_ptr<TestEnv::Future>> fs;
    for (int i = 0; i < 10; ++i)
        fs.push_back(env.submit(DbOpKind::Query, "SELECT SLEEP(0.05), ? AS n",
                                ParamList{static_cast<int64_t>(i)}));
    for (auto& f : fs) { ASSERT_TRUE(env.waitFor(f)); EXPECT_TRUE(f->result.ok) << f->result.err; }

    // 事务连接仍是同一条，且没被别人拿走
    auto tx_conn2 = tx_mgr.getConnection(tx_id);
    ASSERT_NE(tx_conn2, nullptr);
    EXPECT_EQ(tx_conn2->threadId(), tx_thread_id) << "事务连接被换掉了";

    // 在事务内写一行（用事务绑定的连接）
    auto tx_write = env.submit(DbOpKind::Exec, "INSERT INTO t_tx_probe (id, v) VALUES (?, ?)",
                               ParamList{static_cast<int64_t>(1), static_cast<int64_t>(42)}, tx_conn2);
    ASSERT_TRUE(env.waitFor(tx_write));
    ASSERT_TRUE(tx_write->result.ok) << tx_write->result.err;

    // 事务内能看到未提交的写入
    auto tx_read = env.submit(DbOpKind::Query, "SELECT v FROM t_tx_probe WHERE id = ?",
                              ParamList{static_cast<int64_t>(1)}, tx_conn2);
    ASSERT_TRUE(env.waitFor(tx_read));
    ASSERT_TRUE(tx_read->result.ok);
    ASSERT_EQ(tx_read->result.rows.size(), 1u);
    EXPECT_EQ(tx_read->result.rows[0][0], "42");

    // 回滚（异步）
    std::atomic<bool> rolled{false};
    tx_mgr.rollbackAsync(tx_id, [&](bool ok, const std::string&) { EXPECT_TRUE(ok); rolled.store(true); });
    const auto roll_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!rolled.load() && std::chrono::steady_clock::now() < roll_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(rolled.load());
    EXPECT_EQ(tx_mgr.getConnection(tx_id), nullptr) << "回滚后事务应释放";

    // 回滚后数据不可见，且连接已归还池中可继续使用
    auto after = env.submit(DbOpKind::Query, "SELECT COUNT(*) AS c FROM t_tx_probe");
    ASSERT_TRUE(env.waitFor(after));
    ASSERT_TRUE(after->result.ok);
    EXPECT_EQ(after->result.rows[0][0], "0");

    auto drop = env.submit(DbOpKind::Exec, "DROP TABLE t_tx_probe");
    ASSERT_TRUE(env.waitFor(drop));
    EXPECT_EQ(MySQLConnection::busyViolations(), 0);
}

// ── 5a) 查询超时：把 socket 等待上限调到 1s，跑 SLEEP(3) 应失败并释放连接 ──
TEST_F(TxAndFailureTest, QueryTimeoutReleasesConnection)
{
    // read_timeout=1s：SLEEP(3) 应被连接层中断
    TestEnv env(2, 2, /*wait_ms=*/1000, /*read_timeout_ms=*/1000);

    const auto t0 = std::chrono::steady_clock::now();
    auto slow = env.submit(DbOpKind::Query, "SELECT SLEEP(3)");
    ASSERT_TRUE(env.waitFor(slow, 25000));
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();

    EXPECT_FALSE(slow->result.ok) << "超时应判失败";
    // 协作式超时：库在等待掩码里带 MYSQL_WAIT_TIMEOUT，超时后由库自己报错。
    // 旧版 Connector/C（3.4.5 及更早）不带该位，会退化成"跑完才算"，用耗时兜住这个回归。
    EXPECT_LT(elapsed_ms, 2200) << "超时未被库及时判定，耗时 " << elapsed_ms << "ms";

    // 超时后连接被作废回收，池仍可用
    auto ping = env.submit(DbOpKind::Query, "SELECT 1");
    ASSERT_TRUE(env.waitFor(ping, 20000));
    EXPECT_TRUE(ping->result.ok) << ping->result.err;

    // 池已可用（作废的连接被回收，后续按需新建）
    auto ping2 = env.submit(DbOpKind::Query, "SELECT 2");
    ASSERT_TRUE(env.waitFor(ping2, 20000));
    EXPECT_TRUE(ping2->result.ok) << ping2->result.err;
    if (ping2->result.ok) EXPECT_EQ(ping2->result.rows[0][0], "2");
}

// ── 5b) 取消：目前没有 cancel API，验证"放弃等待"不会破坏后续运行 ──
// 说明：logic 侧协程 yield 超时后只是不再关心结果；DB 侧没有中断能力，
//       操作仍会跑完。这里验证：慢操作在跑的同时提交其它操作，全部正确完成，
//       且连接最终全部归还（不泄漏、不串数据）。
TEST_F(TxAndFailureTest, AbandonedSlowOpDoesNotCorruptOthers)
{
    TestEnv env(3, 3);

    auto slow = env.submit(DbOpKind::Query, "SELECT SLEEP(1.0), 999");   // 不等待它，模拟被放弃

    std::vector<std::shared_ptr<TestEnv::Future>> fs;
    for (int i = 0; i < 6; ++i)
        fs.push_back(env.submit(DbOpKind::Query, "SELECT ? AS n", ParamList{static_cast<int64_t>(i)}));

    for (auto& f : fs) { ASSERT_TRUE(env.waitFor(f)); ASSERT_TRUE(f->result.ok) << f->result.err; }
    for (int i = 0; i < 6; ++i) EXPECT_EQ(fs[static_cast<size_t>(i)]->result.rows[0][0], std::to_string(i));

    // 被放弃的慢操作最终也会完成，并且连接被归还
    ASSERT_TRUE(env.waitFor(slow, 20000));
    EXPECT_TRUE(slow->result.ok) << slow->result.err;
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    EXPECT_EQ(env.pool.activeCount(), 0);
    EXPECT_EQ(MySQLConnection::busyViolations(), 0);
}

// ── 5c) 断线恢复：从外部 KILL 掉正在使用的连接，操作应干净失败，池后续仍可用 ──
TEST_F(TxAndFailureTest, KillConnectionThenRecover)
{
    TestEnv env(2, 4);

    auto ok = env.submit(DbOpKind::Query, "SELECT 1");
    ASSERT_TRUE(env.waitFor(ok));
    ASSERT_TRUE(ok->result.ok);

    // 借一条连接出来，从另一条连接上 KILL 它
    auto victim = env.pool.acquire(2000);
    ASSERT_NE(victim, nullptr);
    const unsigned long tid = victim->threadId();

    // 用一条独立连接发 KILL，避免在池连接上混用阻塞 API 与异步 API
    MySQLConnection killer(testConfig());
    ASSERT_TRUE(killer.connect());
    std::string kerr;
    ASSERT_TRUE(killer.execRawSql("KILL " + std::to_string(tid), kerr)) << kerr;

    // 被杀掉的连接上跑操作：应失败（而不是崩溃 / 挂死 / 悄悄重连）
    auto dead = env.submit(DbOpKind::Query, "SELECT 1", {}, victim);
    ASSERT_TRUE(env.waitFor(dead, 20000));
    EXPECT_FALSE(dead->result.ok) << "断线后操作应失败";
    victim.reset();

    // 池自愈：断线连接被作废回收，后续操作重新拿到可用连接
    int ok_count = 0;
    for (int i = 0; i < 5; ++i)
    {
        auto f = env.submit(DbOpKind::Query, "SELECT 42 AS v");
        ASSERT_TRUE(env.waitFor(f, 20000));
        if (f->result.ok)
        {
            ++ok_count;
            EXPECT_EQ(f->result.rows[0][0], "42");
        }
    }
    EXPECT_GE(ok_count, 4) << "断线后连接池应恢复，实际成功 " << ok_count << "/5";
    EXPECT_EQ(MySQLConnection::busyViolations(), 0);
}
