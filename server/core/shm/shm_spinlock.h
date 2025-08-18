#pragma once
#include <atomic>
#include <cstdint>
#include <thread>

struct ShmSpinLock
{
    // 0=unlocked, 1=locked
    std::atomic<uint32_t> state;

    void InitUnlocked()
    {
        state.store(0, std::memory_order_relaxed);
    }
    bool TryLock()
    {
        uint32_t expected = 0;
        return state.compare_exchange_strong(expected, 1, std::memory_order_acquire, std::memory_order_relaxed);
    }
    void Lock()
    {
        // 指数退避 + yield
        uint32_t spins = 1;
        for (;;)
        {
            if (TryLock())
                return;
            for (uint32_t i = 0; i < spins; ++i)
            {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386) || defined(_M_IX86)
#if defined(_MSC_VER)
                _mm_pause();
#else
                __builtin_ia32_pause();
#endif
#else
                // 其他平台用 yield
                std::this_thread::yield();
#endif
            }
            if (spins < (1u << 12))
                spins <<= 1;
        }
    }
    void Unlock()
    {
        state.store(0, std::memory_order_release);
    }
};
