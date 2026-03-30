/**
 * test_load_balancer.cpp
 * LoadBalancer 6 种策略单元测试
 *
 * 覆盖：
 *  RoundRobin      - 轮询、key 直查、空集
 *  WeightedRR      - 权重比例统计
 *  LeastConn       - 最小连接数选择
 *  ConsistentHash  - 相同 key 幂等、空 key 退化
 *  SmoothWeightedRR- Nginx 平滑加权轮询分布验证
 *  LatencyAware    - 最低延迟优先，0 延迟视为高延迟
 */

#include <gtest/gtest.h>

#include <cmath>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "LoadBalancer.h"
#include "ServiceInstance.h"

// ──────────────────────────────────────────────
// 辅助函数：快速创建 ServiceInstance
// ──────────────────────────────────────────────
static ServiceInstancePtr MakeInst(const std::string& id,
                                    const std::string& svc_name = "svc",
                                    uint32_t weight = 1,
                                    uint64_t connections = 0,
                                    uint64_t latency_us = 0)
{
    auto inst        = std::make_shared<ServiceInstance>();
    inst->id         = id;
    inst->svc_name   = svc_name;
    inst->address    = "127.0.0.1";
    inst->port       = 8080;
    inst->weight     = weight;
    inst->connections = connections;
    inst->avg_latency_us = latency_us;
    inst->healthy    = true;
    return inst;
}

static ServiceInstances MakeMap(std::initializer_list<ServiceInstancePtr> insts)
{
    ServiceInstances m;
    for (auto& i : insts) m[i->id] = i;
    return m;
}

// ──────────────────────────────────────────────
// 1. RoundRobin
// ──────────────────────────────────────────────
class RoundRobinTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        lb_ = LBFactory().create("round_robin");
    }
    std::shared_ptr<ILoadBalancer> lb_;
};

TEST_F(RoundRobinTest, EmptyInstances)
{
    ServiceInstances empty;
    EXPECT_EQ(lb_->select(empty), nullptr);
}

TEST_F(RoundRobinTest, SingleInstance)
{
    auto inst = MakeInst("a");
    auto m    = MakeMap({inst});
    auto sel  = lb_->select(m);
    EXPECT_NE(sel, nullptr);
    EXPECT_EQ(sel->id, "a");
}

TEST_F(RoundRobinTest, ThreeInstancesRotate)
{
    auto m = MakeMap({MakeInst("a"), MakeInst("b"), MakeInst("c")});

    std::map<std::string, int> count;
    for (int i = 0; i < 30; ++i)
    {
        auto sel = lb_->select(m);
        ASSERT_NE(sel, nullptr);
        count[sel->id]++;
    }

    // 轮询 30 次，3 个实例各自被选约 10 次（允许误差）
    for (auto& kv : count)
        EXPECT_NEAR(kv.second, 10, 3) << "id=" << kv.first;
}

TEST_F(RoundRobinTest, KeyDirectLookupHit)
{
    auto m   = MakeMap({MakeInst("a"), MakeInst("b")});
    auto sel = lb_->select(m, "b");
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->id, "b");
}

TEST_F(RoundRobinTest, KeyDirectLookupMiss)
{
    auto m   = MakeMap({MakeInst("a")});
    auto sel = lb_->select(m, "not_exist");
    EXPECT_EQ(sel, nullptr);
}

// ──────────────────────────────────────────────
// 2. WeightedRR
// ──────────────────────────────────────────────
class WeightedRRTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        lb_ = LBFactory().create("weighted_rr");
    }
    std::shared_ptr<ILoadBalancer> lb_;
};

TEST_F(WeightedRRTest, EmptyInstances)
{
    ServiceInstances empty;
    EXPECT_EQ(lb_->select(empty), nullptr);
}

TEST_F(WeightedRRTest, WeightRatioApproximately)
{
    // weight 1:3 → 选中比约 1:3
    auto m = MakeMap({MakeInst("light", "svc", 1), MakeInst("heavy", "svc", 3)});

    std::map<std::string, int> count;
    for (int i = 0; i < 1000; ++i)
    {
        auto sel = lb_->select(m);
        ASSERT_NE(sel, nullptr);
        count[sel->id]++;
    }

    double ratio = static_cast<double>(count["heavy"]) / count["light"];
    // 期望约 3，允许 ±50% 误差（统计随机性）
    EXPECT_NEAR(ratio, 3.0, 1.5) << "heavy=" << count["heavy"] << " light=" << count["light"];
}

// ──────────────────────────────────────────────
// 3. LeastConn
// ──────────────────────────────────────────────
class LeastConnTest : public ::testing::Test
{
protected:
    void SetUp() override { lb_ = LBFactory().create("least_conn"); }
    std::shared_ptr<ILoadBalancer> lb_;
};

TEST_F(LeastConnTest, EmptyInstances)
{
    EXPECT_EQ(lb_->select(ServiceInstances{}), nullptr);
}

TEST_F(LeastConnTest, SelectMinConnections)
{
    auto m = MakeMap({
        MakeInst("high", "svc", 1, 10),
        MakeInst("low",  "svc", 1, 3),
        MakeInst("mid",  "svc", 1, 7),
    });
    auto sel = lb_->select(m);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->id, "low");
}

TEST_F(LeastConnTest, ZeroConnections)
{
    // 连接数都为 0 时，选任意一个（不崩溃）
    auto m = MakeMap({MakeInst("a", "svc", 1, 0), MakeInst("b", "svc", 1, 0)});
    EXPECT_NE(lb_->select(m), nullptr);
}

// ──────────────────────────────────────────────
// 4. ConsistentHash
// ──────────────────────────────────────────────
class ConsistentHashTest : public ::testing::Test
{
protected:
    void SetUp() override { lb_ = LBFactory().create("cons_hash"); }
    std::shared_ptr<ILoadBalancer> lb_;
};

TEST_F(ConsistentHashTest, EmptyInstances)
{
    EXPECT_EQ(lb_->select(ServiceInstances{}, "key"), nullptr);
}

TEST_F(ConsistentHashTest, SameKeyReturnsSameInstance)
{
    auto m = MakeMap({MakeInst("a"), MakeInst("b"), MakeInst("c")});
    auto sel1 = lb_->select(m, "user123");
    auto sel2 = lb_->select(m, "user123");
    ASSERT_NE(sel1, nullptr);
    ASSERT_NE(sel2, nullptr);
    EXPECT_EQ(sel1->id, sel2->id);
}

TEST_F(ConsistentHashTest, DifferentKeysDistribute)
{
    auto m = MakeMap({MakeInst("a"), MakeInst("b"), MakeInst("c")});
    std::set<std::string> ids;
    for (int i = 0; i < 100; ++i)
    {
        auto sel = lb_->select(m, "key_" + std::to_string(i));
        ASSERT_NE(sel, nullptr);
        ids.insert(sel->id);
    }
    // 100 个不同 key 应分布到多个实例，不全相同
    EXPECT_GT(ids.size(), 1u);
}

TEST_F(ConsistentHashTest, EmptyKeyFallsBackToRoundRobin)
{
    // key 为空退化为轮询，不崩溃，返回非 nullptr
    auto m = MakeMap({MakeInst("a"), MakeInst("b")});
    EXPECT_NE(lb_->select(m, ""), nullptr);
}

// ──────────────────────────────────────────────
// 5. SmoothWeightedRR（Nginx 算法）
// ──────────────────────────────────────────────
class SmoothWeightedTest : public ::testing::Test
{
protected:
    void SetUp() override { lb_ = LBFactory().create("smooth_weighted"); }
    std::shared_ptr<ILoadBalancer> lb_;
};

TEST_F(SmoothWeightedTest, EmptyInstances)
{
    EXPECT_EQ(lb_->select(ServiceInstances{}), nullptr);
}

TEST_F(SmoothWeightedTest, SingleInstance)
{
    auto m   = MakeMap({MakeInst("only", "svc", 5)});
    auto sel = lb_->select(m);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->id, "only");
}

TEST_F(SmoothWeightedTest, WeightDistributionLargeScale)
{
    // 每次测试用新实例，避免上一个测试的状态影响本测试
    auto lb = LBFactory().create("smooth_weighted");
    // 权重 5:1，select 6000 次，heavy 应约出现 5000 次
    auto m = MakeMap({MakeInst("heavy", "svc", 5), MakeInst("light", "svc", 1)});

    std::map<std::string, int> count;
    for (int i = 0; i < 6000; ++i)
    {
        auto sel = lb->select(m);
        ASSERT_NE(sel, nullptr);
        count[sel->id]++;
    }

    double ratio = static_cast<double>(count["heavy"]) / count["light"];
    // 期望比约 5，允许 ±1.5 误差
    EXPECT_NEAR(ratio, 5.0, 1.5)
        << "heavy=" << count["heavy"] << " light=" << count["light"];
}

TEST_F(SmoothWeightedTest, SixCycleDistribution)
{
    // 每次测试用新实例以确保初始状态
    auto lb = LBFactory().create("smooth_weighted");
    // 权重 5:1，6 次为一轮，heavy 应出现 5 次
    auto m = MakeMap({MakeInst("heavy", "svc", 5), MakeInst("light", "svc", 1)});

    std::map<std::string, int> count;
    for (int i = 0; i < 6; ++i)
    {
        auto sel = lb->select(m);
        ASSERT_NE(sel, nullptr);
        count[sel->id]++;
    }
    EXPECT_EQ(count["heavy"], 5);
    EXPECT_EQ(count["light"], 1);
}

TEST_F(SmoothWeightedTest, AddNewInstanceMidway)
{
    auto m = MakeMap({MakeInst("a", "svc", 2), MakeInst("b", "svc", 1)});

    // 先 select 几次，建立内部状态
    for (int i = 0; i < 10; ++i) lb_->select(m);

    // 加入新实例
    m[std::string("c")] = MakeInst("c", "svc", 1);
    // 不应崩溃，应能正常 select
    for (int i = 0; i < 10; ++i)
        EXPECT_NE(lb_->select(m), nullptr);
}

// ──────────────────────────────────────────────
// 6. LatencyAware
// ──────────────────────────────────────────────
class LatencyAwareTest : public ::testing::Test
{
protected:
    void SetUp() override { lb_ = LBFactory().create("latency_aware"); }
    std::shared_ptr<ILoadBalancer> lb_;
};

TEST_F(LatencyAwareTest, EmptyInstances)
{
    EXPECT_EQ(lb_->select(ServiceInstances{}), nullptr);
}

TEST_F(LatencyAwareTest, SelectLowestLatency)
{
    auto m = MakeMap({
        MakeInst("slow",   "svc", 1, 0, 200),
        MakeInst("fast",   "svc", 1, 0, 50),
        MakeInst("medium", "svc", 1, 0, 100),
    });
    auto sel = lb_->select(m);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->id, "fast");
}

TEST_F(LatencyAwareTest, ZeroLatencyTreatedAsHigh)
{
    // avg_latency_us == 0 视为未知（高延迟），应优先选有真实延迟的
    auto m = MakeMap({
        MakeInst("unknown", "svc", 1, 0, 0),   // 0 = 未知
        MakeInst("known",   "svc", 1, 0, 100),
    });
    auto sel = lb_->select(m);
    ASSERT_NE(sel, nullptr);
    EXPECT_EQ(sel->id, "known");
}

TEST_F(LatencyAwareTest, AllUnknownLatency)
{
    // 全部延迟为 0，应选任意一个（不崩溃）
    auto m = MakeMap({MakeInst("a", "svc", 1, 0, 0), MakeInst("b", "svc", 1, 0, 0)});
    EXPECT_NE(lb_->select(m), nullptr);
}

// ──────────────────────────────────────────────
// 7. LBFactory - unknown name fallback
// ──────────────────────────────────────────────
TEST(LBFactory, UnknownNameFallsBackToRoundRobin)
{
    LBFactory factory;
    auto lb = factory.create("no_such_strategy");
    ASSERT_NE(lb, nullptr);

    auto m   = MakeMap({MakeInst("x")});
    auto sel = lb->select(m);
    EXPECT_NE(sel, nullptr);
}

TEST(LBFactory, AllStrategiesCreate)
{
    LBFactory factory;
    for (auto& name : {"round_robin", "weighted_rr", "least_conn",
                       "cons_hash", "smooth_weighted", "latency_aware"})
    {
        auto lb = factory.create(name);
        EXPECT_NE(lb, nullptr) << "Failed to create: " << name;
    }
}

// ──────────────────────────────────────────────
// 8. 扩展测试
// ──────────────────────────────────────────────

// RoundRobin：20 个实例，select 200 次，所有实例均被选到
TEST_F(RoundRobinTest, LargePoolRotates)
{
    ServiceInstances m;
    for (int i = 0; i < 20; ++i)
        m["inst_" + std::to_string(i)] = MakeInst("inst_" + std::to_string(i));

    std::set<std::string> seen;
    for (int i = 0; i < 200; ++i)
    {
        auto sel = lb_->select(m);
        ASSERT_NE(sel, nullptr);
        seen.insert(sel->id);
    }
    EXPECT_EQ(seen.size(), 20u);  // 所有实例均被选到
}

// WeightedRR：只有 1 个实例，weight=100，1000 次全选它
TEST_F(WeightedRRTest, SingleInstanceAlwaysSelected)
{
    auto m = MakeMap({MakeInst("only", "svc", 100)});
    for (int i = 0; i < 1000; ++i)
    {
        auto sel = lb_->select(m);
        ASSERT_NE(sel, nullptr);
        EXPECT_EQ(sel->id, "only");
    }
}

// WeightedRR：所有权重均为 1 时分布均匀（退化为 RR）
TEST_F(WeightedRRTest, WeightOneEqualsRoundRobin)
{
    auto m = MakeMap({MakeInst("a", "svc", 1), MakeInst("b", "svc", 1), MakeInst("c", "svc", 1)});
    std::map<std::string, int> count;
    for (int i = 0; i < 300; ++i)
    {
        auto sel = lb_->select(m);
        ASSERT_NE(sel, nullptr);
        count[sel->id]++;
    }
    // 权重均为1，分布应均匀，各约 100 次，允许 ±20 误差
    for (auto& kv : count)
        EXPECT_NEAR(kv.second, 100, 20) << "id=" << kv.first;
}

// LeastConn：所有实例连接数相同时，不崩溃，返回非 nullptr（tie-breaking）
TEST_F(LeastConnTest, TieBreaking)
{
    auto m = MakeMap({
        MakeInst("a", "svc", 1, 5),
        MakeInst("b", "svc", 1, 5),
        MakeInst("c", "svc", 1, 5),
    });
    // 三者相同连接数，任选一个，不崩溃
    EXPECT_NE(lb_->select(m), nullptr);
}

// ConsistentHash：同 key 重复 100 次，结果相同
TEST_F(ConsistentHashTest, ConsistencyAfterRepeatCalls)
{
    auto m = MakeMap({MakeInst("a"), MakeInst("b"), MakeInst("c")});
    std::string first_id;
    for (int i = 0; i < 100; ++i)
    {
        auto sel = lb_->select(m, "sticky_user");
        ASSERT_NE(sel, nullptr);
        if (first_id.empty()) first_id = sel->id;
        EXPECT_EQ(sel->id, first_id);
    }
}

// SmoothWeighted：运行时从 map 删除一个实例，不崩溃且不再被选到
TEST_F(SmoothWeightedTest, RemoveInstanceMidway)
{
    auto lb = LBFactory().create("smooth_weighted");
    auto m  = MakeMap({MakeInst("keep", "svc", 3), MakeInst("remove_me", "svc", 1)});

    // 先 select 若干次建立状态
    for (int i = 0; i < 20; ++i) lb->select(m);

    // 删除 remove_me
    m.erase("remove_me");

    // 继续 select，不应崩溃，且只返回 "keep"
    for (int i = 0; i < 20; ++i)
    {
        auto sel = lb->select(m);
        ASSERT_NE(sel, nullptr);
        EXPECT_EQ(sel->id, "keep");
    }
}

// LatencyAware：只有 1 个实例，无论延迟如何都返回它
TEST_F(LatencyAwareTest, SingleInstance)
{
    auto m = MakeMap({MakeInst("solo", "svc", 1, 0, 999)});
    for (int i = 0; i < 10; ++i)
    {
        auto sel = lb_->select(m);
        ASSERT_NE(sel, nullptr);
        EXPECT_EQ(sel->id, "solo");
    }
}

// 并发：4 线程同时对 RoundRobin select 10000 次，无崩溃（TSan验证）
TEST(ConcurrentLB, ConcurrentSelect)
{
    auto lb = LBFactory().create("round_robin");
    auto m  = MakeMap({MakeInst("a"), MakeInst("b"), MakeInst("c")});

    constexpr int kThreads = 4;
    constexpr int kIters   = 10000;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i)
            {
                auto sel = lb->select(m);
                EXPECT_NE(sel, nullptr);
            }
        });
    }
    for (auto& th : threads) th.join();
}
