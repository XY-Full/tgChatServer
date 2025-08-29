#pragma once
#include <pthread.h>
#include <stdexcept>
#include <string>
#include "shm.h"

class ShmLock
{
public:
    ShmLock(std::string name)
    {
        // 如果是新创建的,需要初始化锁
        if (mutex_memory_.Open(name, sizeof(pthread_mutexattr_t)) == SHM_CREATE)
        {
            pthread_mutexattr_t attr;
            if (pthread_mutexattr_init(&attr) != 0)
            {
                throw std::runtime_error("Failed to initialize mutex attributes");
            }
            if (pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) != 0)
            {
                pthread_mutexattr_destroy(&attr);
                throw std::runtime_error("Failed to set process-shared attribute");
            }
            if (pthread_mutex_init(mutex_, &attr) != 0)
            {
                pthread_mutexattr_destroy(&attr);
                throw std::runtime_error("Failed to initialize mutex");
            }
            pthread_mutexattr_destroy(&attr);
        }
    }

    ~ShmLock()
    {
        // 注意: 我们不在这里销毁互斥锁，因为它在共享内存中
    }

    void Lock()
    {
        if (pthread_mutex_lock(mutex_) != 0)
        {
            throw std::runtime_error("Failed to lock mutex");
        }
    }

    void Unlock()
    {
        if (pthread_mutex_unlock(mutex_) != 0)
        {
            throw std::runtime_error("Failed to unlock mutex");
        }
    }

    bool TryLock()
    {
        return pthread_mutex_trylock(mutex_) == 0;
    }

    void Destroy()
    {
        delete mutex_;
        mutex_ = nullptr;
        mutex_memory_.Close(true);
    }

private:
    pthread_mutex_t *mutex_;
    SharedMemory    mutex_memory_;
};