#pragma once
#include "slab_base.h"

class HybridSlabAllocator : public SlabAllocatorBase
{
public:
    enum class PoolType { LOCAL, SHM };

    HybridSlabAllocator(size_t obj_size, size_t alignment = 8);

protected:
    void* alloc_page(size_t size) override;
    void  free_page(void* p, size_t size) override;

private:
    PoolType select_pool();
};
