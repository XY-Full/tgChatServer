#pragma once
#include "shm_lock.h"
#include <cstddef>
#include <cstring>

struct RingBufferHeader 
{
    size_t head;
    size_t tail;
    size_t capacity;
};

class ShmRingBuffer 
{
public:
    ShmRingBuffer(void* addr, size_t capacity, bool init);
    bool Push(const void* data, size_t len);
    bool Pop(void* data, size_t len);

private:
    RingBufferHeader* m_hdr;
    char* m_buffer;
    size_t m_capacity;
    ShmLock m_lock;
};
