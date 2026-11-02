#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace LogicDB
{

// 列属性
enum class FieldFlag : unsigned
{
    None          = 0,
    PrimaryKey    = 1u << 0,
    AutoIncrement = 1u << 1, // 自增列：INSERT 时不写入，由数据库生成
};

constexpr FieldFlag operator|(FieldFlag a, FieldFlag b)
{
    return static_cast<FieldFlag>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

constexpr bool hasFlag(FieldFlag flags, FieldFlag bit)
{
    return (static_cast<unsigned>(flags) & static_cast<unsigned>(bit)) != 0;
}

/**
 * @brief 一列的定义
 *
 * @tparam T protobuf 表消息类型
 * @tparam V 列的 C++ 类型（int64_t / int32_t / double / std::string）
 */
template <typename T, typename V>
struct Column
{
    using Message = T;
    using Value   = V;

    std::string_view name;
    V (*get)(const T&);
    void (*set)(T&, V);
    bool (*has)(const T&); // 仅 proto3 optional 字段有，其余为 nullptr
    FieldFlag flags;
};

} // namespace LogicDB
