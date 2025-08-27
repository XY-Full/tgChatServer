// ibus_common.h
#ifndef IBUS_COMMON_H
#define IBUS_COMMON_H

#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <functional>
#include <unordered_map>

namespace IBus {

// 错误码
enum class ErrorCode {
    OK = 0,
    TIMEOUT,
    CONNECTION_ERROR,
    INVALID_TOPIC,
    INVALID_MESSAGE,
    NOT_READY,
    INTERNAL_ERROR,
    PERMISSION_DENIED,
    QUOTA_EXCEEDED
};

// 消息标志
enum MessageFlags : uint8_t {
    NONE = 0,
    REQUEST = 1 << 0,    // 请求消息
    RESPONSE = 1 << 1,   // 响应消息
    MULTICAST = 1 << 2,  // 组播消息
    PERSISTENT = 1 << 3, // 持久化消息
    PRIORITY_HIGH = 1 << 4, // 高优先级
    PRIORITY_LOW = 1 << 5   // 低优先级
};

// QoS级别
enum class QoS {
    AT_MOST_ONCE = 0,    // 最多一次
    AT_LEAST_ONCE = 1,   // 至少一次
    EXACTLY_ONCE = 2     // 恰好一次
};

// 消息结构
struct Message {
    uint64_t id;                // 消息ID
    std::string topic;          // 主题
    std::vector<uint8_t> data;  // 数据负载
    MessageFlags flags;         // 消息标志
    QoS qos;                    // 服务质量
    uint64_t timestamp;         // 时间戳
    uint64_t ttl;               // 存活时间(毫秒)
    uint64_t correlation_id;    // 关联ID(用于请求-响应)
    
    // 可选的消息属性
    std::unordered_map<std::string, std::string> properties;
};

// 统计信息
struct Stats {
    uint64_t messages_sent;
    uint64_t messages_received;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t errors;
    uint64_t connections;
};

// 订阅选项
struct SubscribeOptions {
    QoS qos = QoS::AT_LEAST_ONCE;
    bool no_local = false;      // 不接收自己发布的消息
    bool retain_as_published = false; // 保持发布时的保留标志
    uint32_t max_message_size = 1024 * 1024; // 最大消息大小
};

// 发布选项
struct PublishOptions {
    QoS qos = QoS::AT_LEAST_ONCE;
    bool retain = false;        // 服务器是否保留此消息
    uint64_t ttl = 0;           // 存活时间(毫秒)
};

// 连接选项
struct ConnectOptions {
    std::string client_id;
    std::string username;
    std::string password;
    uint16_t keep_alive = 60;   // 保活时间(秒)
    bool clean_session = true;  // 清理会话
    std::chrono::milliseconds connection_timeout{5000};
    std::chrono::milliseconds operation_timeout{3000};
};

// 回调函数类型
using MessageHandler = std::function<void(const Message&)>;
using ResponseHandler = std::function<void(ErrorCode, const Message&)>;
using ConnectionHandler = std::function<void(ErrorCode)>;
using DisconnectionHandler = std::function<void(ErrorCode)>;

} // namespace IBus

#endif // IBUS_COMMON_H