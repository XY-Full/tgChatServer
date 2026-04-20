/**
 * test_helper_forward.cpp
 * Helper::ForwardRawAppMsg 单元测试
 *
 * 覆盖：
 *  - header 字段完整复制（conn_id, msg_id, type, data_len 等）
 *  - payload bytes 完整复制（memcpy 正确性）
 *  - data_ 指针指向新 slab 块中 AppMsg 结构体之后（不是原始位置）
 *  - dst_ 字段等于传入的 dst_service 字符串
 *  - dst_ 超长时安全截断（不越界）
 *  - data_len_=0 / data_=nullptr 时正常返回（不崩溃，data_ 可为 nullptr）
 *  - shared_ptr 析构后 slab 内存归还（通过再次 Alloc 不 OOM 验证）
 *  - 多次连续调用不 OOM（slab 每次 Free 后可重用）
 *  - 并发调用不崩溃、不数据竞争
 */

#include <gtest/gtest.h>

#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Helper.h"
#include "network/AppMsg.h"
#include "network/MsgWrapper.h"
#include "GlobalSpace.h"

// ─────────────────────────────────────────────
// 辅助：构造带 payload 的 AppMsg（栈分配）
// ─────────────────────────────────────────────

struct StackAppMsg
{
    AppMsg            msg;
    std::vector<char> payload;

    // payload_data: 要放入 data_ 的字节内容
    explicit StackAppMsg(uint16_t msg_id, uint64_t conn_id,
                         const std::string& payload_str = "")
    {
        memset(&msg, 0, sizeof(AppMsg));
        msg.header_.version_   = MAGIC_VERSION;
        msg.header_.type_      = Type::C2S;
        msg.header_.conn_id_   = conn_id;
        msg.header_.seq_       = 99;
        msg.msg_id_            = msg_id;
        msg.data_len_          = static_cast<uint16_t>(payload_str.size());

        if (!payload_str.empty())
        {
            payload.assign(payload_str.begin(), payload_str.end());
            msg.data_ = payload.data();
        }
        else
        {
            msg.data_ = nullptr;
        }
    }
};

// ─────────────────────────────────────────────
// Fixture
// ─────────────────────────────────────────────

class ForwardRawAppMsgTest : public ::testing::Test
{
protected:
    // GlobalSpace()->shm_slab_ 是静态的，在进程级别初始化一次即可。
    // Helper::ForwardRawAppMsg 内部使用 GlobalSpace()->shm_slab_。
    // 无需额外 SetUp。
};

// ─────────────────────────────────────────────
// 1. header 字段完整复制
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, HeaderFieldsCopied)
{
    StackAppMsg src(42, 12345, "hello");

    auto wrap = Helper::ForwardRawAppMsg(src.msg, "LogicService");
    ASSERT_NE(wrap, nullptr);

    // 从 slab 取回 AppMsg
    auto* dst = reinterpret_cast<AppMsg*>(
        GlobalSpace()->shm_slab_.off2ptr(wrap->offset_));
    ASSERT_NE(dst, nullptr);

    EXPECT_EQ(dst->header_.conn_id_, 12345u);
    EXPECT_EQ(dst->header_.seq_,     99u);
    EXPECT_EQ(dst->header_.type_,    Type::C2S);
    EXPECT_EQ(dst->msg_id_,          42u);
    EXPECT_EQ(dst->data_len_,        static_cast<uint16_t>(5));
}

// ─────────────────────────────────────────────
// 2. payload bytes 完整复制
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, PayloadBytesCopied)
{
    std::string payload = "unit_test_payload_data_12345";
    StackAppMsg src(10, 1, payload);

    auto wrap = Helper::ForwardRawAppMsg(src.msg, "dst");
    ASSERT_NE(wrap, nullptr);

    auto* dst = reinterpret_cast<AppMsg*>(
        GlobalSpace()->shm_slab_.off2ptr(wrap->offset_));
    ASSERT_NE(dst, nullptr);
    ASSERT_NE(dst->data_, nullptr);

    std::string copied(dst->data_, dst->data_len_);
    EXPECT_EQ(copied, payload);
}

// ─────────────────────────────────────────────
// 3. data_ 指针指向新 slab 块内部（不是原始地址）
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, DataPointerPointsIntoNewSlab)
{
    std::string payload = "ptr_check";
    StackAppMsg src(5, 2, payload);

    auto wrap = Helper::ForwardRawAppMsg(src.msg, "");
    ASSERT_NE(wrap, nullptr);

    auto* dst = reinterpret_cast<AppMsg*>(
        GlobalSpace()->shm_slab_.off2ptr(wrap->offset_));
    ASSERT_NE(dst, nullptr);

    // data_ 应等于 dst 结构体之后的地址
    char* expected_data_ptr = reinterpret_cast<char*>(dst) + sizeof(AppMsg);
    EXPECT_EQ(dst->data_, expected_data_ptr);

    // 不应等于原始 payload 地址
    EXPECT_NE(dst->data_, src.msg.data_);
}

// ─────────────────────────────────────────────
// 4. dst_ 字段正确设置
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, DstFieldSetCorrectly)
{
    StackAppMsg src(1, 3, "x");

    auto wrap = Helper::ForwardRawAppMsg(src.msg, "LogicService");
    ASSERT_NE(wrap, nullptr);

    EXPECT_STREQ(wrap->dst_, "LogicService");
}

TEST_F(ForwardRawAppMsgTest, EmptyDstField)
{
    StackAppMsg src(1, 3, "x");

    auto wrap = Helper::ForwardRawAppMsg(src.msg, "");
    ASSERT_NE(wrap, nullptr);

    EXPECT_STREQ(wrap->dst_, "");
}

// ─────────────────────────────────────────────
// 5. dst_ 超长时安全截断（不越界写）
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, LongDstTruncated)
{
    // dst_ 是 char[16]，超过 15 字符应被截断
    std::string long_dst(100, 'X');
    StackAppMsg src(1, 4, "y");

    auto wrap = Helper::ForwardRawAppMsg(src.msg, long_dst);
    ASSERT_NE(wrap, nullptr);

    // dst_[15] 必须是 '\0'（null 终止）
    EXPECT_EQ(wrap->dst_[15], '\0');
    // dst_ 长度 <= 15
    EXPECT_LE(strlen(wrap->dst_), 15u);
}

// ─────────────────────────────────────────────
// 6. data_len_=0 不崩溃，data_ 可为 nullptr
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, EmptyPayloadNoCrash)
{
    StackAppMsg src(7, 5, "");  // data_len_=0, data_=nullptr

    ASSERT_EQ(src.msg.data_len_, 0);
    ASSERT_EQ(src.msg.data_,     nullptr);

    auto wrap = Helper::ForwardRawAppMsg(src.msg, "SomeService");
    ASSERT_NE(wrap, nullptr);

    auto* dst = reinterpret_cast<AppMsg*>(
        GlobalSpace()->shm_slab_.off2ptr(wrap->offset_));
    ASSERT_NE(dst, nullptr);
    EXPECT_EQ(dst->data_len_, 0);
}

// ─────────────────────────────────────────────
// 7. shared_ptr 析构后 slab 内存可被再次分配（不 OOM）
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, SlabMemoryFreedOnDestroy)
{
    std::string payload(256, 'A');  // 256 字节 payload
    StackAppMsg src(9, 6, payload);

    constexpr int kRounds = 200;
    for (int i = 0; i < kRounds; ++i)
    {
        auto wrap = Helper::ForwardRawAppMsg(src.msg, "test");
        // wrap 在这里析构 → Free 归还 slab
        EXPECT_NE(wrap, nullptr) << "Alloc failed at round " << i;
    }
    // 若 slab 内存未归还，200 次后早已 OOM，EXPECT_NE 会失败
}

// ─────────────────────────────────────────────
// 8. 多条消息同时存活（slab 可容纳多块）
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, MultipleWrappersConcurrently)
{
    std::vector<std::shared_ptr<AppMsgWrapper>> alive;
    alive.reserve(20);

    for (int i = 0; i < 20; ++i)
    {
        std::string payload = "msg_" + std::to_string(i);
        StackAppMsg src(static_cast<uint16_t>(i), static_cast<uint64_t>(i), payload);
        auto wrap = Helper::ForwardRawAppMsg(src.msg, "multi");
        ASSERT_NE(wrap, nullptr) << "Alloc failed at i=" << i;

        // 验证内容正确
        auto* dst = reinterpret_cast<AppMsg*>(
            GlobalSpace()->shm_slab_.off2ptr(wrap->offset_));
        std::string copied(dst->data_, dst->data_len_);
        EXPECT_EQ(copied, payload);

        alive.push_back(wrap);
    }
    // alive 析构，所有 slab 块归还
}

// ─────────────────────────────────────────────
// 9. 大 payload（接近 slab 单次分配上限）
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, LargePayload)
{
    // slab 单次 Alloc 上限受 RoundToClassSize 约束（最大 size-class ≤ 8192）
    // 取 4096 - sizeof(AppMsg) 确保在支持范围内
    constexpr size_t kPayloadSize = 4096 - sizeof(AppMsg);
    std::string payload(kPayloadSize, 'Z');
    StackAppMsg src(99, 999, payload);

    auto wrap = Helper::ForwardRawAppMsg(src.msg, "big");
    ASSERT_NE(wrap, nullptr);

    auto* dst = reinterpret_cast<AppMsg*>(
        GlobalSpace()->shm_slab_.off2ptr(wrap->offset_));
    ASSERT_NE(dst, nullptr);
    EXPECT_EQ(dst->data_len_, static_cast<uint16_t>(kPayloadSize));

    std::string copied(dst->data_, dst->data_len_);
    EXPECT_EQ(copied, payload);
}

// ─────────────────────────────────────────────
// 10. 并发调用不崩溃、slab 线程安全
// ─────────────────────────────────────────────

TEST_F(ForwardRawAppMsgTest, ConcurrentCallsNoCrash)
{
    constexpr int kThreads   = 4;
    constexpr int kPerThread = 100;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    std::atomic<int> success_count{0};

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]() {
            for (int i = 0; i < kPerThread; ++i)
            {
                std::string payload = "thread_" + std::to_string(t) + "_" + std::to_string(i);
                StackAppMsg src(static_cast<uint16_t>(t * 100 + i),
                                static_cast<uint64_t>(t * 1000 + i),
                                payload);

                auto wrap = Helper::ForwardRawAppMsg(src.msg, "concurrent");
                if (wrap)
                {
                    // 验证内容
                    auto* dst = reinterpret_cast<AppMsg*>(
                        GlobalSpace()->shm_slab_.off2ptr(wrap->offset_));
                    if (dst && dst->data_ && dst->data_len_ == payload.size())
                    {
                        std::string copied(dst->data_, dst->data_len_);
                        if (copied == payload)
                            success_count.fetch_add(1, std::memory_order_relaxed);
                    }
                    // wrap 析构 → Free
                }
            }
        });
    }

    for (auto& th : threads) th.join();

    EXPECT_EQ(success_count.load(), kThreads * kPerThread);
}
