#pragma once
#include <cstdint>
#include <cstddef>
#include <atomic>
#include <new>
#include <cstring>

namespace shmslab {

// 固定页大小（可按需调大）；偏移均为 base 相对
static constexpr uint32_t PAGE_SIZE = 64 * 1024; // 64KB
static constexpr uint32_t MAX_CLASSES = 10;      // 16..8192 共10个size-class
static inline uint32_t Align8(uint32_t x){ return (x + 7u) & ~7u; }

struct SlabClass {
    uint32_t item_size;      // 每个对象大小（对齐后）
    uint32_t items_per_page; // 该类每页可容纳项数
    uint32_t free_list;      // 空闲对象链头（offset，0=空）
    uint32_t page_list;      // 本类 page 链表（offset of PageHeader）
    std::atomic<uint32_t> lock; // 0=unlocked 1=locked（简单自旋锁）
};

struct PageHeader {
    uint32_t next_page;      // offset of next page
    uint32_t class_idx;      // 所属 size-class
    uint32_t free_count;     // 当前空闲数
    uint32_t bitmap_off;     // 位图偏移（相对本页起点）
    // 之后是 bitmap + items 区域（按 class item_size 切分）
};

// 分配器总头，放在共享内存里（偏移用 uint32_t）
struct SlabHeader {
    uint32_t magic;          // 'SLAB' 0x534C4142
    uint32_t version;        // 1
    uint32_t total_size;     // 整个共享内存大小
    uint32_t base_offset;    // 分配器起点（通常就是该头的偏移）

    // bump allocator 用于申请新页
    std::atomic<uint32_t> bump;  // 下一次可分配的起始 offset
    uint32_t end;                // 分配器管理范围终点 offset（不含）

    // size class 表
    SlabClass classes[MAX_CLASSES];
};

class ShmSlab {
public:
    // base: 共享内存基址；total: 共享内存总大小；region_off/region_size：分配器管理区间
    // create=true 时初始化，否则附着
    ShmSlab(void* base, uint32_t total, uint32_t region_off, uint32_t region_size, bool create);

    // 分配/释放：返回/接受 偏移量（0=失败）
    uint32_t Alloc(uint32_t bytes);
    void     Free(uint32_t off, uint32_t bytes);

    // 调试/统计
    uint32_t ClassCount() const { return MAX_CLASSES; }
    const SlabHeader* Header() const { return hdr_; }

private:
    inline uint8_t* base8() const { return reinterpret_cast<uint8_t*>(base_); }
    inline void* off2ptr(uint32_t off) const { return off ? base8()+off : nullptr; }
    inline uint32_t ptr2off(const void* p) const {
        if (!p) return 0; return static_cast<uint32_t>(reinterpret_cast<const uint8_t*>(p)-base8());
    }

    static uint32_t SizeClassIndex(uint32_t bytes);
    static uint32_t RoundToClassSize(uint32_t bytes);

    // 内部：生成新页并挂到 class
    uint32_t NewPage(uint32_t cls);
    // class 级简单自旋锁
    static void Lock(std::atomic<uint32_t>& l);
    static void Unlock(std::atomic<uint32_t>& l);

private:
    void* base_;
    uint32_t total_;
    SlabHeader* hdr_;
};

} // namespace shmslab
