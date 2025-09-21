#include "shm_ringbuffer.h"

// 针对字节类型的优化实现
template <> bool ShmRingBuffer<uint8_t>::Peek(uint8_t *items, size_t count)
{
    if (count == 0)
        return true;

    std::lock_guard<ShmSpinLock> guard(*lock_);

    if (IsEmpty() || Available() < count)
    {
        return false;
    }

    // 计算连续可读的字节数
    size_t contiguous_available = ring_size_ - header_->head;
    if (contiguous_available >= count)
    {
        // 一次性拷贝连续的数据
        memcpy(items, buffer_ + header_->head, count);
    }
    else
    {
        // 分两段拷贝：从尾部到结束，然后从开始到剩余部分
        memcpy(items, buffer_ + header_->head, contiguous_available);
        memcpy(items + contiguous_available, buffer_, count - contiguous_available);
    }
    return true;
}

template <> bool ShmRingBuffer<uint8_t>::PushFront(const uint8_t *items, size_t count)
{
    if (count == 0)
        return true;
    if (!items)
        return false;

    std::lock_guard<ShmSpinLock> guard(*lock_);

    // 检查是否有足够空间
    if (Available() < count)
    {
        return false;
    }

    // 计算头部前面的连续可用空间
    size_t contiguous_available = header_->head;

    if (likely(contiguous_available >= count))
    {
        // 一次性拷贝连续的数据到头部前面
        memcpy(buffer_ + header_->head - count, items, count);
        header_->head = (header_->head - count + ring_size_) % ring_size_;
    }
    else
    {
        // 分两段拷贝：从缓冲区末尾向前，然后从末尾到头部前面
        size_t first_part = count - contiguous_available;
        memcpy(buffer_ + ring_size_ - first_part, items, first_part);
        memcpy(buffer_, items + first_part, contiguous_available);
        header_->head = ring_size_ - first_part;
    }

    return true;
}

// 针对字节类型的优化实现
template <> bool ShmRingBuffer<uint8_t>::Pop(uint8_t *items, size_t count)
{
    if (count == 0)
        return true;

    std::lock_guard<ShmSpinLock> guard(*lock_);

    if (IsEmpty() || Available() < count)
    {
        return false;
    }

    // 计算连续可读的字节数
    size_t contiguous_available = ring_size_ - header_->head;
    if (likely(contiguous_available >= count))
    {
        // 一次性拷贝连续的数据
        memcpy(items, buffer_ + header_->head, count);
        header_->head += count;
    }
    else
    {
        // 分两段拷贝：从尾部到结束，然后从开始到剩余部分
        memcpy(items, buffer_ + header_->head, contiguous_available);
        memcpy(items + contiguous_available, buffer_, count - contiguous_available);
        header_->head = count - contiguous_available;
    }

    return true;
}

// 针对字节类型的优化实现
template <> bool ShmRingBuffer<uint8_t>::Push(const uint8_t *items, size_t count)
{
    if (count == 0)
        return true;

    std::lock_guard<ShmSpinLock> guard(*lock_);

    // 检查是否有足够空间
    if (Available() < count) // 需要实现 Available() 方法
    {
        return false;
    }

    // 计算连续可用空间（从tail到缓冲区末尾）
    size_t contiguous_available = ring_size_ - header_->tail;

    if (likely(contiguous_available >= count))
    {
        // 一次性拷贝连续的数据
        memcpy(buffer_ + header_->tail, items, count);
        header_->tail = (header_->tail + count) % ring_size_;
    }
    else
    {
        // 分两段拷贝：从尾部到结束，然后从开始到剩余部分
        memcpy(buffer_ + header_->tail, items, contiguous_available);
        memcpy(buffer_, items + contiguous_available, count - contiguous_available);
        header_->tail = count - contiguous_available;
    }

    return true;
}
