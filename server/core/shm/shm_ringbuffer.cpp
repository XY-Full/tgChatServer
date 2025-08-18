#include "shm_ringbuffer.h"

ShmRingBuffer::ShmRingBuffer(void *addr, size_t capacity, bool init) : m_lock(addr) // 锁占据共享内存起始部分
{
    m_hdr = reinterpret_cast<RingBufferHeader *>((char *)addr + sizeof(pthread_mutex_t));
    m_buffer = reinterpret_cast<char *>(m_hdr + 1);

    if (init)
    {
        m_hdr->head = 0;
        m_hdr->tail = 0;
        m_hdr->capacity = capacity;
    }
    m_capacity = capacity;
}

bool ShmRingBuffer::Push(const void *data, size_t len)
{
    if (len > m_capacity)
        return false;

    m_lock.Lock();
    size_t next_tail = (m_hdr->tail + len) % m_capacity;
    if (next_tail == m_hdr->head)
    {
        m_lock.Unlock();
        return false; // full
    }
    if (m_hdr->tail + len <= m_capacity)
    {
        memcpy(m_buffer + m_hdr->tail, data, len);
    }
    else
    {
        size_t first_part = m_capacity - m_hdr->tail;
        memcpy(m_buffer + m_hdr->tail, data, first_part);
        memcpy(m_buffer, (char *)data + first_part, len - first_part);
    }
    m_hdr->tail = next_tail;
    m_lock.Unlock();
    return true;
}

bool ShmRingBuffer::Pop(void *data, size_t len)
{
    m_lock.Lock();
    if (m_hdr->head == m_hdr->tail)
    {
        m_lock.Unlock();
        return false; // empty
    }
    if (m_hdr->head + len <= m_capacity)
    {
        memcpy(data, m_buffer + m_hdr->head, len);
    }
    else
    {
        size_t first_part = m_capacity - m_hdr->head;
        memcpy(data, m_buffer + m_hdr->head, first_part);
        memcpy((char *)data + first_part, m_buffer, len - first_part);
    }
    m_hdr->head = (m_hdr->head + len) % m_capacity;
    m_lock.Unlock();
    return true;
}
