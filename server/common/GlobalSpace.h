#pragma once

#include "../core/shm/shm_slab.h"

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)

namespace IBus { class BusClient; };
class ConfigManager;
class Timer;

struct GlobalStruct
{
    IBus::BusClient* bus_;
    ConfigManager* configMgr_;
    shmslab::ShmSlab shm_slab_{"SSPackShmSlab"};
    Timer* timer_;
};

GlobalStruct *GlobalSpace();