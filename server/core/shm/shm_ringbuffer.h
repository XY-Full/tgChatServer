#pragma once
#include <string>
#include "shm_spinlock.h"
#include <memory>
#include "shm.h"
#include "GlobalSpace.h"
#include "shm.h"
#include <cstring>
#include <mutex>

// 共享内存环形缓冲区头结构
struct RingBufferHeader {
    size_t head;
    size_t tail;
    size_t capacity;
    size_t element_size;
    bool initialized;
};

template<typename T>
class ShmRingBuffer {
public:
    // 空构造函数
    ShmRingBuffer() {}

    // 构造函数 - 创建新的环形缓冲区
    ShmRingBuffer(const std::string& shm_name, size_t ring_size = 1 << 20);
    
    // 析构函数
    ~ShmRingBuffer();
    
    // 禁止拷贝和赋值
    ShmRingBuffer(const ShmRingBuffer&) = delete;
    ShmRingBuffer& operator=(const ShmRingBuffer&) = delete;
    
    // 移动构造函数
    ShmRingBuffer(ShmRingBuffer&& other) noexcept;
    
    // 移动赋值运算符
    ShmRingBuffer& operator=(ShmRingBuffer&& other) noexcept;

    // 批量写数据到缓冲区
    bool Push(const T* items, size_t count);
    
    // 推送消息到缓冲区
    bool Push(const T& item);

    // 批量从缓冲区弹出消息
    bool Pop(T* items, size_t count);
    
    // 从缓冲区弹出消息
    bool Pop(T& item);

    // 从缓冲区丢弃消息
    bool Drop(size_t count);

    // 推送消息到头部
    bool PushFront(const T* item, size_t count = 1);

    // 偷看消息
    bool Peek(T* items, size_t count);

    // 尝试推送消息（非阻塞）
    bool TryPush(const T& item);
    
    // 尝试弹出消息（非阻塞）
    bool TryPop(T& item);
    
    // 检查缓冲区是否为空
    bool IsEmpty() const;
    
    // 检查缓冲区是否已满
    bool IsFull() const;
    
    // 获取缓冲区中元素数量
    size_t Size() const;
    
    // 获取缓冲区容量
    size_t Capacity() const;
    
    // 获取共享内存名称
    const std::string& GetShmName() const;
    
    // 重置缓冲区（清空所有数据）
    void Reset();
    
private:
    // 初始化共享内存
    void InitShm(bool init_header);
    
    // 计算可用空间
    size_t Available() const;
    
    // 计算已使用空间
    size_t Used() const;

    // 计算自身需要的共享内存大小
    size_t CalculateShmSize() const;
    
    std::string shm_name_;
    size_t ring_size_;
    void* shm_addr_;
    size_t shm_size_;
    pthread_mutex_t* mutex_;
    RingBufferHeader* header_;
    T* buffer_;
    std::unique_ptr<ShmSpinLock> lock_;
    bool is_owner_;

    SharedMemory shm_manager_;
};

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

    // 无论是新建还是复用已有共享内存，都强制重新初始化 header
    // recv_buffer 是进程内临时缓冲，不需要跨重启持久化，
    // 否则进程重启后会读到上次遗留的 head/tail，导致 Size() 异常
    InitShm(true);
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
template <> bool ShmRingBuffer<uint8_t>::Push(const uint8_t *items, size_t count);

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
    std::lock_guard<ShmSpinLock> guard(*lock_);

    if (IsFull())
    {
        return false;
    }

    // 复制数据到缓冲区
    buffer_[header_->tail] = item;
    header_->tail = (header_->tail + 1) % ring_size_;

    return true;
}

template <typename T> bool ShmRingBuffer<T>::Drop(size_t count)
{
    std::lock_guard<ShmSpinLock> guard(*lock_);

    if (count == 0)
    {
        return false;
    }

    size_t available = Used();
    if (available == 0)
    {
        return false;
    }

    // 截断到实际可用数量，防止 head 越过 tail
    if (count > available)
    {
        count = available;
    }

    // 丢弃数据
    header_->head = (header_->head + count) % ring_size_;

    return true;
}

// 针对字节类型的优化实现
template <> bool ShmRingBuffer<uint8_t>::Pop(uint8_t *items, size_t count);

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

template <> bool ShmRingBuffer<uint8_t>::PushFront(const uint8_t *items, size_t count);

// 针对字节类型的优化实现
template <> bool ShmRingBuffer<uint8_t>::Peek(uint8_t *items, size_t count);

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