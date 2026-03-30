/**
 * test_service_registry.cpp
 * ServiceRegistry 单元测试
 *
 * 覆盖：
 *  Register & Get  - 注册、续约、注销、get_instances
 *  TTL & Cleanup   - TTL 到期自动清理，续约延长生命周期
 *  DeltaMap        - subscribe/unsubscribe/pop_deltas/push_delta_to_others
 *  Concurrent      - 多线程并发注册 + 查询无数据竞争
 */

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "ServiceRegistry.h"
#include "ServiceInstance.h"

using namespace std::chrono_literals;

// ──────────────────────────────────────────────
// 辅助
// ──────────────────────────────────────────────
static ServiceInstancePtr MakeInst(const std::string& id, const std::string& svc = "svc")
{
    auto inst      = std::make_shared<ServiceInstance>();
    inst->id       = id;
    inst->svc_name = svc;
    inst->address  = "127.0.0.1";
    inst->port     = 8080;
    inst->weight   = 1;
    inst->healthy  = true;
    return inst;
}

// ══════════════════════════════════════════════
// 1. Register & Get
// ══════════════════════════════════════════════
class RegistryRegisterTest : public ::testing::Test
{
protected:
    ServiceRegistry reg_;
};

TEST_F(RegistryRegisterTest, RegisterAndGetInstances)
{
    auto inst = MakeInst("inst1");
    reg_.register_instance(inst, 30s, true);

    auto result = reg_.get_instances("svc", true);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.begin()->second->id, "inst1");
}

TEST_F(RegistryRegisterTest, RenewDoesNotDuplicate)
{
    auto inst = MakeInst("inst1");
    reg_.register_instance(inst, 30s, true);
    reg_.register_instance(inst, 30s, false);  // 续约

    auto result = reg_.get_instances("svc");
    EXPECT_EQ(result.size(), 1u);
}

TEST_F(RegistryRegisterTest, MultipleInstances)
{
    reg_.register_instance(MakeInst("a"), 30s, true);
    reg_.register_instance(MakeInst("b"), 30s, true);
    reg_.register_instance(MakeInst("c"), 30s, true);

    auto result = reg_.get_instances("svc");
    EXPECT_EQ(result.size(), 3u);
}

TEST_F(RegistryRegisterTest, DeregisterRemovesInstance)
{
    reg_.register_instance(MakeInst("inst1"), 30s, true);
    reg_.register_instance(MakeInst("inst2"), 30s, true);
    reg_.deregister_instance("svc", "inst1");

    auto result = reg_.get_instances("svc");
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.begin()->second->id, "inst2");
}

TEST_F(RegistryRegisterTest, DeregisterLastMakesServiceEmpty)
{
    reg_.register_instance(MakeInst("only"), 30s, true);
    reg_.deregister_instance("svc", "only");
    EXPECT_TRUE(reg_.get_instances("svc").empty());
}

TEST_F(RegistryRegisterTest, DeregisterNonExistentNoCrash)
{
    EXPECT_NO_THROW(reg_.deregister_instance("svc", "ghost"));
}

TEST_F(RegistryRegisterTest, GetByIdFound)
{
    auto inst = MakeInst("inst1");
    reg_.register_instance(inst, 30s, true);
    auto found = reg_.get_by_id("svc", "inst1");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->id, "inst1");
}

TEST_F(RegistryRegisterTest, GetByIdNotFound)
{
    EXPECT_EQ(reg_.get_by_id("svc", "ghost"), nullptr);
}

TEST_F(RegistryRegisterTest, GetInstancesUnknownService)
{
    EXPECT_TRUE(reg_.get_instances("no_such_svc").empty());
}

TEST_F(RegistryRegisterTest, OnlyHealthyFilter)
{
    auto healthy   = MakeInst("h");
    auto unhealthy = MakeInst("u");
    unhealthy->healthy = false;

    reg_.register_instance(healthy,   30s, true);
    reg_.register_instance(unhealthy, 30s, true);

    auto all  = reg_.get_instances("svc", false);
    auto well = reg_.get_instances("svc", true);
    EXPECT_EQ(all.size(),  2u);
    EXPECT_EQ(well.size(), 1u);
    EXPECT_EQ(well.begin()->second->id, "h");
}

// ══════════════════════════════════════════════
// 2. TTL & Cleanup
// ══════════════════════════════════════════════
class RegistryTTLTest : public ::testing::Test
{
protected:
    ServiceRegistry reg_;
};

TEST_F(RegistryTTLTest, ExpiredInstanceRemovedAfterCleanup)
{
    reg_.register_instance(MakeInst("exp"), 1s, true);

    // 还没过期，应存在
    EXPECT_EQ(reg_.get_instances("svc").size(), 1u);

    std::this_thread::sleep_for(1100ms);
    reg_.cleanup_expired();

    EXPECT_TRUE(reg_.get_instances("svc").empty());
}

TEST_F(RegistryTTLTest, RenewedInstanceSurvivesCleanup)
{
    reg_.register_instance(MakeInst("alive"), 2s, true);
    std::this_thread::sleep_for(500ms);

    // 续约（延长 TTL）
    reg_.register_instance(MakeInst("alive"), 2s, false);
    std::this_thread::sleep_for(1600ms);

    reg_.cleanup_expired();
    // 续约后 TTL 从续约时刻重新计算，还剩约 400ms，应仍存活
    EXPECT_EQ(reg_.get_instances("svc").size(), 1u);
}

// ══════════════════════════════════════════════
// 3. DeltaMap
// ══════════════════════════════════════════════
class RegistryDeltaTest : public ::testing::Test
{
protected:
    ServiceRegistry reg_;
};

TEST_F(RegistryDeltaTest, SubscribeAndReceiveUpsert)
{
    reg_.subscribe("sub1");
    reg_.register_instance(MakeInst("inst1"), 30s, true);

    auto deltas = reg_.pop_deltas("sub1");
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].op, DeltaEntry::Op::UPSERT);
    EXPECT_EQ(deltas[0].inst->id, "inst1");
}

TEST_F(RegistryDeltaTest, RenewDoesNotGenerateDelta)
{
    reg_.subscribe("sub1");
    reg_.register_instance(MakeInst("inst1"), 30s, true);
    reg_.pop_deltas("sub1");  // 消费掉第一条

    // 续约（is_new_instance=false）不应产生新 delta
    reg_.register_instance(MakeInst("inst1"), 30s, false);
    auto deltas = reg_.pop_deltas("sub1");
    EXPECT_TRUE(deltas.empty());
}

TEST_F(RegistryDeltaTest, DeregisterGeneratesOfflineDelta)
{
    reg_.subscribe("sub1");
    reg_.register_instance(MakeInst("inst1"), 30s, true);
    reg_.pop_deltas("sub1");  // 消费 UPSERT

    reg_.deregister_instance("svc", "inst1");
    auto deltas = reg_.pop_deltas("sub1");
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].op, DeltaEntry::Op::OFFLINE);
}

TEST_F(RegistryDeltaTest, PopClearsQueue)
{
    reg_.subscribe("sub1");
    reg_.register_instance(MakeInst("a"), 30s, true);
    reg_.register_instance(MakeInst("b"), 30s, true);

    auto d1 = reg_.pop_deltas("sub1");
    EXPECT_EQ(d1.size(), 2u);

    // 第二次 pop 应为空
    auto d2 = reg_.pop_deltas("sub1");
    EXPECT_TRUE(d2.empty());
}

TEST_F(RegistryDeltaTest, SelfChangeNotPushedToSelf)
{
    // changed_id == subscriber_id 时不推送给自己
    reg_.subscribe("inst1");
    reg_.register_instance(MakeInst("inst1"), 30s, true);

    // push_delta_to_others 以 inst1 为 changed_id，
    // 所以 sub1=inst1 的 queue 不应收到
    auto deltas = reg_.pop_deltas("inst1");
    EXPECT_TRUE(deltas.empty());
}

TEST_F(RegistryDeltaTest, TwoSubscribersIndependent)
{
    reg_.subscribe("sub1");
    reg_.subscribe("sub2");

    reg_.register_instance(MakeInst("inst1"), 30s, true);

    // sub1 消费，不影响 sub2
    auto d1 = reg_.pop_deltas("sub1");
    EXPECT_EQ(d1.size(), 1u);

    auto d2 = reg_.pop_deltas("sub2");
    EXPECT_EQ(d2.size(), 1u);

    // 再次 pop 都为空
    EXPECT_TRUE(reg_.pop_deltas("sub1").empty());
    EXPECT_TRUE(reg_.pop_deltas("sub2").empty());
}

TEST_F(RegistryDeltaTest, UnsubscribeNoCrash)
{
    reg_.subscribe("sub1");
    reg_.unsubscribe("sub1");

    // 注销后 pop_deltas 应返回空，不崩溃
    EXPECT_NO_THROW({
        auto d = reg_.pop_deltas("sub1");
        EXPECT_TRUE(d.empty());
    });
}

TEST_F(RegistryDeltaTest, ExpiredCleanupGeneratesOfflineDelta)
{
    reg_.subscribe("sub1");
    reg_.register_instance(MakeInst("exp"), 1s, true);
    reg_.pop_deltas("sub1");  // 消费 UPSERT

    std::this_thread::sleep_for(1100ms);
    reg_.cleanup_expired();

    auto deltas = reg_.pop_deltas("sub1");
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].op, DeltaEntry::Op::OFFLINE);
    EXPECT_EQ(deltas[0].inst->id, "exp");
}

// ══════════════════════════════════════════════
// 4. Concurrent - 多线程无数据竞争
// ══════════════════════════════════════════════
TEST(RegistryConcurrent, ConcurrentRegisterAndGet)
{
    ServiceRegistry reg;
    constexpr int kThreads = 4;
    constexpr int kIters   = 500;

    std::vector<std::thread> writers, readers;

    for (int t = 0; t < kThreads; ++t)
    {
        writers.emplace_back([&, t]() {
            for (int i = 0; i < kIters; ++i)
            {
                auto inst = MakeInst("inst_" + std::to_string(t) + "_" + std::to_string(i));
                reg.register_instance(inst, 30s, true);
            }
        });
        readers.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i)
                reg.get_instances("svc");
        });
    }

    for (auto& t : writers) t.join();
    for (auto& t : readers) t.join();

    // 若有数据竞争，TSan/ASan 会报告；否则只验证函数正常完成
    SUCCEED();
}

// ══════════════════════════════════════════════
// 5. 扩展测试
// ══════════════════════════════════════════════

// re-register 同 id 但修改 address/weight，get_by_id 返回新值
TEST_F(RegistryRegisterTest, UpdateInstanceProperties)
{
    auto inst1 = MakeInst("inst1");
    inst1->address = "10.0.0.1";
    inst1->weight  = 1;
    reg_.register_instance(inst1, 30s, true);

    auto inst2 = MakeInst("inst1");  // 同 id
    inst2->address = "10.0.0.2";
    inst2->weight  = 5;
    reg_.register_instance(inst2, 30s, false);  // 续约同时更新

    auto found = reg_.get_by_id("svc", "inst1");
    ASSERT_NE(found, nullptr);
    // 续约会更新 instance 对象
    EXPECT_EQ(found->weight, 5u);
}

// 注册 2 个不同服务，snapshot 包含两者
TEST_F(RegistryRegisterTest, SnapshotContainsAllServices)
{
    auto inst_a = MakeInst("a", "svc_a");
    auto inst_b = MakeInst("b", "svc_b");
    reg_.register_instance(inst_a, 30s, true);
    reg_.register_instance(inst_b, 30s, true);

    auto snap = reg_.snapshot();
    EXPECT_EQ(snap.count("svc_a"), 1u);
    EXPECT_EQ(snap.count("svc_b"), 1u);
}

// 注册后 routing_table_string() 非空
TEST_F(RegistryRegisterTest, RoutingTableStringNotEmpty)
{
    reg_.register_instance(MakeInst("inst1"), 30s, true);
    std::string table = reg_.routing_table_string();
    EXPECT_FALSE(table.empty());
    // 应包含服务名或实例 id 的部分信息
    EXPECT_NE(table.find("svc"), std::string::npos);
}

// 注册 3 个短 TTL 实例，全过期后一次 cleanup 清掉全部
TEST_F(RegistryTTLTest, MultipleExpiriesInOneCleanup)
{
    for (int i = 0; i < 3; ++i)
        reg_.register_instance(MakeInst("exp_" + std::to_string(i)), 1s, true);

    EXPECT_EQ(reg_.get_instances("svc").size(), 3u);

    std::this_thread::sleep_for(1100ms);
    reg_.cleanup_expired();

    EXPECT_TRUE(reg_.get_instances("svc").empty());
}

// 注册 3 个实例，pop_deltas 得到 3 条
TEST_F(RegistryDeltaTest, MultipleRegistrationsMultipleDeltas)
{
    reg_.subscribe("sub1");
    for (int i = 0; i < 3; ++i)
        reg_.register_instance(MakeInst("inst_" + std::to_string(i)), 30s, true);

    auto deltas = reg_.pop_deltas("sub1");
    EXPECT_EQ(deltas.size(), 3u);
    for (auto& d : deltas)
        EXPECT_EQ(d.op, DeltaEntry::Op::UPSERT);
}

// 同 id 重新注册 is_new_instance=true，再次产生 UPSERT delta
TEST_F(RegistryDeltaTest, UpdateRegistrationGeneratesUpsert)
{
    reg_.subscribe("sub1");
    reg_.register_instance(MakeInst("inst1"), 30s, true);
    reg_.pop_deltas("sub1");  // 消费第一次

    // 再次注册同 id，is_new_instance=true
    reg_.register_instance(MakeInst("inst1"), 30s, true);
    auto deltas = reg_.pop_deltas("sub1");
    ASSERT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].op, DeltaEntry::Op::UPSERT);
}

// 并发：2 线程写注册 + 2 线程 pop_deltas，无数据竞争
TEST(RegistryConcurrent, ConcurrentSubscribeAndPop)
{
    ServiceRegistry reg;
    reg.subscribe("reader1");
    reg.subscribe("reader2");

    constexpr int kIters = 200;
    std::vector<std::thread> threads;

    // 写线程：注册实例
    for (int t = 0; t < 2; ++t)
    {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kIters; ++i)
            {
                auto inst = MakeInst("inst_" + std::to_string(t) + "_" + std::to_string(i));
                reg.register_instance(inst, 30s, true);
            }
        });
    }

    // 读线程：pop deltas
    for (auto& sub : {"reader1", "reader2"})
    {
        std::string sub_id = sub;
        threads.emplace_back([&reg, sub_id]() {
            for (int i = 0; i < kIters; ++i)
                reg.pop_deltas(sub_id);
        });
    }

    for (auto& t : threads) t.join();
    SUCCEED();
}

// subscribe() 幂等性：同一 id 重复 subscribe 不会覆盖已有队列
TEST_F(RegistryDeltaTest, SubscribeIdempotent)
{
    reg_.subscribe("sub_idem");

    // 注册一个实例，产生一条 delta
    reg_.register_instance(MakeInst("inst_before"), 30s, true);

    // 再次 subscribe 同一 id（应为 no-op，不清空队列）
    reg_.subscribe("sub_idem");

    // 队列里应还有第一条 delta
    auto deltas = reg_.pop_deltas("sub_idem");
    EXPECT_EQ(deltas.size(), 1u);
    EXPECT_EQ(deltas[0].inst->id, "inst_before");
}
