#include "shm.h"
#include <cstring>

SharedMemory::SharedMemory()
    :
#if defined(_WIN32) || defined(_WIN64)
      m_handle(nullptr),
#endif
      m_addr(nullptr),
      m_size(0) {}

SharedMemory::~SharedMemory() { Close(); }

bool SharedMemory::Open(const std::string& name, std::size_t size, bool create)
{
#if defined(_WIN32) || defined(_WIN64)
    // Windows: 命名映射对象
    DWORD protect = PAGE_READWRITE;
    DWORD access  = FILE_MAP_ALL_ACCESS;

    if (create) {
        // Create or open
        m_handle = CreateFileMappingA(
            INVALID_HANDLE_VALUE, nullptr, protect,
            static_cast<DWORD>((static_cast<unsigned long long>(size) >> 32) & 0xFFFFFFFFull),
            static_cast<DWORD>(static_cast<unsigned long long>(size) & 0xFFFFFFFFull),
            name.c_str()
        );
        if (!m_handle) return false;

        // 如果已存在，大小以创建时传入为准；我们仍把 m_size 设为调用方给的 size
        // 真正的“实际大小”在匿名映射上并不好查询；这点与 POSIX 不同。
        m_size = size;
    } else {
        m_handle = OpenFileMappingA(access, FALSE, name.c_str());
        if (!m_handle) return false;

        // 无法从 handle 直接拿到 mapping size，调用方需保证 size>=真实占用
        // 我们把 m_size 设为传入 size，用于边界检查；若想保守，可只做地址检查。
        m_size = size;
    }

    // 视图大小=0 表示整个映射，避免 32 位截断
    m_addr = MapViewOfFile(m_handle, access, 0, 0, 0);
    if (!m_addr) {
        Close();
        return false;
    }
    return true;

#else
    // POSIX
    int flags = O_RDWR;
    if (create) flags |= O_CREAT;

    // shm 名字必须以 '/' 开头
    std::string shm_name = name.empty() || name[0] == '/' ? name : ("/" + name);

    int fd = shm_open(shm_name.c_str(), flags, 0666);
    if (fd == -1) return false;

    struct stat st{};
    if (create) {
        // 创建或打开：如果是新建，ftruncate；如果已存在，ftruncate 不会缩小已有对象
        if (ftruncate(fd, static_cast<off_t>(size)) == -1) {
            close(fd);
            return false;
        }
    }

    if (fstat(fd, &st) == -1) {
        close(fd);
        return false;
    }

    // 附着已有时，实际大小以 fstat 为准
    m_size = static_cast<std::size_t>(st.st_size);

    m_addr = mmap(nullptr, m_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);

    if (m_addr == MAP_FAILED) {
        m_addr = nullptr;
        m_size = 0;
        return false;
    }
    return true;
#endif
}

bool SharedMemory::Read(void* buffer, std::size_t size, std::size_t offset)
{
    if (!m_addr || offset > m_size || size > m_size - offset) return false;
    std::memcpy(buffer, static_cast<const char*>(m_addr) + offset, size);
    return true;
}

bool SharedMemory::Write(const void* buffer, std::size_t size, std::size_t offset)
{
    if (!m_addr || offset > m_size || size > m_size - offset) return false;
    std::memcpy(static_cast<char*>(m_addr) + offset, buffer, size);
    // 如需跨机持久，可在此加入 msync(MS_SYNC)
    return true;
}

void SharedMemory::Close()
{
#if defined(_WIN32) || defined(_WIN64)
    if (m_addr) { UnmapViewOfFile(m_addr); m_addr = nullptr; }
    if (m_handle) { CloseHandle(m_handle); m_handle = nullptr; }
#else
    if (m_addr) { munmap(m_addr, m_size); m_addr = nullptr; }
#endif
    m_size = 0;
}
