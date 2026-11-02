/**
 * test_auth_tls.cpp
 * 7) MySQL 8.4 认证与 TLS
 *
 * 说明：本机 MySQL 以 mysql_native_password 方式授权（MariaDB Connector/C 不支持
 *       caching_sha2_password），因此这里验证：
 *        - 正确账号密码可连接，密码错误连接失败
 *        - 服务端确实是 MySQL 8.4（version 断言）
 *        - use_ssl=true 时能建立 TLS 连接（Ssl_cipher 非空）
 */

#include <gtest/gtest.h>

#include "test_common.h"

namespace
{

class AuthTlsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!dbAvailable()) GTEST_SKIP() << "MySQL 不可用，跳过集成测试";
    }
};

// 用给定配置跑一条查询，返回 [是否成功, 首行首列]
std::pair<bool, std::string> runQuery(MySQLConfig cfg, const std::string& sql)
{
    TestEnv env_from_cfg(1, 1);
    (void)env_from_cfg;   // 占位：下面用独立连接直接跑，避免依赖 env 的库名
    MySQLConnection conn(cfg);
    if (!conn.connect()) return {false, ""};

    MySQLConnection::QueryResult r;
    std::string err;
    if (!conn.query(sql, {}, r, err)) return {false, ""};
    if (r.rows.empty() || r.rows[0].empty()) return {true, ""};
    return {true, r.rows[0][0]};
}

} // namespace

TEST_F(AuthTlsTest, ServerIsMysql84)
{
    MySQLConfig cfg = testConfig();
    MySQLConnection conn(cfg);
    ASSERT_TRUE(conn.connect());

    auto [ok, version] = runQuery(cfg, "SELECT @@version");
    ASSERT_TRUE(ok);
    EXPECT_TRUE(version.rfind("8.4", 0) == 0) << "期望 MySQL 8.4，实际：" << version;
}

TEST_F(AuthTlsTest, CorrectPasswordConnects)
{
    MySQLConfig cfg = testConfig();
    MySQLConnection conn(cfg);
    EXPECT_TRUE(conn.connect());
    EXPECT_TRUE(conn.isConnected());
}

TEST_F(AuthTlsTest, WrongPasswordFails)
{
    MySQLConfig cfg = testConfig();
    cfg.password = "definitely_wrong_password";
    MySQLConnection conn(cfg);
    EXPECT_FALSE(conn.connect());
}

TEST_F(AuthTlsTest, TlsConnectionEstablished)
{
    MySQLConfig cfg = testConfig();
    cfg.use_ssl = true;

    MySQLConnection conn(cfg);
    if (!conn.connect())
    {
        GTEST_SKIP() << "服务端未启用 TLS 或客户端库不支持，跳过";
    }

    // 确认当前连接确实走了 TLS
    auto [ok, cipher] = runQuery(cfg, "SHOW STATUS LIKE 'Ssl_cipher'");
    ASSERT_TRUE(ok);

    MySQLConnection conn2(cfg);
    ASSERT_TRUE(conn2.connect());
    MySQLConnection::QueryResult r;
    std::string err;
    ASSERT_TRUE(conn2.query("SHOW SESSION STATUS LIKE 'Ssl_cipher'", {}, r, err)) << err;
    ASSERT_FALSE(r.rows.empty());
    ASSERT_GE(r.rows[0].size(), 2u);
    EXPECT_FALSE(r.rows[0][1].empty()) << "TLS 连接应能取到 Ssl_cipher";
}
