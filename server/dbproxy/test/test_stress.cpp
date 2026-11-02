/**
 * test_stress.cpp
 * 8) 长时间压力测试 + 内存泄漏观察
 *
 * 观察方式：读取 /proc/self/status 的 VmRSS，比较压力前后增量。
 * 默认 3000 次操作；可用 TEST_STRESS_OPS 环境变量调整。
 */

#include <gtest/gtest.h>

#include "test_common.h"

#include <fstream>

namespace
{

long rssKb()
{
    std::ifstream f("/proc/self/status");
    std::string key;
    long value = 0;
    std::string unit;
    while (f >> key)
    {
        if (key == "VmRSS:")
        {
            f >> value >> unit;
            return value;
        }
        std::getline(f, unit);
    }
    return -1;
}

class StressTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        if (!dbAvailable()) GTEST_SKIP() << "MySQL 不可用，跳过集成测试";
    }
};

} // namespace

TEST_F(StressTest, SustainedLoadNoLeak)
{
    const int ops = std::stoi(envOr("TEST_STRESS_OPS", "3000"));

    TestEnv env(4, 8);

    // 预热：把连接池、语句缓存、线程栈都拉起来
    auto warm = env.submit(DbOpKind::Query, "SELECT 1");
    ASSERT_TRUE(env.waitFor(warm));

    const long rss_before = rssKb();
    ASSERT_GT(rss_before, 0);

    int failures = 0;
    const int batch = 50;
    for (int done = 0; done < ops; ++done)
    {
        std::vector<std::shared_ptr<TestEnv::Future>> fs;
        for (int i = 0; i < batch && done + i < ops; ++i)
        {
            const int n = done + i;
            if (n % 3 == 0)
                fs.push_back(env.submit(DbOpKind::Query, "SELECT ?, '中文压力测试' AS s",
                                        ParamList{static_cast<int64_t>(n)}));
            else if (n % 3 == 1)
                fs.push_back(env.submit(DbOpKind::Exec, "DO ?", ParamList{static_cast<int64_t>(n)}));
            else
                fs.push_back(env.submit(DbOpKind::Query, "SELECT SLEEP(0.01), ? AS n",
                                        ParamList{static_cast<int64_t>(n)}));
        }
        for (auto& f : fs)
        {
            if (!env.waitFor(f, 30000)) { ++failures; continue; }
            if (!f->result.ok) ++failures;
        }
    }

    // 收尾再跑一次，确认池与 IO 循环仍然健康
    auto tail = env.submit(DbOpKind::Query, "SELECT 1");
    ASSERT_TRUE(env.waitFor(tail));
    EXPECT_TRUE(tail->result.ok) << tail->result.err;

    const long rss_after = rssKb();
    const long delta_kb = rss_after - rss_before;

    std::cout << "[stress] ops=" << ops << " rss " << rss_before << "KB -> " << rss_after
              << "KB (delta " << delta_kb << "KB), failures=" << failures << std::endl;

    EXPECT_EQ(failures, 0);
    EXPECT_EQ(MySQLConnection::busyViolations(), 0);
    EXPECT_EQ(env.pool.activeCount(), 0);
    // 允许一定抖动：主要防止线性泄漏
    EXPECT_LT(delta_kb, 120 * 1024) << "疑似内存泄漏：" << delta_kb << "KB";
}
