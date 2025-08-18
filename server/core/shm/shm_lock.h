#pragma once
#include "shm.h"
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <pthread.h>
#endif

class ShmLock 
{
public:
    ShmLock(void* addr);
    ~ShmLock();

    void Lock();
    void Unlock();

private:
#if defined(_WIN32) || defined(_WIN64)
    HANDLE m_mutex;
#else
    pthread_mutex_t* m_mutex;
#endif
};
