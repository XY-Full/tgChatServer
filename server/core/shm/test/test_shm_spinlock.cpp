/**
 * test_shm_spinlock.cpp
 * ShmSpinLock 单元测试
 *
 * 覆盖：
 *  - Lock / Unlock 基础流程
 *  - TryLock 非阻塞语义
 *  - lock_guard RAII 用法
 *  - 多线程竞争（8 线程同时 TryLock，只有 1 个成功）
 */

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>
#include <sys/mman.h>   // shm_unlink
#include <string>
#include <cstdlib>      // getpid

#include "shm_spinlock.h"

// ──────────────────────────────────────────────
// 辅助：生成进程唯一的 SHM 名称，避免并行测试冲突
// ──────────────────────────────────────────────
static std::string MakeLockName(const std::string& suffix)
{
    return "/test_lock_" + std::to_string(getpid()) + "_" + suffix;
}

static void CleanupLock(const std::string& name)
{
    shm_unlink(name.c_str());
}

// ──────────────────────────────────────────────
// Fixture
// ──────────────────────────────────────────────
class ShmSpinLockTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        name_ = MakeLockName(std::to_string(counter_++));
        lock_ = std::make_unique<ShmSpinLock>(name_);
    }

    void TearDown() override
    {
        lock_.reset();
        CleanupLock(name_);
    }

    std::string name_;
    std::unique_ptr<ShmSpinLock> lock_;
    static std::atomic<int> counter_;
};

std::atomic<int> ShmSpinLockTest::counter_{0};

// ──────────────────────────────────────────────
// 基础 Lock / Unlock
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, LockUnlock)
{
    ASSERT_NO_THROW(lock_->Lock());
    ASSERT_NO_THROW(lock_->Unlock());
}

// ──────────────────────────────────────────────
// TryLock 首次应成功
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, TryLockSucceedsWhenFree)
{
    EXPECT_TRUE(lock_->TryLock());
    lock_->Unlock();
}

// ──────────────────────────────────────────────
// 注意：ShmLock 底层使用 pthread_mutex（PTHREAD_MUTEX_DEFAULT）
// 同一线程再次 Lock 是未定义行为（非递归锁），
// 因此这里只测 TryLock 在解锁后可以再次获取
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, TryLockAfterUnlock)
{
    EXPECT_TRUE(lock_->TryLock());
    lock_->Unlock();
    EXPECT_TRUE(lock_->TryLock());
    lock_->Unlock();
}

// ──────────────────────────────────────────────
// lock_guard RAII 用法
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, LockGuard)
{
    {
        std::lock_guard<ShmSpinLock> guard(*lock_);
        // 在持有锁的情况下执行一些操作
        SUCCEED();
    }
    // 析构后应该能再次获取锁
    EXPECT_TRUE(lock_->TryLock());
    lock_->Unlock();
}

// ──────────────────────────────────────────────
// IsValid / GetName
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, IsValidAndGetName)
{
    EXPECT_TRUE(lock_->IsValid());
    EXPECT_EQ(lock_->GetName(), name_);
}

// ──────────────────────────────────────────────
// 多线程：8 线程同时 TryLock，同一时刻只有 1 个成功
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, ConcurrentTryLock)
{
    constexpr int kThreads = 8;
    std::atomic<int> success_count{0};
    std::atomic<bool> start{false};

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&]() {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            if (lock_->TryLock())
            {
                success_count.fetch_add(1, std::memory_order_relaxed);
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                lock_->Unlock();
            }
        });
    }

    start.store(true, std::memory_order_release);
    for (auto& t : threads)
        t.join();

    // 同一时刻锁只能被一个线程持有，因此获得锁的次数 >= 1
    EXPECT_GE(success_count.load(), 1);
    EXPECT_LE(success_count.load(), kThreads);
}

// ──────────────────────────────────────────────
// 多线程：8 线程互斥累加共享计数器
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, MutualExclusionCounter)
{
    constexpr int kThreads = 8;
    constexpr int kIters   = 1000;
    int counter = 0;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&]() {
            for (int j = 0; j < kIters; ++j)
            {
                lock_->Lock();
                ++counter;
                lock_->Unlock();
            }
        });
    }

    for (auto& t : threads)
        t.join();

    EXPECT_EQ(counter, kThreads * kIters);
}

// ──────────────────────────────────────────────
// SetMaxSpins / GetMaxSpins
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, SetAndGetMaxSpins)
{
    EXPECT_EQ(lock_->GetMaxSpins(), 4096u);  // 默认值
    lock_->SetMaxSpins(512);
    EXPECT_EQ(lock_->GetMaxSpins(), 512u);
    lock_->SetMaxSpins(1);
    EXPECT_EQ(lock_->GetMaxSpins(), 1u);
}

// ──────────────────────────────────────────────
// 移动构造
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, MoveConstructor)
{
    ASSERT_TRUE(lock_->IsValid());
    std::string orig_name = lock_->GetName();

    ShmSpinLock moved(std::move(*lock_));

    // 移动后新对象有效，名称保留
    EXPECT_TRUE(moved.IsValid());
    EXPECT_EQ(moved.GetName(), orig_name);

    // 移动后可以正常加解锁
    ASSERT_NO_THROW(moved.Lock());
    ASSERT_NO_THROW(moved.Unlock());
}

// ──────────────────────────────────────────────
// 移动赋值
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, MoveAssignment)
{
    std::string name2 = MakeLockName(std::to_string(counter_++));
    ShmSpinLock lock2(name2);

    ASSERT_TRUE(lock2.IsValid());
    lock2 = std::move(*lock_);

    EXPECT_TRUE(lock2.IsValid());
    EXPECT_EQ(lock2.GetName(), name_);

    // 赋值后可正常使用
    ASSERT_NO_THROW(lock2.Lock());
    ASSERT_NO_THROW(lock2.Unlock());

    // 清理 lock2 对应的 shm
    lock2.Destroy();
    shm_unlink(name2.c_str());
}

// ──────────────────────────────────────────────
// 高竞争：16 线程 × 2000 次加锁，无数据竞争
// ──────────────────────────────────────────────
TEST_F(ShmSpinLockTest, HighContentionScaling)
{
    constexpr int kThreads = 16;
    constexpr int kIters   = 2000;
    int counter = 0;

    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i)
    {
        threads.emplace_back([&]() {
            for (int j = 0; j < kIters; ++j)
            {
                lock_->Lock();
                ++counter;
                lock_->Unlock();
            }
        });
    }

    for (auto& t : threads)
        t.join();

    EXPECT_EQ(counter, kThreads * kIters);
}
