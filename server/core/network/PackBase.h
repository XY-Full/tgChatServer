#ifndef PACK_BASE_H
#define PACK_BASE_H

#include "../../common/Log.h"
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#define MAGIC_VERSION 0x01

#pragma pack(push, 1)

enum class Type : uint8_t
{
    UNKNOWN = 0,
    C2S = 1, // 客户端->服务端
    S2C = 2, // 服务端->客户端
    S2SReq = 3, // 服务端->服务端Req
    S2SRsp = 3, // 服务端->服务端Rsp
};

// 包头部结构
struct Header
{
    uint8_t version_;   // 魔数包头版本
    Type type_;         // 包类型
    uint16_t pack_len_; // 负载总长度
    uint32_t seq_;      // 序列号
};

class PackBase
{
public:
    PackBase();
    virtual ~PackBase()
    {
        if (data_)
        {
            delete data_;
            data_ = nullptr;
        }
    }

    // 包头
    Header header_;

    // 消息id，根据这个进行匹配对应回调
    uint16_t msg_id_;
    // body长度
    uint16_t data_len_;
    // body指针，指向紧跟在包体区域后的内存区域
    char *data_;
};

#pragma pack(pop)
#endif // PACK_BASE_H