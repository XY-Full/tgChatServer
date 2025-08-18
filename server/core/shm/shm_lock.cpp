#include "shm_lock.h"
#include <stdexcept>

ShmLock::ShmLock(void* addr)
{
#if defined(_WIN32) || defined(_WIN64)
    // Windows 互斥锁直接基于命名 mutex 创建
    m_mutex = CreateMutexA(NULL, FALSE, (LPCSTR)addr); 
    if (!m_mutex) throw std::runtime_error("CreateMutex failed");
#else
    m_mutex = reinterpret_cast<pthread_mutex_t*>(addr);

    // 初始化互斥锁（进程共享）
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(m_mutex, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}

ShmLock::~ShmLock()
{
#if defined(_WIN32) || defined(_WIN64)
    if (m_mutex) CloseHandle(m_mutex);
#else
    if (m_mutex) pthread_mutex_destroy(m_mutex);
#endif
}

void ShmLock::Lock()
{
#if defined(_WIN32) || defined(_WIN64)
    WaitForSingleObject(m_mutex, INFINITE);
#else
    pthread_mutex_lock(m_mutex);
#endif
}

void ShmLock::Unlock()
{
#if defined(_WIN32) || defined(_WIN64)
    ReleaseMutex(m_mutex);
#else
    pthread_mutex_unlock(m_mutex);
#endif
}
