#include "shm.h"
#include <cstring>
#include <sys/mman.h>

SharedMemory::SharedMemory()
    :
#if defined(_WIN32) || defined(_WIN64)
      m_handle(nullptr),
#endif
      m_addr(nullptr), m_size(0)
{
}

SharedMemory::~SharedMemory()
{
    // 默认不销毁共享内存数据
    Close(false);
}

int32_t SharedMemory::Open(const std::string &name, std::size_t size, bool create)
{
#if defined(_WIN32) || defined(_WIN64)
    // Windows实现
    DWORD protect = PAGE_READWRITE;
    DWORD access = FILE_MAP_ALL_ACCESS;
    int32_t result = SHM_ERROR;    // -1代表映射有误，0代表已存在，1代表新建
    
    // 先尝试打开现有的共享内存
    m_handle = OpenFileMappingA(access, FALSE, name.c_str());
    
    if (m_handle)
    {
        // 共享内存已存在，检查大小
        // 注意：Windows无法直接查询映射对象的大小，需要其他方法
        // 这里我们使用一个技巧：尝试映射并检查文件大小
        
        // 创建临时视图来检查大小
        void* temp_addr = MapViewOfFile(m_handle, access, 0, 0, 0);
        if (!temp_addr)
        {
            CloseHandle(m_handle);
            m_handle = nullptr;
            return result;
        }
        
        // 获取内存信息
        MEMORY_BASIC_INFORMATION info;
        if (VirtualQuery(temp_addr, &info, sizeof(info)))
        {
            // 检查区域大小
            if (info.RegionSize != size)
            {
                // 大小不匹配，报错
                UnmapViewOfFile(temp_addr);
                CloseHandle(m_handle);
                m_handle = nullptr;
                return result;
            }
        }
        
        // 大小匹配，使用这个映射
        UnmapViewOfFile(temp_addr);
        m_size = size;

        result = SHM_EXIST;
    }
    else if (create)
    {
        // 共享内存不存在，且允许创建
        m_handle = CreateFileMappingA(
            INVALID_HANDLE_VALUE, 
            nullptr, 
            protect,
            static_cast<DWORD>((static_cast<unsigned long long>(size) >> 32) & 0xFFFFFFFFull),
            static_cast<DWORD>(static_cast<unsigned long long>(size) & 0xFFFFFFFFull), 
            name.c_str()
        );
        
        if (!m_handle)
            return result;
        
        m_size = size;

        result = SHM_CREATE;
    }
    else
    {
        // 共享内存不存在，且不允许创建
        return result;
    }

    // 创建视图
    m_addr = MapViewOfFile(m_handle, access, 0, 0, 0);
    if (!m_addr)
    {
        Close();
        return SHM_ERROR;
    }

    return result;

#else
    // POSIX实现
    int32_t result = SHM_ERROR;    // -1代表映射有误，0代表已存在，1代表新建
    int flags = O_RDWR;
    std::string shm_name = name.empty() || name[0] == '/' ? name : ("/" + name);
    
    // 先尝试打开现有的共享内存
    int fd = shm_open(shm_name.c_str(), O_RDWR, 0666);
    
    if (fd != -1)
    {
        // 共享内存已存在，检查大小
        struct stat st;
        if (fstat(fd, &st) == -1)
        {
            close(fd);
            return result;
        }
        
        // 检查大小是否匹配
        if (static_cast<std::size_t>(st.st_size) != size)
        {
            close(fd);
            return result; // 大小不匹配，报错
        }
        
        // 大小匹配，继续映射
        m_size = static_cast<std::size_t>(st.st_size);
        result = SHM_EXIST;
    }
    else if (create && errno == ENOENT)
    {
        // 共享内存不存在，且允许创建
        flags |= O_CREAT | O_EXCL;
        fd = shm_open(shm_name.c_str(), flags, 0666);
        
        if (fd == -1)
            return result;
        
        // 设置大小
        if (ftruncate(fd, static_cast<off_t>(size)) == -1)
        {
            close(fd);
            shm_unlink(shm_name.c_str());
            return result;
        }
        
        m_size = size;
        result = SHM_CREATE;
    }
    else
    {
        // 共享内存不存在，且不允许创建，或者其他错误
        return result;
    }
    
    // 映射共享内存
    m_addr = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    
    if (m_addr == MAP_FAILED)
    {
        m_addr = nullptr;
        m_size = 0;
        if (create)
            shm_unlink(shm_name.c_str());
        return SHM_ERROR;
    }
    
    m_name = shm_name;
    return result;
#endif
}

bool SharedMemory::Read(void *buffer, std::size_t size, std::size_t offset)
{
    if (!m_addr || offset > m_size || size > m_size - offset)
        return false;
    std::memcpy(buffer, static_cast<const char *>(m_addr) + offset, size);
    return true;
}

bool SharedMemory::Write(const void *buffer, std::size_t size, std::size_t offset)
{
    if (!m_addr || offset > m_size || size > m_size - offset)
        return false;
    std::memcpy(static_cast<char *>(m_addr) + offset, buffer, size);
    // 如需跨机持久，可在此加入 msync(MS_SYNC)
    return true;
}

void SharedMemory::Close(bool remove)
{
#if defined(_WIN32) || defined(_WIN64)
    if (m_addr)
    {
        UnmapViewOfFile(m_addr);
        m_addr = nullptr;
    }
    if (m_handle)
    {
        CloseHandle(m_handle);
        m_handle = nullptr;
    }
#else
    if (m_addr)
    {
        munmap(m_addr, m_size);
        m_addr = nullptr;
    }

    if(remove && !m_name.empty())
    {
        shm_unlink(m_name.c_str());
    }
#endif
    m_size = 0;
}