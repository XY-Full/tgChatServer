#pragma once
#include <cstddef>
#include <string>

class ISharedMemory
{
public:
    virtual ~ISharedMemory() = default;

    virtual bool Open(const std::string& name, std::size_t size, bool create = true) = 0;
    virtual void* GetAddress() const = 0;
    virtual std::size_t GetSize() const = 0;
    virtual bool Read(void* buffer, std::size_t size, std::size_t offset = 0) = 0;
    virtual bool Write(const void* buffer, std::size_t size, std::size_t offset = 0) = 0;
    virtual void Close() = 0;
};
