#include "ServerNetPack.h"
#include <google/protobuf/descriptor.h>

// 从请求和Protobuf消息创建响应包
ServerNetPack::ServerNetPack(const ServerNetPack &request, const google::protobuf::Message *msg, int8_t flag)
{
    // 使用请求中的消息名作为响应消息名
    this->msg = request.get_message_name();
    this->msg_len = static_cast<int32_t>(this->msg.size());

    // 序列化Protobuf消息体
    if (msg)
    {
        this->msg += msg->SerializeAsString();
    }

    // 设置标志位
    this->flag = (flag == None) ? request.flag : flag;

    // 计算总长度: len(4) + flag(1) + msg_len(4) + msg_data(n)
    this->len = sizeof(len) + sizeof(flag) + sizeof(msg_len) + static_cast<int32_t>(this->msg.size());
}

// 从Protobuf消息创建新包
ServerNetPack::ServerNetPack(const google::protobuf::Message *msg, int8_t flag)
{
    // 使用Protobuf消息的全名作为消息名
    if (msg)
    {
        this->msg = msg->GetDescriptor()->full_name();
        this->msg_len = static_cast<int32_t>(this->msg.size());

        // 序列化消息体
        this->msg += msg->SerializeAsString();
    }
    else
    {
        this->msg_len = 0;
    }

    // 设置标志位
    this->flag = (flag == None) ? PUB_FLAG : flag;

    // 计算总长度
    this->len = sizeof(len) + sizeof(flag) + sizeof(msg_len) + static_cast<int32_t>(this->msg.size());
}

// 序列化包为二进制数据
std::shared_ptr<std::string> ServerNetPack::serialize() const
{
    auto data = std::make_shared<std::string>();
    data->reserve(len);

    // 写入长度字段 (小端字节序)
    for (int i = 0; i < 4; i++)
    {
        data->push_back(static_cast<char>((len >> (i * 8)) & 0xFF));
    }

    // 写入标志位
    data->push_back(static_cast<char>(flag));

    // 写入消息名长度 (小端字节序)
    for (int i = 0; i < 4; i++)
    {
        data->push_back(static_cast<char>((msg_len >> (i * 8)) & 0xFF));
    }

    // 写入消息数据
    data->append(msg);

    return data;
}

// 从二进制数据反序列化包
void ServerNetPack::deserialize(int64_t conn_id, const std::string &data)
{
    // 检查最小长度
    if (data.size() < sizeof(int32_t) * 2 + sizeof(int8_t) + sizeof(int32_t)) // len(4) + flag(1) + msg_len(4)
    {
        throw std::runtime_error("Invalid packet: too short");
    }

    // 读取长度字段 (小端字节序)
    len = 0;
    for (int i = 0; i < 4; i++)
    {
        len |= static_cast<int32_t>(static_cast<unsigned char>(data[i])) << (i * 8);
    }

    // 验证长度
    if (static_cast<size_t>(len) != data.size())
    {
        throw std::runtime_error("Packet length mismatch");
    }

    // 读取标志位
    flag = static_cast<int8_t>(data[4]);

    // 读取消息名长度 (小端字节序)
    msg_len = 0;
    for (int i = 0; i < 4; i++)
    {
        msg_len |= static_cast<int32_t>(static_cast<unsigned char>(data[5 + i])) << (i * 8);
    }

    // 读取消息数据
    if (data.size() > 9)
    {
        msg = data.substr(9);
    }
    else
    {
        msg.clear();
    }

    // 验证消息名长度
    if (msg_len > static_cast<int32_t>(msg.size()))
    {
        throw std::runtime_error("Message name length exceeds data size");
    }
}

// 获取消息名
std::string ServerNetPack::get_message_name() const
{
    if (msg_len > 0 && msg_len <= static_cast<int32_t>(msg.size()))
    {
        return msg.substr(0, msg_len);
    }
    return "";
}

// 获取消息体数据
std::string ServerNetPack::get_message_body() const
{
    if (msg_len >= 0 && msg_len < static_cast<int32_t>(msg.size()))
    {
        return msg.substr(msg_len);
    }
    return "";
}