#pragma once
#include "shm_lock.h"
#include <cstdint>

struct ShmSpinLock
{
    ShmLock lock_;

    void InitUnlocked(std::string name)
    {
        lock_ = ShmLock(name);
    }

    void Destroy()
    {
        lock_.Destroy();
    }

    bool TryLock()
    {
        return lock_.TryLock();
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
        lock_.Unlock();
    }

    // 为了兼容lock_guard
    void lock()
    {
        Lock();
    }

    void unlock()
    {
        Unlock();
    }
};
