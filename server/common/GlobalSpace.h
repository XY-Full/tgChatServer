#pragma once

#include "JsonConfig.h"
#include "../core/shm/shm_slab.h"
#include "Timer.h"

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

class Busd;
class JsonConfig;

struct GlobalStruct
{
    Busd *busd_;
    JsonConfig *configMgr_;
    shmslab::ShmSlab shm_slab_{"SSPackShmSlab"};
    Timer* timer_;
};

GlobalStruct *GlobalSpace();