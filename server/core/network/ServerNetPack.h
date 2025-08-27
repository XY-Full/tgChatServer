#pragma once

#include "ServerStruct.h"
#include "google/protobuf/message.h"
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

class ServerNetPack
{
public:
    int32_t len = 0;
    int8_t flag = 0;
    int32_t msg_len = 0; // msg中的前n个字节为消息名
    std::string msg;

    ServerNetPack() = default;

    // 从请求和Protobuf消息创建响应包
    ServerNetPack(const ServerNetPack &request, const google::protobuf::Message *msg, int8_t flag = None);

    // 从Protobuf消息创建新包
    ServerNetPack(const google::protobuf::Message *msg, int8_t flag = None);

    // 序列化包为二进制数据
    std::shared_ptr<std::string> serialize() const;

    // 从二进制数据反序列化包
    void deserialize(int64_t conn_id, const std::string &data);

    // 获取消息名
    std::string get_message_name() const;

    // 获取消息体数据
    std::string get_message_body() const;
};