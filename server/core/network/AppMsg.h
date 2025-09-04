#pragma once

#include "PackBase.h"

#pragma pack(push, 1)

class AppMsg : public PackBase
{
public:
    char src_name[16];      // 包来源服务名
    char dst_name[16];      // 包目的服务(名)
};

#pragma pack(pop)