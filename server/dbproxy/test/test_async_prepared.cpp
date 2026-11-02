/**
 * test_async_prepared.cpp
 * 1) Prepared Statement 全流程异步执行
 * 2) 多协程同时使用不同连接
 * 3) 同一连接禁止并发执行多个 SQL
 * 4) 大结果集 / BLOB / NULL / 中文
 */

#include <gtest/gtest.h>

#include "test_common.h"

#include <set>

namespace
{

class AsyncDbTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!dbAvailable()) GTEST_SKIP() << "MySQL 不可用，跳过集成测试";
    }
};

} // namespace

// ── 1) Prepared Statement 全流程异步：prepare → bind → execute → store → fetch ──
TEST_F(AsyncDbTest, PreparedStatementFullAsyncFlow)
{
    TestEnv env(2, 4);

    auto ddl = env.submit(DbOpKind::Exec, "DROP TABLE IF EXISTS t_async_ps");
    ASSERT_TRUE(env.waitFor(ddl));

    auto create = env.submit(DbOpKind::Exec,
        "CREATE TABLE t_async_ps (id INT PRIMARY KEY AUTO_INCREMENT, name VARCHAR(64), score DOUBLE)");
    ASSERT_TRUE(env.waitFor(create));
    ASSERT_TRUE(create->result.ok) << create->result.err;

    // 参数化写入：字符串 / 整型 / 浮点 / NULL
    auto ins = env.submit(DbOpKind::Exec, "INSERT INTO t_async_ps (name, score) VALUES (?, ?)",
                          ParamList{std::string("中文 alice"), 12.5});
    ASSERT_TRUE(env.waitFor(ins));
    ASSERT_TRUE(ins->result.ok) << ins->result.err;
    EXPECT_GT(ins->result.last_insert_id, 0u);

    auto ins_null = env.submit(DbOpKind::Exec, "INSERT INTO t_async_ps (name, score) VALUES (?, ?)",
                               ParamList{std::string("bob"), std::monostate{}});
    ASSERT_TRUE(env.waitFor(ins_null));
    ASSERT_TRUE(ins_null->result.ok);

    // 参数化读取（全流程走预处理语句）
    auto q = env.submit(DbOpKind::Query, "SELECT id, name, score FROM t_async_ps WHERE name = ?",
                        ParamList{std::string("中文 alice")});
    ASSERT_TRUE(env.waitFor(q));
    ASSERT_TRUE(q->result.ok) << q->result.err;
    ASSERT_EQ(q->result.rows.size(), 1u);
    EXPECT_EQ(q->result.rows[0][1], "中文 alice");
    EXPECT_EQ(q->result.rows[0][2], "12.5");

    // NULL 读取
    auto qn = env.submit(DbOpKind::Query, "SELECT name, score FROM t_async_ps WHERE name = ?",
                         ParamList{std::string("bob")});
    ASSERT_TRUE(env.waitFor(qn));
    ASSERT_TRUE(qn->result.ok);
    ASSERT_EQ(qn->result.rows.size(), 1u);
    ASSERT_EQ(qn->result.null_flags.size(), 1u);
    EXPECT_TRUE(qn->result.null_flags[0][1]) << "score 应为 NULL";

    auto drop = env.submit(DbOpKind::Exec, "DROP TABLE t_async_ps");
    ASSERT_TRUE(env.waitFor(drop));
}

// ── 2) 多任务并发，各自使用不同连接 ──
TEST_F(AsyncDbTest, ConcurrentOpsUseDistinctConnections)
{
    TestEnv env(4, 4);   // 4 个连接
    const int n = 12;    // 12 个并发慢查询，必然出现排队

    std::vector<std::shared_ptr<TestEnv::Future>> fs;
    for (int i = 0; i < n; ++i)
        fs.push_back(env.submit(DbOpKind::Query, "SELECT SLEEP(0.2), ? AS n",
                                ParamList{static_cast<int64_t>(i)}));

    for (auto& f : fs) ASSERT_TRUE(env.waitFor(f, 30000));
    for (auto& f : fs) EXPECT_TRUE(f->result.ok) << f->result.err;

    // 并发期间在飞操作数不应超过连接数（说明一个连接只承载一个操作）
    EXPECT_LE(env.pool.activeCount(), 4);
    EXPECT_EQ(MySQLConnection::busyViolations(), 0) << "同一连接被并发占用";
}

// ── 3) 同一连接禁止并发执行：连接池独占性 + 不变量计数 ──
TEST_F(AsyncDbTest, ConnectionExclusiveOwnership)
{
    TestEnv env(1, 1);   // 只有 1 个连接：所有操作必须串行

    std::vector<std::shared_ptr<TestEnv::Future>> fs;
    for (int i = 0; i < 8; ++i)
        fs.push_back(env.submit(DbOpKind::Query, "SELECT ? AS n", ParamList{static_cast<int64_t>(i)}));

    for (auto& f : fs) ASSERT_TRUE(env.waitFor(f));
    for (auto& f : fs) EXPECT_TRUE(f->result.ok) << f->result.err;

    EXPECT_EQ(MySQLConnection::busyViolations(), 0);

    // 直接验证池的独占语义：借出后再次 tryAcquire 拿不到
    auto c1 = env.pool.acquire(2000);
    ASSERT_NE(c1, nullptr);
    auto c2 = env.pool.tryAcquire();
    EXPECT_EQ(c2, nullptr) << "连接已借出，不应再被分配";
    c1.reset();
    auto c3 = env.pool.tryAcquire();
    EXPECT_NE(c3, nullptr) << "归还后应可再次分配";
}

// ── 6) 大结果集 / BLOB / NULL / 中文 ──
TEST_F(AsyncDbTest, LargeResultSetBlobNullUtf8)
{
    TestEnv env(2, 4);

    auto drop = env.submit(DbOpKind::Exec, "DROP TABLE IF EXISTS t_async_big");
    ASSERT_TRUE(env.waitFor(drop));
    auto create = env.submit(DbOpKind::Exec,
        "CREATE TABLE t_async_big (id INT PRIMARY KEY, note TEXT, blob_col BLOB, maybe_int INT NULL, cn VARCHAR(64))");
    ASSERT_TRUE(env.waitFor(create));
    ASSERT_TRUE(create->result.ok) << create->result.err;

    const int rows = 2000;
    std::vector<uint8_t> blob(512);
    for (size_t i = 0; i < blob.size(); ++i) blob[i] = static_cast<uint8_t>(i % 256);

    // 逐行插入（顺带压一下写入路径）
    std::vector<std::shared_ptr<TestEnv::Future>> inserts;
    for (int i = 0; i < rows; ++i)
    {
        inserts.push_back(env.submit(DbOpKind::Exec,
            "INSERT INTO t_async_big (id, note, blob_col, maybe_int, cn) VALUES (?, ?, ?, ?, ?)",
            ParamList{static_cast<int64_t>(i),
                      std::string("note-") + std::to_string(i),
                      blob,
                      (i % 2 == 0) ? ParamValue{static_cast<int64_t>(i)} : ParamValue{std::monostate{}},
                      std::string("中文内容-") + std::to_string(i)}));
        if (inserts.size() >= 200)   // 分批等，避免堆积过多 future
        {
            for (auto& f : inserts) { ASSERT_TRUE(env.waitFor(f)); ASSERT_TRUE(f->result.ok) << f->result.err; }
            inserts.clear();
        }
    }
    for (auto& f : inserts) { ASSERT_TRUE(env.waitFor(f)); ASSERT_TRUE(f->result.ok) << f->result.err; }

    // 大结果集读取
    auto big = env.submit(DbOpKind::Query, "SELECT id, note, blob_col, maybe_int, cn FROM t_async_big ORDER BY id");
    ASSERT_TRUE(env.waitFor(big, 60000));
    ASSERT_TRUE(big->result.ok) << big->result.err;
    ASSERT_EQ(big->result.rows.size(), static_cast<size_t>(rows));

    for (int i = 0; i < rows; ++i)
    {
        const auto& r = big->result.rows[static_cast<size_t>(i)];
        EXPECT_EQ(r[0], std::to_string(i));
        EXPECT_EQ(r[1], "note-" + std::to_string(i));
        // BLOB：0x00 0x01 ... 循环
        ASSERT_EQ(r[2].size(), blob.size());
        for (size_t k = 0; k < blob.size(); ++k)
            EXPECT_EQ(static_cast<uint8_t>(r[2][k]), static_cast<uint8_t>(k % 256)) << "row " << i << " byte " << k;
        // NULL 交替
        const bool is_null = big->result.null_flags[static_cast<size_t>(i)][3];
        EXPECT_EQ(is_null, (i % 2 == 1));
        // 中文
        EXPECT_EQ(r[4], "中文内容-" + std::to_string(i));
    }

    auto drop2 = env.submit(DbOpKind::Exec, "DROP TABLE t_async_big");
    ASSERT_TRUE(env.waitFor(drop2));
}
