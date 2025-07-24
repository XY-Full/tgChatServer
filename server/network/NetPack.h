#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include "google/protobuf/message.h"
#include "ServerStruct.h"

class NetPack
{
public:
    int32_t len     = 0;
    int32_t seq     = 0;
    int32_t msg_id  = 0;
    int64_t conn_id = 0;
    int64_t uid     = 0;
    int8_t  flag    = 0;

    std::string msg;

    NetPack() = default;
    NetPack(const NetPack& request, const google::protobuf::Message* msg, int8_t msg_id = None);
    NetPack(const google::protobuf::Message* msg, int8_t msg_id = None);

    std::shared_ptr<std::string> serialize() const;
    void deserialize(int64_t conn_id, const std::string& data);
};
