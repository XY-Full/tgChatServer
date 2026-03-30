/**
 * test_session_manager.cpp
 * SessionManager 单元测试
 *
 * 覆盖：
 *  - on_connect / on_disconnect 基础生命周期
 *  - on_auth_ok 首次绑定（返回 nullopt）
 *  - on_auth_ok 防重复登录（返回旧 conn_id）
 *  - is_authed / get_user_id / find_conn_by_user
 *  - online_count 计数
 *  - get_session proto、connected_at
 *  - 并发：8 线程并发 connect / auth / disconnect 无数据竞争
 */

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "SessionManager.h"

// ──────────────────────────────────────────────
// Fixture
// ──────────────────────────────────────────────
class SessionMgrTest : public ::testing::Test
{
protected:
    SessionManager mgr_;
};

// ──────────────────────────────────────────────
// 1. on_connect / on_disconnect 基础
// ──────────────────────────────────────────────
TEST_F(SessionMgrTest, ConnectIncreasesCount)
{
    mgr_.on_connect(101, "tcp");
    EXPECT_EQ(mgr_.online_count(), 1u);

    mgr_.on_connect(102, "ws");
    EXPECT_EQ(mgr_.online_count(), 2u);
}

TEST_F(SessionMgrTest, DisconnectDecreasesCount)
{
    mgr_.on_connect(101, "tcp");
    mgr_.on_disconnect(101);
    EXPECT_EQ(mgr_.online_count(), 0u);
}

TEST_F(SessionMgrTest, DisconnectNonExistentNoCrash)
{
    EXPECT_NO_THROW(mgr_.on_disconnect(999));
}

TEST_F(SessionMgrTest, NewConnectionNotAuthed)
{
    mgr_.on_connect(101, "tcp");
    EXPECT_FALSE(mgr_.is_authed(101));
    EXPECT_TRUE(mgr_.get_user_id(101).empty());
}

// ──────────────────────────────────────────────
// 2. on_auth_ok 首次绑定
// ──────────────────────────────────────────────
TEST_F(SessionMgrTest, AuthOkFirstTime)
{
    mgr_.on_connect(101, "tcp");
    auto old = mgr_.on_auth_ok(101, "user1");

    EXPECT_FALSE(old.has_value());   // 无旧连接
    EXPECT_TRUE(mgr_.is_authed(101));
    EXPECT_EQ(mgr_.get_user_id(101), "user1");
}

TEST_F(SessionMgrTest, FindConnByUserAfterAuth)
{
    mgr_.on_connect(101, "tcp");
    mgr_.on_auth_ok(101, "user1");

    auto found = mgr_.find_conn_by_user("user1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, 101u);
}

TEST_F(SessionMgrTest, FindConnByUserNotFound)
{
    auto result = mgr_.find_conn_by_user("ghost");
    EXPECT_FALSE(result.has_value());
}

// ──────────────────────────────────────────────
// 3. 防重复登录
// ──────────────────────────────────────────────
TEST_F(SessionMgrTest, DuplicateLoginReturnsOldConn)
{
    // 第一个连接鉴权
    mgr_.on_connect(101, "tcp");
    mgr_.on_auth_ok(101, "user1");

    // 第二个连接尝试以同一 user_id 鉴权
    mgr_.on_connect(102, "ws");
    auto old = mgr_.on_auth_ok(102, "user1");

    ASSERT_TRUE(old.has_value());
    EXPECT_EQ(*old, 101u);  // 返回旧连接 ID
}

TEST_F(SessionMgrTest, KickOldAndRebind)
{
    mgr_.on_connect(101, "tcp");
    mgr_.on_auth_ok(101, "user1");

    mgr_.on_connect(102, "ws");
    auto old = mgr_.on_auth_ok(102, "user1"); // 返回 101

    // 调用方踢掉旧连接
    ASSERT_TRUE(old.has_value());
    mgr_.on_disconnect(*old);

    // 再次绑定
    auto r2 = mgr_.on_auth_ok(102, "user1");
    EXPECT_FALSE(r2.has_value());  // 成功绑定，无旧连接
    EXPECT_TRUE(mgr_.is_authed(102));
    EXPECT_EQ(*mgr_.find_conn_by_user("user1"), 102u);
}

// ──────────────────────────────────────────────
// 4. on_disconnect 清理
// ──────────────────────────────────────────────
TEST_F(SessionMgrTest, DisconnectClearsAuthState)
{
    mgr_.on_connect(101, "tcp");
    mgr_.on_auth_ok(101, "user1");
    mgr_.on_disconnect(101);

    EXPECT_FALSE(mgr_.is_authed(101));
    EXPECT_TRUE(mgr_.get_user_id(101).empty());
    EXPECT_FALSE(mgr_.find_conn_by_user("user1").has_value());
    EXPECT_EQ(mgr_.online_count(), 0u);
}

TEST_F(SessionMgrTest, DisconnectDoesNotRemoveNewerConnectionMapping)
{
    // 踢掉旧连接后，新连接的 user→conn 映射不应被清除
    mgr_.on_connect(101, "tcp");
    mgr_.on_auth_ok(101, "user1");

    mgr_.on_connect(102, "ws");
    mgr_.on_auth_ok(102, "user1"); // 返回旧 conn，但未绑定
    // 模拟：先踢掉 101，然后 102 绑定
    mgr_.on_disconnect(101);
    mgr_.on_auth_ok(102, "user1");

    auto found = mgr_.find_conn_by_user("user1");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, 102u);
}

// ──────────────────────────────────────────────
// 5. get_session
// ──────────────────────────────────────────────
TEST_F(SessionMgrTest, GetSessionProto)
{
    mgr_.on_connect(101, "kcp");
    auto sess = mgr_.get_session(101);
    ASSERT_TRUE(sess.has_value());
    EXPECT_EQ(sess->proto, "kcp");
    EXPECT_EQ(sess->conn_id, 101u);
    EXPECT_FALSE(sess->authed);
}

TEST_F(SessionMgrTest, GetSessionAfterAuth)
{
    mgr_.on_connect(101, "tcp");
    mgr_.on_auth_ok(101, "userX");
    auto sess = mgr_.get_session(101);
    ASSERT_TRUE(sess.has_value());
    EXPECT_TRUE(sess->authed);
    EXPECT_EQ(sess->user_id, "userX");
}

TEST_F(SessionMgrTest, GetSessionNotFound)
{
    EXPECT_FALSE(mgr_.get_session(999).has_value());
}

TEST_F(SessionMgrTest, GetSessionConnectedAt)
{
    auto before = std::chrono::steady_clock::now();
    mgr_.on_connect(101, "tcp");
    auto after  = std::chrono::steady_clock::now();

    auto sess = mgr_.get_session(101);
    ASSERT_TRUE(sess.has_value());
    EXPECT_GE(sess->connected_at, before);
    EXPECT_LE(sess->connected_at, after);
}

// ──────────────────────────────────────────────
// 6. 并发安全
// ──────────────────────────────────────────────
TEST_F(SessionMgrTest, ConcurrentConnectAuthDisconnect)
{
    constexpr int kThreads = 8;
    constexpr int kIters   = 200;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]() {
            uint64_t base_conn = static_cast<uint64_t>(t) * 10000;
            for (int i = 0; i < kIters; ++i)
            {
                uint64_t conn_id = base_conn + i;
                std::string user_id = "user_" + std::to_string(t) + "_" + std::to_string(i);

                mgr_.on_connect(conn_id, "tcp");
                auto old = mgr_.on_auth_ok(conn_id, user_id);
                // 若有旧连接（同用户 ID），先踢掉再重试（测试中不太可能发生，但处理完整）
                if (old.has_value())
                {
                    mgr_.on_disconnect(*old);
                    mgr_.on_auth_ok(conn_id, user_id);
                }
                mgr_.is_authed(conn_id);
                mgr_.get_user_id(conn_id);
                mgr_.on_disconnect(conn_id);
            }
        });
    }

    for (auto& t : threads) t.join();

    // 所有线程完成后，状态应全部清空
    EXPECT_EQ(mgr_.online_count(), 0u);
}

// ──────────────────────────────────────────────
// 7. 扩展测试
// ──────────────────────────────────────────────

// 大量 connect/disconnect 循环后 online_count 归零
TEST_F(SessionMgrTest, OnlineCountAfterMultipleConnectDisconnect)
{
    for (int i = 0; i < 1000; ++i)
    {
        mgr_.on_connect(static_cast<uint64_t>(i), "tcp");
    }
    EXPECT_EQ(mgr_.online_count(), 1000u);

    for (int i = 0; i < 1000; ++i)
    {
        mgr_.on_disconnect(static_cast<uint64_t>(i));
    }
    EXPECT_EQ(mgr_.online_count(), 0u);
}

// on_auth_ok 传入空 user_id：不崩溃
TEST_F(SessionMgrTest, AuthWithEmptyUserId)
{
    mgr_.on_connect(200, "tcp");
    EXPECT_NO_THROW({
        auto r = mgr_.on_auth_ok(200, "");
        // 空 user_id 可能绑定或不绑定，只要不崩溃即可
        (void)r;
    });
}

// 未 connect 的 conn_id 调用 get_user_id 返回空字符串
TEST_F(SessionMgrTest, GetUserIdBeforeConnect)
{
    EXPECT_TRUE(mgr_.get_user_id(99999).empty());
}

// auth 后 disconnect，find_conn_by_user 返回 nullopt
TEST_F(SessionMgrTest, FindConnAfterDisconnect)
{
    mgr_.on_connect(300, "tcp");
    mgr_.on_auth_ok(300, "userZ");
    ASSERT_TRUE(mgr_.find_conn_by_user("userZ").has_value());

    mgr_.on_disconnect(300);
    EXPECT_FALSE(mgr_.find_conn_by_user("userZ").has_value());
}

// 100 个不同用户同时在线，各自独立互不干扰
TEST_F(SessionMgrTest, MultipleUsersIndependent)
{
    constexpr int kUsers = 100;

    for (int i = 0; i < kUsers; ++i)
    {
        mgr_.on_connect(static_cast<uint64_t>(i + 1000), "tcp");
        mgr_.on_auth_ok(static_cast<uint64_t>(i + 1000), "user_" + std::to_string(i));
    }

    EXPECT_EQ(mgr_.online_count(), static_cast<size_t>(kUsers));

    for (int i = 0; i < kUsers; ++i)
    {
        auto found = mgr_.find_conn_by_user("user_" + std::to_string(i));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, static_cast<uint64_t>(i + 1000));
    }
}

// on_auth_ok 在 on_connect 之前调用（conn_id 不在 sessions_ 中）
// 预期：如果 user_id 无旧连接，找不到 session，返回 nullopt；且 user_id 未被绑定
TEST_F(SessionMgrTest, AuthWithoutPriorConnect)
{
    // conn_id=777 从未 on_connect，直接调用 on_auth_ok
    auto result = mgr_.on_auth_ok(777, "orphan_user");

    // 按实现：sessions_.find(777) == end，直接返回 nullopt
    EXPECT_FALSE(result.has_value());

    // user_id 没有绑定到任何连接
    EXPECT_FALSE(mgr_.find_conn_by_user("orphan_user").has_value());

    // conn_id 777 不在 sessions_，is_authed 应返回 false
    EXPECT_FALSE(mgr_.is_authed(777));
}
