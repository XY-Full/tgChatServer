/**
 * test_shm_ringbuffer.cpp
 * ShmRingBuffer 单元测试
 *
 * 覆盖：
 *  BasicOps   - 初始状态、Push/Pop 基础、满/空边界
 *  BatchOps   - 批量 Push/Pop、Drop、Peek、PushFront（uint8_t 特化）
 *  WrapAround - head/tail 绕回边界，Size() 计算正确性
 *  Concurrent - 多生产者/多消费者并发安全
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>   // shm_unlink
#include <unistd.h>     // getpid

#include "shm_ringbuffer.h"

// ──────────────────────────────────────────────
// 辅助
// ──────────────────────────────────────────────
static std::atomic<int> g_rb_counter{0};

static std::string MakeRbName(const std::string& tag)
{
    return "/test_rb_" + std::to_string(getpid()) + "_" + tag + "_" +
           std::to_string(g_rb_counter.fetch_add(1));
}

// 清理共享内存文件（RingBuffer 用到两个：数据区 + lock）
static void CleanupRb(const std::string& name)
{
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// ══════════════════════════════════════════════
// 1. BasicOps - int 类型，单线程
// ══════════════════════════════════════════════
class RbBasicTest : public ::testing::Test
{
protected:
    static constexpr size_t kCap = 8; // ring_size=8 => 最多存 7 个元素

    void SetUp() override
    {
        name_ = MakeRbName("basic");
        rb_   = std::make_unique<ShmRingBuffer<int>>(name_, kCap);
    }
    void TearDown() override
    {
        rb_.reset();
        CleanupRb(name_);
    }

    std::string name_;
    std::unique_ptr<ShmRingBuffer<int>> rb_;
};

TEST_F(RbBasicTest, InitialState)
{
    EXPECT_TRUE(rb_->IsEmpty());
    EXPECT_FALSE(rb_->IsFull());
    EXPECT_EQ(rb_->Size(),     0u);
    EXPECT_EQ(rb_->Capacity(), kCap);
}

TEST_F(RbBasicTest, PushPopSingle)
{
    EXPECT_TRUE(rb_->Push(42));
    EXPECT_EQ(rb_->Size(), 1u);
    EXPECT_FALSE(rb_->IsEmpty());

    int v = 0;
    EXPECT_TRUE(rb_->Pop(v));
    EXPECT_EQ(v, 42);
    EXPECT_TRUE(rb_->IsEmpty());
}

TEST_F(RbBasicTest, PushUntilFull)
{
    // ring_size=8 => 满时 tail+1 == head => 最多推 kCap-1 = 7 个
    const size_t max = kCap - 1;
    for (size_t i = 0; i < max; ++i)
    {
        EXPECT_TRUE(rb_->Push(static_cast<int>(i))) << "i=" << i;
    }
    EXPECT_TRUE(rb_->IsFull());
    EXPECT_EQ(rb_->Size(), max);
}

TEST_F(RbBasicTest, PushWhenFull)
{
    for (size_t i = 0; i < kCap - 1; ++i)
        rb_->Push(0);
    EXPECT_FALSE(rb_->Push(99)); // 已满，应返回 false
}

TEST_F(RbBasicTest, PopWhenEmpty)
{
    int v = 0;
    EXPECT_FALSE(rb_->Pop(v));
}

TEST_F(RbBasicTest, FIFOOrder)
{
    for (int i = 0; i < 5; ++i)
        EXPECT_TRUE(rb_->Push(i));
    for (int i = 0; i < 5; ++i)
    {
        int v = -1;
        EXPECT_TRUE(rb_->Pop(v));
        EXPECT_EQ(v, i);
    }
}

TEST_F(RbBasicTest, Reset)
{
    rb_->Push(1);
    rb_->Push(2);
    rb_->Reset();
    EXPECT_TRUE(rb_->IsEmpty());
    EXPECT_EQ(rb_->Size(), 0u);
}

TEST_F(RbBasicTest, GetShmName)
{
    EXPECT_EQ(rb_->GetShmName(), name_);
}

// ══════════════════════════════════════════════
// 2. BatchOps & uint8_t 特化路径
// ══════════════════════════════════════════════
class RbBatchTest : public ::testing::Test
{
protected:
    static constexpr size_t kCap = 256;

    void SetUp() override
    {
        name_ = MakeRbName("batch");
        rb_   = std::make_unique<ShmRingBuffer<uint8_t>>(name_, kCap);
    }
    void TearDown() override
    {
        rb_.reset();
        CleanupRb(name_);
    }

    std::string name_;
    std::unique_ptr<ShmRingBuffer<uint8_t>> rb_;
};

TEST_F(RbBatchTest, BatchPushPop)
{
    constexpr size_t N = 50;
    uint8_t src[N], dst[N] = {};
    for (size_t i = 0; i < N; ++i) src[i] = static_cast<uint8_t>(i);

    EXPECT_TRUE(rb_->Push(src, N));
    EXPECT_EQ(rb_->Size(), N);

    EXPECT_TRUE(rb_->Pop(dst, N));
    EXPECT_TRUE(rb_->IsEmpty());
    for (size_t i = 0; i < N; ++i)
        EXPECT_EQ(dst[i], src[i]) << "mismatch at i=" << i;
}

TEST_F(RbBatchTest, Drop)
{
    uint8_t src[10] = {0,1,2,3,4,5,6,7,8,9};
    rb_->Push(src, 10);
    EXPECT_EQ(rb_->Size(), 10u);

    EXPECT_TRUE(rb_->Drop(3));
    EXPECT_EQ(rb_->Size(), 7u);

    uint8_t v = 0;
    EXPECT_TRUE(rb_->Pop(v));
    EXPECT_EQ(v, 3u); // 头已跳过 0,1,2
}

TEST_F(RbBatchTest, DropMoreThanSize)
{
    uint8_t src[5] = {10,20,30,40,50};
    rb_->Push(src, 5);

    // 丢弃超过实际数量，应仅丢弃全部
    EXPECT_TRUE(rb_->Drop(100));
    EXPECT_TRUE(rb_->IsEmpty());
}

TEST_F(RbBatchTest, DropEmpty)
{
    EXPECT_FALSE(rb_->Drop(1));
}

TEST_F(RbBatchTest, Peek)
{
    uint8_t src[4] = {1,2,3,4};
    rb_->Push(src, 4);

    uint8_t peek_buf[4] = {};
    EXPECT_TRUE(rb_->Peek(peek_buf, 4));

    // Peek 不消费数据
    EXPECT_EQ(rb_->Size(), 4u);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(peek_buf[i], src[i]);
}

TEST_F(RbBatchTest, PushFront)
{
    uint8_t tail_data[3] = {10, 20, 30};
    rb_->Push(tail_data, 3);

    uint8_t front_data[2] = {1, 2};
    EXPECT_TRUE(rb_->PushFront(front_data, 2));

    // 头部数据应先被 Pop 出来
    uint8_t buf[5] = {};
    rb_->Pop(buf, 5);
    EXPECT_EQ(buf[0], 1u);
    EXPECT_EQ(buf[1], 2u);
    EXPECT_EQ(buf[2], 10u);
}

// ══════════════════════════════════════════════
// 3. WrapAround - head/tail 绕回
// ══════════════════════════════════════════════
class RbWrapTest : public ::testing::Test
{
protected:
    static constexpr size_t kCap = 8; // 最多 7 个元素

    void SetUp() override
    {
        name_ = MakeRbName("wrap");
        rb_   = std::make_unique<ShmRingBuffer<int>>(name_, kCap);
    }
    void TearDown() override
    {
        rb_.reset();
        CleanupRb(name_);
    }

    std::string name_;
    std::unique_ptr<ShmRingBuffer<int>> rb_;
};

TEST_F(RbWrapTest, TailWrap)
{
    // 填满，弹出 3 个，再推 3 个 -> tail 绕回
    for (int i = 0; i < 7; ++i) rb_->Push(i);
    int v;
    rb_->Pop(v); // head=1
    rb_->Pop(v); // head=2
    rb_->Pop(v); // head=3

    // 再推 3 个，tail 会绕过末端
    for (int i = 100; i < 103; ++i) EXPECT_TRUE(rb_->Push(i));
    EXPECT_EQ(rb_->Size(), 7u);

    // 弹出所有，验证顺序
    int expected[] = {3,4,5,6,100,101,102};
    for (int e : expected)
    {
        int got = -1;
        EXPECT_TRUE(rb_->Pop(got));
        EXPECT_EQ(got, e);
    }
    EXPECT_TRUE(rb_->IsEmpty());
}

TEST_F(RbWrapTest, SizeCorrectAfterWrap)
{
    // 循环多次推/弹，持续验证 Size()
    int push_count = 0, pop_count = 0;
    for (int round = 0; round < 5; ++round)
    {
        // 推满
        while (!rb_->IsFull())
        {
            rb_->Push(push_count++);
        }
        // 弹一半
        size_t half = rb_->Size() / 2;
        int v;
        for (size_t i = 0; i < half; ++i)
        {
            rb_->Pop(v);
            ++pop_count;
        }
        EXPECT_EQ(rb_->Size(), (kCap - 1) - half);
    }
}

// ══════════════════════════════════════════════
// 4. Concurrent - 多线程安全
// ══════════════════════════════════════════════
class RbConcurrentTest : public ::testing::Test
{
protected:
    static constexpr size_t kCap = 1024;

    void SetUp() override
    {
        name_ = MakeRbName("conc");
        rb_   = std::make_unique<ShmRingBuffer<uint8_t>>(name_, kCap);
    }
    void TearDown() override
    {
        rb_.reset();
        CleanupRb(name_);
    }

    std::string name_;
    std::unique_ptr<ShmRingBuffer<uint8_t>> rb_;
};

// 1 producer + 1 consumer：总数据量匹配
TEST_F(RbConcurrentTest, SingleProducerConsumer)
{
    constexpr int kTotal = 100000;
    std::atomic<int> produced{0}, consumed{0};

    std::thread producer([&]() {
        uint8_t v = 0;
        while (produced.load(std::memory_order_relaxed) < kTotal)
        {
            if (rb_->TryPush(v))
            {
                produced.fetch_add(1, std::memory_order_relaxed);
                v = static_cast<uint8_t>((v + 1) % 256);
            }
        }
    });

    std::thread consumer([&]() {
        uint8_t v = 0;
        while (consumed.load(std::memory_order_relaxed) < kTotal)
        {
            if (rb_->TryPop(v))
            {
                consumed.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });

    producer.join();
    consumer.join();

    EXPECT_EQ(produced.load(), kTotal);
    EXPECT_EQ(consumed.load(), kTotal);
}

// 4 producer + 4 consumer：总 Push 数 == 总 Pop 数
TEST_F(RbConcurrentTest, MultiProducerConsumer)
{
    constexpr int kPerProducer = 10000;
    constexpr int kProducers   = 4;
    constexpr int kConsumers   = 4;
    constexpr int kTotal       = kPerProducer * kProducers;

    std::atomic<int> pushed{0}, popped{0};
    std::atomic<bool> done{false};

    std::vector<std::thread> threads;
    for (int i = 0; i < kProducers; ++i)
    {
        threads.emplace_back([&]() {
            uint8_t v = 0;
            int local = 0;
            while (local < kPerProducer)
            {
                if (rb_->TryPush(v))
                {
                    pushed.fetch_add(1, std::memory_order_relaxed);
                    ++local;
                }
            }
        });
    }

    for (int i = 0; i < kConsumers; ++i)
    {
        threads.emplace_back([&]() {
            uint8_t v = 0;
            while (!done.load(std::memory_order_acquire) ||
                   popped.load(std::memory_order_relaxed) < kTotal)
            {
                if (rb_->TryPop(v))
                    popped.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // 等待所有 producer 完成
    for (int i = 0; i < kProducers; ++i)
        threads[i].join();
    done.store(true, std::memory_order_release);
    for (int i = kProducers; i < kProducers + kConsumers; ++i)
        threads[i].join();

    EXPECT_EQ(pushed.load(), kTotal);
    EXPECT_EQ(popped.load(), kTotal);
}

// TryPush/TryPop 非阻塞：满时 TryPush 返回 false，空时 TryPop 返回 false
TEST_F(RbConcurrentTest, TryPushTryPopNonBlocking)
{
    // 填满
    uint8_t v = 0;
    while (!rb_->IsFull())
        rb_->Push(v++);
    EXPECT_FALSE(rb_->TryPush(0));

    // 弹空
    while (!rb_->IsEmpty())
        rb_->Pop(v);
    EXPECT_FALSE(rb_->TryPop(v));
}

// ══════════════════════════════════════════════
// 5. 边界与移动语义
// ══════════════════════════════════════════════

// capacity=1 时，只能容纳 0 个元素（IsFull 立刻为真），任何 Push 都失败
TEST(RbEdgeTest, CapacityOne)
{
    std::string name = MakeRbName("cap1");
    {
        ShmRingBuffer<int> rb(name, 1);
        // ring_size=1 => (tail+1)%1 == head 始终为真，即总是 Full
        EXPECT_TRUE(rb.IsFull());
        EXPECT_FALSE(rb.Push(42));
        EXPECT_EQ(rb.Size(), 0u);
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// Drop(0) 应返回 false
TEST(RbEdgeTest, DropZeroReturnsFalse)
{
    std::string name = MakeRbName("drop0");
    {
        ShmRingBuffer<int> rb(name, 8);
        rb.Push(1);
        EXPECT_FALSE(rb.Drop(0));
        EXPECT_EQ(rb.Size(), 1u);  // 元素仍在
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// 空 buffer 上 Drop(1) 应返回 false
TEST(RbEdgeTest, DropOnEmptyReturnsFalse)
{
    std::string name = MakeRbName("dropempty");
    {
        ShmRingBuffer<int> rb(name, 8);
        EXPECT_TRUE(rb.IsEmpty());
        EXPECT_FALSE(rb.Drop(1));
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// tail 绕回后 Size() 仍正确
TEST(RbEdgeTest, SizeAfterWrap)
{
    std::string name = MakeRbName("wrap_size");
    {
        ShmRingBuffer<int> rb(name, 8);  // 最多存 7 个
        // 填满 7 个，弹出 4 个，再填 4 个 → tail 绕回
        for (int i = 0; i < 7; ++i) rb.Push(i);
        int v = 0;
        for (int i = 0; i < 4; ++i) rb.Pop(v);
        for (int i = 0; i < 4; ++i) rb.Push(100 + i);
        // 此时有 7 个元素（3原来的 + 4新的）
        EXPECT_EQ(rb.Size(), 7u);
        EXPECT_TRUE(rb.IsFull());
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// Reset 后 Size()==0，且仍可正常 Push/Pop
TEST(RbEdgeTest, ResetClearsDataAndSize)
{
    std::string name = MakeRbName("reset");
    {
        ShmRingBuffer<int> rb(name, 8);
        for (int i = 0; i < 5; ++i) rb.Push(i);
        EXPECT_EQ(rb.Size(), 5u);

        rb.Reset();
        EXPECT_EQ(rb.Size(), 0u);
        EXPECT_TRUE(rb.IsEmpty());

        // Reset 后重新使用
        rb.Push(99);
        int v = 0;
        EXPECT_TRUE(rb.Pop(v));
        EXPECT_EQ(v, 99);
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// 移动构造：移动后新对象可用，旧对象 shm_addr_ == nullptr（不调用其方法）
TEST(RbEdgeTest, MoveConstructor)
{
    std::string name = MakeRbName("move_ctor");
    {
        ShmRingBuffer<int> src(name, 8);
        src.Push(42);

        ShmRingBuffer<int> dst(std::move(src));

        // 新对象可以 Pop 出原来的数据
        int v = 0;
        EXPECT_TRUE(dst.Pop(v));
        EXPECT_EQ(v, 42);
        EXPECT_EQ(dst.GetShmName(), name);
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// 移动赋值：目标对象接管资源，可正常操作
TEST(RbEdgeTest, MoveAssignment)
{
    std::string name1 = MakeRbName("mvassign1");
    std::string name2 = MakeRbName("mvassign2");
    {
        ShmRingBuffer<int> rb1(name1, 8);
        ShmRingBuffer<int> rb2(name2, 8);

        rb1.Push(7);
        rb1.Push(8);

        rb2 = std::move(rb1);  // rb2 接管 rb1 的资源

        int v = 0;
        EXPECT_TRUE(rb2.Pop(v));
        EXPECT_EQ(v, 7);
        EXPECT_TRUE(rb2.Pop(v));
        EXPECT_EQ(v, 8);
    }
    shm_unlink(name1.c_str());
    shm_unlink((name1 + "_lock_").c_str());
    shm_unlink(name2.c_str());
    shm_unlink((name2 + "_lock_").c_str());
}

// ══════════════════════════════════════════════
// 6. uint8_t 特化边界路径
// ══════════════════════════════════════════════

// Push(items, count=0) 应返回 true（无数据推入）
TEST(RbUint8EdgeTest, PushZeroCountReturnsTrue)
{
    std::string name = MakeRbName("u8_push0");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);
        uint8_t dummy[4] = {1,2,3,4};
        EXPECT_TRUE(rb.Push(dummy, 0));
        EXPECT_EQ(rb.Size(), 0u);  // 没有推入数据
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// Pop(items, count=0) 应返回 true（无数据消费）
TEST(RbUint8EdgeTest, PopZeroCountReturnsTrue)
{
    std::string name = MakeRbName("u8_pop0");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);
        uint8_t src[4] = {1,2,3,4};
        rb.Push(src, 4);

        uint8_t dst[4] = {};
        EXPECT_TRUE(rb.Pop(dst, 0));
        EXPECT_EQ(rb.Size(), 4u);  // 没有消费数据
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// Peek(items, count=0) 应返回 true（无数据读取）
TEST(RbUint8EdgeTest, PeekZeroCountReturnsTrue)
{
    std::string name = MakeRbName("u8_peek0");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);
        uint8_t src[4] = {10,20,30,40};
        rb.Push(src, 4);

        uint8_t buf[4] = {};
        EXPECT_TRUE(rb.Peek(buf, 0));
        EXPECT_EQ(rb.Size(), 4u);  // Peek 不消费，size 不变
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// PushFront(nullptr, count>0) 应返回 false
TEST(RbUint8EdgeTest, PushFrontNullptrReturnsFalse)
{
    std::string name = MakeRbName("u8_front_null");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);
        EXPECT_FALSE(rb.PushFront(nullptr, 4));
        EXPECT_EQ(rb.Size(), 0u);
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// PushFront(items, count=0) 应返回 true（无数据推入）
TEST(RbUint8EdgeTest, PushFrontZeroCountReturnsTrue)
{
    std::string name = MakeRbName("u8_front0");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);
        uint8_t dummy[4] = {1,2,3,4};
        EXPECT_TRUE(rb.PushFront(dummy, 0));
        EXPECT_EQ(rb.Size(), 0u);
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// Push 跨越缓冲区末端（两段拷贝路径）
// 使缓冲区 tail 靠近末端，再推入跨越尾端的数据，验证数据完整性
TEST(RbUint8EdgeTest, PushTwoSegmentWrap)
{
    // ring_size=16，最多存 15 字节
    std::string name = MakeRbName("u8_push_wrap");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);

        // 先推 10 字节再弹出，使 head=0, tail=10
        uint8_t buf10[10] = {0,1,2,3,4,5,6,7,8,9};
        rb.Push(buf10, 10);
        uint8_t discard[10] = {};
        rb.Pop(discard, 10);
        // 此时 head=10, tail=10（内部实现：head=tail=10 表示空）

        // 推 8 字节：tail 从 10 写到 15（6字节），再绕到 0~1（2字节）
        uint8_t src[8] = {0xA0,0xA1,0xA2,0xA3,0xA4,0xA5,0xA6,0xA7};
        EXPECT_TRUE(rb.Push(src, 8));
        EXPECT_EQ(rb.Size(), 8u);

        // Pop 验证顺序
        uint8_t dst[8] = {};
        EXPECT_TRUE(rb.Pop(dst, 8));
        for (int i = 0; i < 8; ++i)
            EXPECT_EQ(dst[i], src[i]) << "mismatch at i=" << i;
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// Pop 跨越缓冲区末端（两段拷贝路径）
TEST(RbUint8EdgeTest, PopTwoSegmentWrap)
{
    std::string name = MakeRbName("u8_pop_wrap");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);

        // 推 10 字节再弹出，head 移到 10
        uint8_t dummy[10] = {};
        rb.Push(dummy, 10);
        rb.Pop(dummy, 10);

        // 再推 8 字节（tail 绕到 2），然后 Pop 跨越末端
        uint8_t src[8] = {0xB0,0xB1,0xB2,0xB3,0xB4,0xB5,0xB6,0xB7};
        rb.Push(src, 8);

        uint8_t dst[8] = {};
        EXPECT_TRUE(rb.Pop(dst, 8));
        for (int i = 0; i < 8; ++i)
            EXPECT_EQ(dst[i], src[i]) << "mismatch at i=" << i;
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// Peek 跨越缓冲区末端（两段拷贝路径）
TEST(RbUint8EdgeTest, PeekTwoSegmentWrap)
{
    std::string name = MakeRbName("u8_peek_wrap");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);

        // 移动 head 到 12
        uint8_t dummy[12] = {};
        rb.Push(dummy, 12);
        rb.Pop(dummy, 12);

        // 推 6 字节（跨越 tail=12..15 再绕到 0..2）
        uint8_t src[6] = {0xC0,0xC1,0xC2,0xC3,0xC4,0xC5};
        rb.Push(src, 6);

        // Peek 6 字节，head 仍在 12，数据跨尾端
        uint8_t buf[6] = {};
        EXPECT_TRUE(rb.Peek(buf, 6));
        EXPECT_EQ(rb.Size(), 6u);  // Peek 不消费
        for (int i = 0; i < 6; ++i)
            EXPECT_EQ(buf[i], src[i]) << "mismatch at i=" << i;
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}

// PushFront 跨越缓冲区起始端（两段拷贝路径）
// head 靠近 0 时，PushFront count > head，触发两段写
TEST(RbUint8EdgeTest, PushFrontTwoSegmentWrap)
{
    std::string name = MakeRbName("u8_front_wrap");
    {
        ShmRingBuffer<uint8_t> rb(name, 16);

        // 先推 3 字节，使 head=0, tail=3
        uint8_t tail_data[3] = {0xD3, 0xD4, 0xD5};
        rb.Push(tail_data, 3);
        // 此时 head=0，PushFront 时 contiguous_available=0 < count

        // PushFront 4 字节：head=0，空间不足直接在前端，需要绕到末尾
        uint8_t front_data[4] = {0xD0, 0xD1, 0xD2, 0xD3};
        EXPECT_TRUE(rb.PushFront(front_data, 4));
        EXPECT_EQ(rb.Size(), 7u);

        // Pop 全部，验证 PushFront 的数据在前
        uint8_t dst[7] = {};
        rb.Pop(dst, 7);
        // 前 4 个是 front_data，后 3 个是 tail_data
        for (int i = 0; i < 4; ++i)
            EXPECT_EQ(dst[i], front_data[i]) << "front mismatch at i=" << i;
        for (int i = 0; i < 3; ++i)
            EXPECT_EQ(dst[4 + i], tail_data[i]) << "tail mismatch at i=" << i;
    }
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock_").c_str());
}
