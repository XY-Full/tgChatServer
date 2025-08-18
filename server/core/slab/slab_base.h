#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <mutex>

class SlabAllocatorBase
{
public:
    struct SlabPage {
        SlabPage* next;
        uint8_t*  free_list;
        uint32_t  free_count;
    };

    SlabAllocatorBase(size_t obj_size, size_t alignment = 8);
    virtual ~SlabAllocatorBase();

    void* alloc();
    void  free(void* p);

protected:
    virtual void* alloc_page(size_t size);
    virtual void  free_page(void* p, size_t size);

    size_t obj_size_;
    size_t slab_size_;
    size_t alignment_;

    std::vector<SlabPage*> partial_slabs_;
    std::vector<SlabPage*> full_slabs_;
    std::vector<SlabPage*> empty_slabs_;
    std::mutex lock_;
};
