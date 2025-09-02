#pragma once

#include "JsonConfig.h"
#include "../core/shm/shm_slab.h"
#include "Timer.h"


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