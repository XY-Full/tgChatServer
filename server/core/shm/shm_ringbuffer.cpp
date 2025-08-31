#include "shm_ringbuffer.h"
#include "shm.h"
#include <mutex>

template <typename T>
ShmRingBuffer<T>::ShmRingBuffer(const std::string &shm_name, size_t ring_size, bool exclusive)
    : shm_name_(shm_name), ring_size_(ring_size), is_owner_(true)
{
    // 计算所需共享内存大小
    shm_size_ = CalculateShmSize();

    // 创建共享内存
    auto result = shm_manager_.Open(shm_name_, shm_size_, true);
    if(result == SHM_ERROR)
    {
        throw std::runtime_error("Failed to create shared memory");
    }

    shm_addr_ = shm_manager_.GetAddress();

    // 初始化共享内存
    InitShm(true);
}

template <typename T>
ShmRingBuffer<T>::ShmRingBuffer(const std::string &shm_name, size_t ring_size, std::nullptr_t)
    : shm_name_(shm_name), ring_size_(ring_size), is_owner_(false)
{
    // 计算所需共享内存大小
    shm_size_ = CalculateShmSize();

    // 打开现有共享内存
    shm_addr_ = shm_manager_.Open(shm_name_, shm_size_, true);

    // 初始化共享内存（不初始化头部）
    InitShm(false);
}

template <typename T> ShmRingBuffer<T>::~ShmRingBuffer()
{
    if (shm_addr_)
    {
        // 如果是所有者，销毁共享内存
        if (is_owner_)
        {
            shm_manager_.Close(true);
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
    lock_ = std::make_unique<ShmSpinLock>();
    lock_->InitUnlocked(shm_name_ + "_lock_");

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