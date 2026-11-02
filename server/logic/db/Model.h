#pragma once

#include "LogicDB.h"
#include "db/Column.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace LogicDB
{

/**
 * @brief 实体映射 trait
 *
 * 由 db_model_gen.py 从 .proto 生成（proto_files/db_models.h），
 * 提供 table() 与列元组 all；下面这些通用实现据此推导列清单、取值与回填。
 */
template <typename T> struct Model;

// ── 绑定参数转换 ──
inline Value toValue(std::integral auto v)       { return static_cast<int64_t>(v); }
inline Value toValue(std::floating_point auto v) { return static_cast<double>(v); }
inline Value toValue(std::string_view v)         { return std::string(v); }
inline Value toValue(const char* v)              { return std::string(v); }
inline Value toValue(const std::string& v)       { return v; }

// ── 由列元组派生的通用实现 ──
template <typename T>
std::vector<std::string_view> modelColumns()
{
    std::vector<std::string_view> out;
    std::apply([&](const auto&... col) { (out.push_back(col.name), ...); }, Model<T>::all);
    return out;
}

template <typename T>
std::vector<std::string_view> modelWriteColumns()
{
    std::vector<std::string_view> out;
    std::apply(
        [&](const auto&... col)
        {
            (..., (hasFlag(col.flags, FieldFlag::AutoIncrement) ? void() : void(out.push_back(col.name))));
        },
        Model<T>::all);
    return out;
}

// INSERT / UPDATE 的业务列取值（顺序与 modelWriteColumns 一致）
template <typename T>
Params modelValues(const T& e)
{
    Params out;
    std::apply(
        [&](const auto&... col)
        {
            (..., (hasFlag(col.flags, FieldFlag::AutoIncrement)
                       ? void()
                       : void(out.emplace_back(toValue(col.get(e))))));
        },
        Model<T>::all);
    return out;
}

template <typename T>
std::optional<std::string> modelPkName()
{
    std::optional<std::string> out;
    std::apply(
        [&](const auto&... col)
        {
            (..., (hasFlag(col.flags, FieldFlag::PrimaryKey) ? void(out = std::string(col.name)) : void()));
        },
        Model<T>::all);
    return out;
}

template <typename T>
Value modelPkValue(const T& e)
{
    Value out;
    std::apply(
        [&](const auto&... col)
        {
            (..., (hasFlag(col.flags, FieldFlag::PrimaryKey) ? void(out = toValue(col.get(e))) : void()));
        },
        Model<T>::all);
    return out;
}

template <typename Col>
auto columnFromRow(const Row& r, const Col& col)
{
    using V = typename Col::Value;
    if constexpr (std::is_same_v<V, std::string>)  return r.asString(col.name).value_or("");
    else if constexpr (std::is_same_v<V, int64_t>) return r.asInt64(col.name).value_or(0);
    else if constexpr (std::is_same_v<V, int32_t>) return r.asInt32(col.name).value_or(0);
    else                                           return static_cast<V>(r.asDouble(col.name).value_or(0));
}

// 一行结果 -> protobuf 表消息
template <typename T>
T modelFromRow(const Row& r)
{
    T e;
    std::apply([&](const auto&... col) { (..., col.set(e, columnFromRow(r, col))); }, Model<T>::all);
    return e;
}

} // namespace LogicDB
