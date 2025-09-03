#pragma once
#include <string>
#include "shm_spinlock.h"
#include <memory>
#include "shm.h"

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