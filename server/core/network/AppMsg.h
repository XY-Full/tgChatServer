#pragma once

#include "PackBase.h"

#pragma pack(push, 1)

class AppMsg : public PackBase
{
public:
    uint64_t co_id_;        // 协程ID，用于回调
    char src_name_[16];     // 包来源服务名
    char dst_name_[16];     // 包目的服务(名)，允许正则匹配进程组
};

using AppMsgPtr = std::shared_ptr<AppMsg>;

#pragma pack(pop)