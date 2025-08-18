#include "shm_slab.h"
#include <cassert>
#include <thread>

namespace shmslab {

static constexpr uint32_t kMagic = 0x534C4142; // 'SLAB'

static inline uint32_t pow2ceil(uint32_t x){
    if (x<=1) return 1;
    --x; x|=x>>1; x|=x>>2; x|=x>>4; x|=x>>8; x|=x>>16; return x+1;
}

uint32_t ShmSlab::SizeClassIndex(uint32_t bytes){
    // 16, 32, 64, 128, 256, 512, 1024, 2048, 4096, 8192
    uint32_t s = pow2ceil(bytes);
    if (s < 16)  s = 16;
    if (s > 8192) return UINT32_MAX;
    uint32_t idx = 0;
    uint32_t v = 16;
    while (v < s && idx+1 < MAX_CLASSES) { v <<= 1; ++idx; }
    return idx;
}
uint32_t ShmSlab::RoundToClassSize(uint32_t bytes){
    uint32_t s = pow2ceil(bytes);
    if (s < 16)  s = 16;
    if (s > 8192) s = pow2ceil(bytes); // 超过最大类，由上层按大块策略处理（此处仍返回对齐值）
    return (s+7u)&~7u;
}

void ShmSlab::Lock(std::atomic<uint32_t>& l){
    for (uint32_t spins=1;;){
        uint32_t exp=0;
        if (l.compare_exchange_weak(exp,1,std::memory_order_acquire)) return;
        for (uint32_t i=0;i<spins;++i) std::this_thread::yield();
        if (spins < (1u<<10)) spins<<=1;
    }
}
void ShmSlab::Unlock(std::atomic<uint32_t>& l){ l.store(0,std::memory_order_release); }

ShmSlab::ShmSlab(void* base, uint32_t total, uint32_t region_off, uint32_t region_size, bool create)
    : base_(base), total_(total)
{
    hdr_ = reinterpret_cast<SlabHeader*>(reinterpret_cast<uint8_t*>(base_) + region_off);
    if (create) {
        new (hdr_) SlabHeader{};
        hdr_->magic = kMagic;
        hdr_->version = 1;
        hdr_->total_size = total_;
        hdr_->base_offset = region_off;
        hdr_->bump.store(region_off + sizeof(SlabHeader), std::memory_order_relaxed);
        hdr_->end = region_off + region_size;

        // 初始化 size-class
        uint32_t sizes[MAX_CLASSES] = {16,32,64,128,256,512,1024,2048,4096,8192};
        for (uint32_t i=0;i<MAX_CLASSES;++i){
            auto& c = hdr_->classes[i];
            c.item_size = (sizes[i]+7u)&~7u;
            c.items_per_page = (PAGE_SIZE - sizeof(PageHeader) - 256 /*bitmap预算*/)/c.item_size;
            if (c.items_per_page == 0) c.items_per_page = 1;
            // bitmap 用 bytes = ceil(items/8)，这里保守在 NewPage 精确计算
            c.free_list = 0;
            c.page_list = 0;
            c.lock.store(0, std::memory_order_relaxed);
        }
    } else {
        if (hdr_->magic != kMagic || hdr_->version != 1) {
            throw std::runtime_error("ShmSlab: incompatible header");
        }
    }
}

uint32_t ShmSlab::NewPage(uint32_t cls){
    auto& c = hdr_->classes[cls];
    // bump 分配一页
    for (;;) {
        uint32_t cur = hdr_->bump.load(std::memory_order_relaxed);
        uint32_t next = cur + PAGE_SIZE;
        if (next > hdr_->end) return 0; // 空间耗尽
        if (hdr_->bump.compare_exchange_weak(cur, next, std::memory_order_acq_rel)) {
            // 初始化本页
            uint8_t* page = reinterpret_cast<uint8_t*>(base_)+cur;
            auto* ph = new (page) PageHeader{};
            ph->next_page = c.page_list;
            ph->class_idx = cls;
            ph->free_count = 0;
            // 精确计算位图大小
            uint32_t items = (PAGE_SIZE - sizeof(PageHeader)) / c.item_size;
            if (items > 8192) items = 8192; // 位图上限
            uint32_t bitmap_bytes = (items + 7u) / 8u;
            ph->bitmap_off = sizeof(PageHeader);
            std::memset(page + ph->bitmap_off, 0, bitmap_bytes);

            // 挂接 page_list
            c.page_list = ptr2off(ph);

            // 初始化空闲链（将本页所有 chunk 链起来）
            uint32_t first_free = 0;
            uint32_t payload_off = ph->bitmap_off + bitmap_bytes;
            uint32_t capacity = (PAGE_SIZE - payload_off) / c.item_size;
            ph->free_count = capacity;
            uint32_t prev = 0;
            for (uint32_t i=0;i<capacity;++i){
                uint32_t off = cur + payload_off + i*c.item_size;
                // 在 chunk 开头放一个 next 偏移（SLL），释放/分配无需单独结构
                *reinterpret_cast<uint32_t*>(base8()+off) = prev; // next
                prev = off;
            }
            first_free = prev;

            // 把该页的 SLL 头并入 class 的 free_list
            // 将链表头指向 first_free
            // 注意：class 外层已加锁
            // free_list 的每个节点的*首4字节*存放 next 偏移
            if (first_free) {
                // 将最后一个节点的 next 指向原 free_list 头
                uint32_t tail = cur + payload_off; // 链的第一个（链尾）
                *reinterpret_cast<uint32_t*>(base8()+tail) = c.free_list;
                c.free_list = first_free;
            }
            return ptr2off(ph);
        }
    }
}

uint32_t ShmSlab::Alloc(uint32_t bytes){
    bytes = Align8(bytes);
    uint32_t cls = SizeClassIndex(bytes);
    if (cls == UINT32_MAX) return 0; // 超出最大类，实际可扩展为“直连大块分配器”

    auto& c = hdr_->classes[cls];
    Lock(c.lock);
    // 优先从 free_list 取
    if (c.free_list == 0) {
        // 尝试新页
        if (NewPage(cls) == 0 && c.free_list == 0) {
            Unlock(c.lock);
            return 0;
        }
    }
    // pop 头结点
    uint32_t off = c.free_list;
    uint32_t* pNext = reinterpret_cast<uint32_t*>(base8()+off);
    c.free_list = *pNext; // 下一节点成为链头
    Unlock(c.lock);

    return off;
}

void ShmSlab::Free(uint32_t off, uint32_t bytes){
    if (!off) return;
    bytes = Align8(bytes);
    uint32_t cls = SizeClassIndex(bytes);
    if (cls == UINT32_MAX) return;

    auto& c = hdr_->classes[cls];
    Lock(c.lock);
    // 将该块 push 到 free_list 头部
    uint32_t* pNext = reinterpret_cast<uint32_t*>(base8()+off);
    *pNext = c.free_list;
    c.free_list = off;
    Unlock(c.lock);
}

} // namespace shmslab
