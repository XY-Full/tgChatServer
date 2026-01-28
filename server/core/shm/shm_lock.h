#pragma once
#include <cassert>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <cerrno>
#include "shm.h"

/**
 * @brief 基于共享内存的进程间互斥锁
 * 
 * 使用 pthread_mutex_t 实现跨进程的互斥锁，支持:
 * - 多进程安全访问
 * - RAII 资源管理
 * - 标准库兼容（可用于 std::lock_guard 等）
 * - 异常安全
 */
class ShmLock
{
public:
    /**
     * @brief 构造函数 - 创建或附加到共享内存锁
     * @param name 共享内存名称（必须在所有进程间保持一致）
     * @throw std::runtime_error 如果初始化失败
     */
    explicit ShmLock(const std::string& name) : name_(name), mutex_(nullptr), is_owner_(false)
    {
        auto result = mutex_memory_.Open(name, sizeof(pthread_mutex_t), true);
        mutex_ = reinterpret_cast<pthread_mutex_t*>(mutex_memory_.GetAddress());
        
        if (!mutex_)
        {
            throw std::runtime_error("ShmLock: failed to get shared memory address");
        }

        // 如果是新创建的，需要初始化锁
        if (result == SHM_CREATE)
        {
            is_owner_ = true;
            InitMutex();
        }
    }

    /**
     * @brief 析构函数 - 清理资源
     * 
     * 注意：不会销毁 pthread_mutex_t，因为其他进程可能仍在使用
     * 需要显式调用 Destroy() 来完全清理
     */
    ~ShmLock() noexcept
    {
        // 不销毁 mutex，因为它可能被其他进程使用
        // 共享内存会在所有进程退出后由系统清理
    }

    // 禁止拷贝
    ShmLock(const ShmLock&) = delete;
    ShmLock& operator=(const ShmLock&) = delete;

    // 允许移动
    ShmLock(ShmLock&& other) noexcept
        : name_(std::move(other.name_))
        , mutex_(other.mutex_)
        , mutex_memory_(std::move(other.mutex_memory_))
        , is_owner_(other.is_owner_)
    {
        other.mutex_ = nullptr;
        other.is_owner_ = false;
    }

    ShmLock& operator=(ShmLock&& other) noexcept
    {
        if (this != &other)
        {
            name_ = std::move(other.name_);
            mutex_ = other.mutex_;
            mutex_memory_ = std::move(other.mutex_memory_);
            is_owner_ = other.is_owner_;
            
            other.mutex_ = nullptr;
            other.is_owner_ = false;
        }
        return *this;
    }

    /**
     * @brief 加锁（阻塞直到获取锁）
     * @throw std::system_error 如果加锁失败
     */
    void Lock()
    {
        assert(mutex_ != nullptr);
        int ret = pthread_mutex_lock(mutex_);
        if (ret != 0)
        {
            throw std::system_error(ret, std::generic_category(), "ShmLock::Lock failed");
        }
    }

    /**
     * @brief 解锁
     * @throw std::system_error 如果解锁失败
     */
    void Unlock()
    {
        assert(mutex_ != nullptr);
        int ret = pthread_mutex_unlock(mutex_);
        if (ret != 0)
        {
            throw std::system_error(ret, std::generic_category(), "ShmLock::Unlock failed");
        }
    }

    /**
     * @brief 尝试加锁（非阻塞）
     * @return true 如果成功获取锁，false 如果锁已被占用
     */
    bool TryLock() noexcept
    {
        assert(mutex_ != nullptr);
        return pthread_mutex_trylock(mutex_) == 0;
    }

    /**
     * @brief 销毁互斥锁并释放共享内存
     * 
     * 警告：只应在确保没有其他进程使用此锁时调用
     * 通常在程序的主进程清理阶段调用
     */
    void Destroy()
    {
        if (mutex_ && is_owner_)
        {
            // 销毁 pthread_mutex_t
            int ret = pthread_mutex_destroy(mutex_);
            if (ret != 0 && ret != EBUSY)
            {
                // EBUSY 表示锁仍被占用，这是预期的情况
                // 记录警告但不抛出异常
            }
        }
        
        mutex_ = nullptr;
        mutex_memory_.Close(is_owner_); // 只有 owner 删除共享内存文件
    }

    /**
     * @brief 检查锁是否有效
     */
    bool IsValid() const noexcept
    {
        return mutex_ != nullptr;
    }

    /**
     * @brief 获取共享内存名称
     */
    const std::string& GetName() const noexcept
    {
        return name_;
    }

    // 为了兼容 std::lock_guard 等标准库组件
    void lock() { Lock(); }
    void unlock() { Unlock(); }
    bool try_lock() { return TryLock(); }

private:
    /**
     * @brief 初始化 pthread_mutex_t
     */
    void InitMutex()
    {
        pthread_mutexattr_t attr;
        
        int ret = pthread_mutexattr_init(&attr);
        if (ret != 0)
        {
            throw std::system_error(ret, std::generic_category(), 
                "ShmLock: failed to initialize mutex attributes");
        }

        // 设置为进程间共享
        ret = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
        if (ret != 0)
        {
            pthread_mutexattr_destroy(&attr);
            throw std::system_error(ret, std::generic_category(), 
                "ShmLock: failed to set PTHREAD_PROCESS_SHARED");
        }

        // 设置为健壮锁（如果支持）
        // 这样当持有锁的进程崩溃时，其他进程可以恢复锁
#ifdef PTHREAD_MUTEX_ROBUST
        ret = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
        if (ret != 0)
        {
            // 不是致命错误，某些系统可能不支持
        }
#endif

        // 设置为错误检查类型（调试时有用）
#ifdef _DEBUG
        ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
#else
        ret = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_DEFAULT);
#endif
        if (ret != 0)
        {
            pthread_mutexattr_destroy(&attr);
            throw std::system_error(ret, std::generic_category(), 
                "ShmLock: failed to set mutex type");
        }

        // 初始化互斥锁
        ret = pthread_mutex_init(mutex_, &attr);
        if (ret != 0)
        {
            pthread_mutexattr_destroy(&attr);
            throw std::system_error(ret, std::generic_category(), 
                "ShmLock: failed to initialize mutex");
        }

        pthread_mutexattr_destroy(&attr);
    }

private:
    std::string name_;              ///< 共享内存名称
    pthread_mutex_t* mutex_;        ///< 指向共享内存中的互斥锁
    SharedMemory mutex_memory_;     ///< 共享内存管理器
    bool is_owner_;                 ///< 是否是创建者（用于决定是否销毁）
};
