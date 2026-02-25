#pragma once
#include "shm_lock.h"
#include <cstdint>
#include <thread>
#include <atomic>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
    #if defined(_MSC_VER)
        #include <intrin.h>
        #define CPU_PAUSE() _mm_pause()
    #else
        #define CPU_PAUSE() __builtin_ia32_pause()
    #endif
#elif defined(__aarch64__) || defined(__arm__)
    #define CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
    #define CPU_PAUSE() std::this_thread::yield()
#endif

/**
 * @brief 基于共享内存的自旋锁
 * 
 * 特点：
 * - 指数退避策略减少 CPU 空转
 * - 支持进程间同步
 * - 适用于锁持有时间很短的场景
 * - 兼容 std::lock_guard 等标准库组件
 * 
 * 注意：
 * - 不适合长时间持有的锁（会浪费 CPU）
 * - 在高竞争场景下性能可能不如普通互斥锁
 */
class ShmSpinLock
{
public:
    /**
     * @brief 构造函数
     * @param name 共享内存名称
     * @param max_spins 最大自旋次数，超过后会 yield（默认 4096）
     */
    explicit ShmSpinLock(const std::string& name, uint32_t max_spins = 4096)
        : lock_(name)
        , max_spins_(max_spins)
    {
    }

    // 禁止拷贝
    ShmSpinLock(const ShmSpinLock&) = delete;
    ShmSpinLock& operator=(const ShmSpinLock&) = delete;

    // 允许移动
    ShmSpinLock(ShmSpinLock&& other) noexcept
        : lock_(std::move(other.lock_))
        , max_spins_(other.max_spins_)
    {
    }

    ShmSpinLock& operator=(ShmSpinLock&& other) noexcept
    {
        if (this != &other)
        {
            lock_ = std::move(other.lock_);
            max_spins_ = other.max_spins_;
        }
        return *this;
    }

    /**
     * @brief 销毁锁并释放共享内存
     * 警告：只应在确保没有其他进程使用此锁时调用
     */
    void Destroy()
    {
        lock_.Destroy();
    }

    /**
     * @brief 尝试加锁（非阻塞）
     * @return true 如果成功获取锁，false 如果锁已被占用
     */
    bool TryLock() noexcept
    {
        return lock_.TryLock();
    }

    /**
     * @brief 加锁（使用指数退避自旋）
     * 
     * 策略：
     * 1. 首先快速尝试获取锁
     * 2. 如果失败，使用指数退避策略自旋
     * 3. 自旋次数逐渐增加（1, 2, 4, 8, ...）
     * 4. 达到最大自旋次数后，让出 CPU 时间片
     */
    void Lock()
    {
        // 快速路径：直接尝试获取锁
        if (TryLock())
        {
            // std::cout << "ShmSpinLock: acquired lock on first try";
            return;
        }

        // 慢速路径：指数退避自旋
        uint32_t spins = 1;
        uint32_t attempts = 0;
        
        while (true)
        {
            // 尝试获取锁
            if (TryLock())
            {
                // std::cout << "ShmSpinLock: acquired lock after " << attempts << " attempts and " << spins << " spins";
                return;
            }

            // 自旋等待
            for (uint32_t i = 0; i < spins; ++i)
            {
                CPU_PAUSE();
            }

            // 指数增长自旋次数，但有上限
            if (spins < max_spins_)
            {
                spins <<= 1; // 翻倍
            }
            else
            {
                // 达到最大自旋次数，让出 CPU
                std::this_thread::yield();
                
                // 每让出若干次后，重置自旋次数
                // 这样可以在锁释放后快速重新获取
                if (++attempts % 10 == 0)
                {
                    spins = 1;
                }
            }
        }
    }

    /**
     * @brief 解锁
     */
    void Unlock()
    {
        lock_.Unlock();
    }

    /**
     * @brief 检查锁是否有效
     */
    bool IsValid() const noexcept
    {
        return lock_.IsValid();
    }

    /**
     * @brief 获取共享内存名称
     */
    const std::string& GetName() const noexcept
    {
        return lock_.GetName();
    }

    /**
     * @brief 设置最大自旋次数
     */
    void SetMaxSpins(uint32_t max_spins) noexcept
    {
        max_spins_ = max_spins;
    }

    /**
     * @brief 获取最大自旋次数
     */
    uint32_t GetMaxSpins() const noexcept
    {
        return max_spins_;
    }

    // 为了兼容 std::lock_guard 等标准库组件
    void lock() { Lock(); }
    void unlock() { Unlock(); }
    bool try_lock() { return TryLock(); }

private:
    ShmLock lock_;          ///< 底层互斥锁
    uint32_t max_spins_;    ///< 最大自旋次数
};

#undef CPU_PAUSE
