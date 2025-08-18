#pragma once
#include "shm_base.h"
#include <cstddef>
#include <string>

#if defined(_WIN32) || defined(_WIN64)
#  include <windows.h>
#else
#  include <sys/mman.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

class SharedMemory : public ISharedMemory {
public:
    SharedMemory();
    ~SharedMemory() override;

    // create=true: 创建或打开(幂等)，如果已存在则直接打开并忽略size
    // create=false: 仅附着已有
    bool Open(const std::string& name, std::size_t size, bool create = true) override;

    void* GetAddress() const override { return m_addr; }
    std::size_t GetSize() const override { return m_size; }

    bool Read(void* buffer, std::size_t size, std::size_t offset = 0) override;
    bool Write(const void* buffer, std::size_t size, std::size_t offset = 0) override;

    void Close() override;

private:
#if defined(_WIN32) || defined(_WIN64)
    HANDLE m_handle;
#endif
    void* m_addr;
    std::size_t m_size;
};
