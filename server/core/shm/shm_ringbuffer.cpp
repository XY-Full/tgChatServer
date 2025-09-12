#include "shm_ringbuffer.h"
#include "GlobalSpace.h"
#include "network/MsgWrapper.h"
#include "shm.h"
#include <cstring>
#include <mutex>

class AppMsgWrapper;
template class ShmRingBuffer<AppMsgWrapper>;
template class ShmRingBuffer<unsigned char>;

template <typename T>
ShmRingBuffer<T>::ShmRingBuffer(const std::string &shm_name, size_t ring_size)
    : shm_name_(shm_name), ring_size_(ring_size), is_owner_(true)
{
    // 计算所需共享内存大小
    shm_size_ = CalculateShmSize();

    // 创建共享内存
    auto result = shm_manager_.Open(shm_name_, shm_size_, true);
    if (result == SHM_ERROR)
    {
        throw std::runtime_error("Failed to create shared memory");
    }

    shm_addr_ = shm_manager_.GetAddress();

    bool needInit = result == SHM_CREATE ? true : false;
    InitShm(needInit);
}

template <typename T> ShmRingBuffer<T>::~ShmRingBuffer()
{
    if (shm_addr_)
    {
        // 如果是所有者，销毁共享内存
        // if (is_owner_)
        // 共享内存中的数据均不做清除
        {
            shm_manager_.Close(false);
        }
    }
}

template <typename T>
ShmRingBuffer<T>::ShmRingBuffer(ShmRingBuffer &&other) noexcept
    : shm_name_(std::move(other.shm_name_)), ring_size_(other.ring_size_), shm_addr_(other.shm_addr_),
      shm_size_(other.shm_size_), mutex_(other.mutex_), header_(other.header_), buffer_(other.buffer_),
      lock_(std::move(other.lock_)), is_owner_(other.is_owner_)
{
    other.shm_addr_ = nullptr;
    other.mutex_ = nullptr;
    other.header_ = nullptr;
    other.buffer_ = nullptr;
    other.is_owner_ = false;
}

template <typename T> ShmRingBuffer<T> &ShmRingBuffer<T>::operator=(ShmRingBuffer &&other) noexcept
{
    if (this != &other)
    {
        // 如果是owner则主动销毁，否则等待析构自动释放资源
        if (shm_addr_ && is_owner_)
        {
            shm_manager_.Close(true);
        }

        // 转移资源
        shm_name_ = std::move(other.shm_name_);
        ring_size_ = other.ring_size_;
        shm_addr_ = other.shm_addr_;
        shm_size_ = other.shm_size_;
        mutex_ = other.mutex_;
        header_ = other.header_;
        buffer_ = other.buffer_;
        lock_ = std::move(other.lock_);
        is_owner_ = other.is_owner_;

        // 置空源对象
        other.shm_addr_ = nullptr;
        other.mutex_ = nullptr;
        other.header_ = nullptr;
        other.buffer_ = nullptr;
        other.is_owner_ = false;
    }
    return *this;
}

template <typename T> size_t ShmRingBuffer<T>::CalculateShmSize() const
{
    return sizeof(pthread_mutex_t) + sizeof(RingBufferHeader) + ring_size_ * sizeof(T);
}

template <typename T> void ShmRingBuffer<T>::InitShm(bool init_header)
{
    // 设置指针
    mutex_ = static_cast<pthread_mutex_t *>(shm_addr_);
    header_ = reinterpret_cast<RingBufferHeader *>(static_cast<char *>(shm_addr_) + sizeof(pthread_mutex_t));
    buffer_ = reinterpret_cast<T *>(header_ + 1);

    // 初始化锁
    lock_ = std::make_unique<ShmSpinLock>(shm_name_ + "_lock_");

    // 如果需要，初始化头部
    if (init_header)
    {
        header_->head = 0;
        header_->tail = 0;
        header_->capacity = ring_size_;
        header_->element_size = sizeof(T);
        header_->initialized = true;
    }
    else
    {
        // 验证头部
        if (!header_->initialized || header_->capacity != ring_size_ || header_->element_size != sizeof(T))
        {
            throw std::runtime_error("Invalid ring buffer header");
        }
    }
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

template <typename T> bool ShmRingBuffer<T>::Push(const T *items, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (!Push(items[i]))
        {
            return false;
        }
    }
    return true;
}

template <typename T> bool ShmRingBuffer<T>::Push(const T &item)
{
    if (IsFull())
    {
        return false;
    }

    // 复制数据到缓冲区
    buffer_[header_->tail] = item;
    header_->tail = (header_->tail + 1) % ring_size_;

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

template <typename T> bool ShmRingBuffer<T>::Pop(T *items, size_t count)
{
    for (size_t i = 0; i < count; ++i)
    {
        if (!Pop(items[i]))
        {
            return false;
        }
    }
    return true;
}

template <typename T> bool ShmRingBuffer<T>::Pop(T &item)
{
    std::lock_guard<ShmSpinLock> guard(*lock_);

    if (IsEmpty())
    {
        return false;
    }

    // 从缓冲区复制数据
    item = buffer_[header_->head];
    header_->head = (header_->head + 1) % ring_size_;

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

template <typename T> bool ShmRingBuffer<T>::TryPush(const T &item)
{
    if (!lock_->TryLock())
    {
        return false;
    }

    if (IsFull())
    {
        lock_->Unlock();
        return false;
    }

    // 复制数据到缓冲区
    buffer_[header_->tail] = item;
    header_->tail = (header_->tail + 1) % ring_size_;

    lock_->Unlock();
    return true;
}

template <typename T> bool ShmRingBuffer<T>::TryPop(T &item)
{
    if (!lock_->TryLock())
    {
        return false;
    }

    if (IsEmpty())
    {
        lock_->Unlock();
        return false;
    }

    // 从缓冲区复制数据
    item = buffer_[header_->head];
    header_->head = (header_->head + 1) % ring_size_;

    lock_->Unlock();
    return true;
}

template <typename T> bool ShmRingBuffer<T>::IsEmpty() const
{
    return header_->head == header_->tail;
}

template <typename T> bool ShmRingBuffer<T>::IsFull() const
{
    return (header_->tail + 1) % ring_size_ == header_->head;
}

template <typename T> size_t ShmRingBuffer<T>::Size() const
{
    return Used();
}

template <typename T> size_t ShmRingBuffer<T>::Capacity() const
{
    return ring_size_;
}

template <typename T> const std::string &ShmRingBuffer<T>::GetShmName() const
{
    return shm_name_;
}

template <typename T> void ShmRingBuffer<T>::Reset()
{
    std::lock_guard<ShmSpinLock> guard(*lock_);
    header_->head = 0;
    header_->tail = 0;
}

template <typename T> size_t ShmRingBuffer<T>::Available() const
{
    if (header_->tail >= header_->head)
    {
        return ring_size_ - (header_->tail - header_->head) - 1;
    }
    else
    {
        return header_->head - header_->tail - 1;
    }
}

template <typename T> size_t ShmRingBuffer<T>::Used() const
{
    if (header_->tail >= header_->head)
    {
        return header_->tail - header_->head;
    }
    else
    {
        return ring_size_ - (header_->head - header_->tail);
    }
}