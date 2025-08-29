#ifndef PACK_BASE_H
#define PACK_BASE_H

#include "../../common/Log.h"
#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <chrono>

#define MAGIC_VERSION 0x01

#pragma pack(push, 1)

enum class Type : uint8_t {
    UNKNOWN = 0,
    C2S         = 1, // 客户端->服务端
    S2C         = 2, // 服务端->客户端
    S2S         = 3, // 服务端->服务端
    HEARTBEAT   = 4, // 心跳包
    ACK         = 5, // 确认包
    NACK        = 6, // 否定确认包
};

// 包头部结构
struct Header {
    uint8_t version;         // 魔数包头版本
    uint8_t type;            // 包类型
    uint32_t seq;            // 序列号
    uint16_t body_length;    // 包体长度
};

class PackBase {
public:
    PackBase();
    virtual ~PackBase()
    {
        if(data_)
        {
            delete data_;
            data_ = nullptr;
        }
    }

    // 包属性
    Type type_;
    uint32_t seq_;
    uint16_t data_len_;
    char* data_;
};

#pragma pack(pop)
#endif // PACK_BASE_H