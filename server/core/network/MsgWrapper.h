#pragma once

#include "AppMsg.h"

class AppMsgWrapper
{
public:
    AppMsgWrapper() = default;
    ~AppMsgWrapper() = default;

    // 数据包在共享内存中的偏移，用于快速定位到包的地址
    uint32_t offset_;
    // 远程目标名称，用于本机快速路由
    char dst_[16];
};