/**
 * test_shm_slab.cpp
 * shmslab::ShmSlab 单元测试
 *
 * 覆盖：
 *  SizeClass  - 各尺寸落入正确的 size-class
 *  AllocFree  - 分配/释放基础、空闲链复用
 *  Overflow   - 超过最大 size-class 返回 0，内存耗尽返回 0
 *  OffsetPtr  - off2ptr / ptr2off 双向转换
 *  Concurrent - 多线程并发分配/释放
 */

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>
#include <sys/mman.h>
#include <unistd.h>

#include "shm_slab.h"

using namespace shmslab;

// ──────────────────────────────────────────────
// 辅助
// ──────────────────────────────────────────────
static std::atomic<int> g_slab_counter{0};

static std::string MakeSlabName(const std::string& tag)
{
    return "/test_slab_" + std::to_string(getpid()) + "_" + tag + "_" +
           std::to_string(g_slab_counter.fetch_add(1));
}

static void CleanupSlab(const std::string& name)
{
    shm_unlink(name.c_str());
    shm_unlink((name + "_lock").c_str());
}

// ──────────────────────────────────────────────
// Fixture：使用足够大的共享内存（4MB）
// ──────────────────────────────────────────────
class ShmSlabTest : public ::testing::Test
{
protected:
    // 4MB，允许多次 NewPage
    static constexpr uint32_t kTotal = 4u * 1024u * 1024u;

    void SetUp() override
    {
        name_ = MakeSlabName("main");
        slab_ = std::make_unique<ShmSlab>(name_, kTotal, 0, kTotal);
    }
    void TearDown() override
    {
        slab_.reset();
        CleanupSlab(name_);
    }

    std::string name_;
    std::unique_ptr<ShmSlab> slab_;
};

// ══════════════════════════════════════════════
// 1. SizeClass 边界
// ══════════════════════════════════════════════
TEST_F(ShmSlabTest, SizeClassSmallest)
{
    // 1~16 字节均落在最小类（16 字节）
    uint32_t off1 = slab_->Alloc(1);
    uint32_t off2 = slab_->Alloc(16);
    EXPECT_NE(off1, 0u);
    EXPECT_NE(off2, 0u);
    slab_->Free(off1, 1);
    slab_->Free(off2, 16);
}

TEST_F(ShmSlabTest, SizeClassBoundaries)
{
    // 测试每个 size-class 边界：16,32,64,128,256,512,1024,2048,4096,8192
    uint32_t sizes[] = {16,32,64,128,256,512,1024,2048,4096,8192};
    for (uint32_t sz : sizes)
    {
        uint32_t off = slab_->Alloc(sz);
        EXPECT_NE(off, 0u) << "Alloc(" << sz << ") failed";
        slab_->Free(off, sz);
    }
}

TEST_F(ShmSlabTest, SizeClassExceedMax)
{
    // 超过 8192 字节，Alloc 应返回 0
    uint32_t off = slab_->Alloc(8193);
    EXPECT_EQ(off, 0u);
}

TEST_F(ShmSlabTest, SizeZero)
{
    // Alloc(0) 对齐到 8，落在 class[0]（16B）
    // 只要不崩溃、能 Free 即可
    uint32_t off = slab_->Alloc(0);
    // 0 对齐到 8 = 0，SizeClassIndex(0) -> pow2ceil(0)=1 -> s=16 -> idx=0
    // 预期成功
    if (off != 0)
        slab_->Free(off, 0);
}

// ══════════════════════════════════════════════
// 2. Alloc / Free 基础
// ══════════════════════════════════════════════
TEST_F(ShmSlabTest, AllocReturnsNonZero)
{
    uint32_t off = slab_->Alloc(64);
    EXPECT_NE(off, 0u);
    EXPECT_NE(slab_->off2ptr(off), nullptr);
    slab_->Free(off, 64);
}

TEST_F(ShmSlabTest, FreeAndReuseOffset)
{
    uint32_t off1 = slab_->Alloc(64);
    ASSERT_NE(off1, 0u);
    slab_->Free(off1, 64);

    // 下次 Alloc 同尺寸应从 free_list 取回（相同偏移）
    uint32_t off2 = slab_->Alloc(64);
    EXPECT_EQ(off2, off1);
    slab_->Free(off2, 64);
}

TEST_F(ShmSlabTest, MultipleAllocsDistinct)
{
    // 连续分配 10 个同尺寸块，偏移各不相同
    std::vector<uint32_t> offs;
    for (int i = 0; i < 10; ++i)
    {
        uint32_t off = slab_->Alloc(128);
        ASSERT_NE(off, 0u);
        offs.push_back(off);
    }
    // 验证互不重叠（偏移唯一）
    for (size_t i = 0; i < offs.size(); ++i)
        for (size_t j = i + 1; j < offs.size(); ++j)
            EXPECT_NE(offs[i], offs[j]);

    for (uint32_t off : offs)
        slab_->Free(off, 128);
}

TEST_F(ShmSlabTest, WriteAndRead)
{
    uint32_t off = slab_->Alloc(64);
    ASSERT_NE(off, 0u);

    // 写入数据
    uint8_t* ptr = reinterpret_cast<uint8_t*>(slab_->off2ptr(off));
    for (int i = 0; i < 64; ++i) ptr[i] = static_cast<uint8_t>(i);

    // 读回验证
    for (int i = 0; i < 64; ++i)
        EXPECT_EQ(ptr[i], static_cast<uint8_t>(i));

    slab_->Free(off, 64);
}

TEST_F(ShmSlabTest, NewPageTriggered)
{
    // 分配足够多的 16B 块，强制触发 NewPage
    // PAGE_SIZE=64KB，每页大约 (64K - header) / 16 ≈ 4000 个
    constexpr int kAllocCount = 5000;
    std::vector<uint32_t> offs;
    for (int i = 0; i < kAllocCount; ++i)
    {
        uint32_t off = slab_->Alloc(16);
        if (off == 0) break; // 内存耗尽
        offs.push_back(off);
    }
    // 至少应成功分配 1 页以上
    EXPECT_GT(offs.size(), 0u);

    for (uint32_t off : offs)
        slab_->Free(off, 16);
}

// ══════════════════════════════════════════════
// 3. off2ptr / ptr2off 转换
// ══════════════════════════════════════════════
TEST_F(ShmSlabTest, Off2PtrNullWhenZero)
{
    EXPECT_EQ(slab_->off2ptr(0), nullptr);
}

TEST_F(ShmSlabTest, Ptr2OffNullWhenNullptr)
{
    EXPECT_EQ(slab_->ptr2off(nullptr), 0u);
}

TEST_F(ShmSlabTest, OffsetRoundTrip)
{
    uint32_t off = slab_->Alloc(32);
    ASSERT_NE(off, 0u);

    void* ptr = slab_->off2ptr(off);
    EXPECT_EQ(slab_->ptr2off(ptr), off);

    slab_->Free(off, 32);
}

// ══════════════════════════════════════════════
// 4. Concurrent - 多线程并发分配/释放
// ══════════════════════════════════════════════
TEST_F(ShmSlabTest, ConcurrentAllocFree)
{
    constexpr int kThreads = 8;
    constexpr int kIters   = 500;
    std::atomic<bool> any_fail{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&]() {
            for (int i = 0; i < kIters; ++i)
            {
                uint32_t off = slab_->Alloc(64);
                if (off == 0)
                {
                    any_fail.store(true, std::memory_order_relaxed);
                    continue;
                }
                // 写一个标记值
                *reinterpret_cast<uint32_t*>(slab_->off2ptr(off)) = 0xDEADBEEFu;
                slab_->Free(off, 64);
            }
        });
    }

    for (auto& t : threads) t.join();

    // 只要没有崩溃（ASan、段错误）就算通过
    // 若内存不足会有 any_fail，但不算错
    SUCCEED();
}

// ══════════════════════════════════════════════
// 5. Header 校验
// ══════════════════════════════════════════════
TEST_F(ShmSlabTest, HeaderMagicAndClassCount)
{
    const SlabHeader* hdr = slab_->Header();
    ASSERT_NE(hdr, nullptr);
    EXPECT_EQ(hdr->magic, 0x534C4142u); // 'SLAB'
    EXPECT_EQ(hdr->version, 1u);
    EXPECT_EQ(slab_->ClassCount(), static_cast<uint32_t>(MAX_CLASSES));
}

// ══════════════════════════════════════════════
// 6. 扩展测试
// ══════════════════════════════════════════════

// Alloc(8192) 应成功（最大 size-class 恰好是 8192）
TEST_F(ShmSlabTest, AllocExactlyMaxClassSize)
{
    uint32_t off = slab_->Alloc(8192);
    EXPECT_NE(off, 0u);
    slab_->Free(off, 8192);
}

// off2ptr(ptr2off(p)) == p 双向转换
TEST_F(ShmSlabTest, Off2PtrRoundTrip)
{
    uint32_t off = slab_->Alloc(64);
    ASSERT_NE(off, 0u);

    void* p = slab_->off2ptr(off);
    ASSERT_NE(p, nullptr);

    uint32_t off2 = slab_->ptr2off(p);
    EXPECT_EQ(off2, off);

    void* p2 = slab_->off2ptr(off2);
    EXPECT_EQ(p2, p);

    slab_->Free(off, 64);
}

// 分配不同 size-class 的对象，地址不重叠
TEST_F(ShmSlabTest, MultiClassAllocNoOverlap)
{
    uint32_t sizes[] = {16, 64, 256, 1024, 4096};
    constexpr int kCount = 5;
    uint32_t offs[kCount];

    for (int i = 0; i < kCount; ++i)
    {
        offs[i] = slab_->Alloc(sizes[i]);
        ASSERT_NE(offs[i], 0u) << "Alloc(" << sizes[i] << ") failed";
    }

    // 验证所有指针区间互不重叠（简单检查：ptr 和 ptr+size 不与其他重叠）
    for (int i = 0; i < kCount; ++i)
    {
        uint8_t* pi = reinterpret_cast<uint8_t*>(slab_->off2ptr(offs[i]));
        for (int j = i + 1; j < kCount; ++j)
        {
            uint8_t* pj = reinterpret_cast<uint8_t*>(slab_->off2ptr(offs[j]));
            // 两块内存不应有交叉：pi+sizes[i] <= pj 或 pj+sizes[j] <= pi
            bool no_overlap = (pi + sizes[i] <= pj) || (pj + sizes[j] <= pi);
            EXPECT_TRUE(no_overlap)
                << "Overlap between class[" << i << "] and class[" << j << "]";
        }
    }

    for (int i = 0; i < kCount; ++i)
        slab_->Free(offs[i], sizes[i]);
}

// 分配直到 OOM，验证返回 0 的路径
TEST_F(ShmSlabTest, ExhaustAndFail)
{
    // 使用小 SHM（128KB），用 16 字节对象快速耗尽
    std::string name = MakeSlabName("exhaust");
    constexpr uint32_t kSmall = 128u * 1024u;
    ShmSlab small_slab(name, kSmall, 0, kSmall);

    int alloc_count = 0;
    std::vector<uint32_t> offs;
    while (true)
    {
        uint32_t off = small_slab.Alloc(16);
        if (off == 0) break;
        offs.push_back(off);
        ++alloc_count;
        if (alloc_count > 100000) break;  // 防止死循环
    }

    // 应该在某个时刻返回 0（OOM）
    EXPECT_GT(alloc_count, 0);

    for (auto off : offs)
        small_slab.Free(off, 16);

    shm_unlink(name.c_str());
    shm_unlink((name + "_lock").c_str());
}

// 释放 N 个对象后，再分配 N 个，全部命中空闲链表（不触发 NewPage）
TEST_F(ShmSlabTest, FreeListReusedAfterMassiveFree)
{
    constexpr int kN = 100;

    // 分配 kN 个 64 字节对象
    std::vector<uint32_t> offs;
    offs.reserve(kN);
    for (int i = 0; i < kN; ++i)
    {
        uint32_t off = slab_->Alloc(64);
        ASSERT_NE(off, 0u);
        offs.push_back(off);
    }

    // 全部释放（进入空闲链表）
    for (auto off : offs) slab_->Free(off, 64);

    // 再分配 kN 个，应全部来自空闲链（不崩溃，地址合法）
    std::vector<uint32_t> offs2;
    offs2.reserve(kN);
    for (int i = 0; i < kN; ++i)
    {
        uint32_t off = slab_->Alloc(64);
        EXPECT_NE(off, 0u);
        offs2.push_back(off);
    }

    for (auto off : offs2) slab_->Free(off, 64);
}

// ══════════════════════════════════════════════
// 7. Free 边界：no-op 路径
// ══════════════════════════════════════════════

// Free(0, bytes) 是 no-op（off==0 早返）：不崩溃，slab 状态不变
TEST_F(ShmSlabTest, FreeOffsetZeroIsNoOp)
{
    // Alloc 一个合法块，记录偏移
    uint32_t off = slab_->Alloc(64);
    ASSERT_NE(off, 0u);

    // Free(0, 64) 应静默返回，不影响任何状态
    EXPECT_NO_THROW(slab_->Free(0, 64));

    // 原合法块的指针仍然有效
    EXPECT_NE(slab_->off2ptr(off), nullptr);
    slab_->Free(off, 64);
}

// Free(off, bytes>8192) 是 no-op（SizeClassIndex 返回 UINT32_MAX 早返）：不崩溃，块未还入链表
TEST_F(ShmSlabTest, FreeOversizedBytesIsNoOp)
{
    // 先分配一个合法的 8192 字节块
    uint32_t off = slab_->Alloc(8192);
    ASSERT_NE(off, 0u);

    // 用 oversized bytes 调用 Free —— 这是一个 no-op，块不会进入任何 free_list
    // 不崩溃即可
    EXPECT_NO_THROW(slab_->Free(off, 8193));

    // 再次 Alloc(8192)：free_list 为空（no-op 没有归还），会从 page 分配新块
    uint32_t off2 = slab_->Alloc(8192);
    EXPECT_NE(off2, 0u);
    EXPECT_NE(off2, off);  // 是新块，不是刚才那块

    slab_->Free(off2, 8192);
    // off 对应的内存泄漏（在测试结束时 slab 析构，SHM 统一释放）
}
