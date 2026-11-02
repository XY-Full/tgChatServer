#include "SqlBuilder.h"

namespace LogicDB
{
namespace sql
{

namespace
{

std::string join(const std::vector<std::string_view>& parts)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i) out += ", ";
        out += parts[i];
    }
    return out;
}

std::string placeholders(size_t n)
{
    std::string out;
    for (size_t i = 0; i < n; ++i)
    {
        if (i) out += ", ";
        out += '?';
    }
    return out;
}

std::string orderSql(const std::vector<Order>& order)
{
    std::string out;
    for (const auto& o : order)
    {
        if (o.empty()) continue;
        if (!out.empty()) out += ", ";
        out += o.column;
        out += (o.sort == Sort::Desc) ? " DESC" : " ASC";
    }
    return out;
}

std::string limitSql(int64_t limit)
{
    return limit > 0 ? " LIMIT " + std::to_string(limit) : std::string();
}

} // namespace

std::string selectSql(std::string_view table, const std::vector<std::string_view>& columns,
                      std::string_view where, const std::vector<Order>& order, int64_t limit)
{
    std::string out = "SELECT " + join(columns) + " FROM " + std::string(table);
    if (!where.empty()) out += " WHERE " + std::string(where);
    const std::string ord = orderSql(order);
    if (!ord.empty()) out += " ORDER BY " + ord;
    out += limitSql(limit);
    return out;
}

std::string countSql(std::string_view table, std::string_view where)
{
    std::string out = "SELECT COUNT(*) AS cnt FROM " + std::string(table);
    if (!where.empty()) out += " WHERE " + std::string(where);
    return out;
}

std::string insertSql(std::string_view table, const std::vector<std::string_view>& columns)
{
    return "INSERT INTO " + std::string(table) +
           " (" + join(columns) + ") VALUES (" + placeholders(columns.size()) + ")";
}

std::string upsertSql(std::string_view table, const std::vector<std::string_view>& columns,
                      std::string_view on_conflict)
{
    return insertSql(table, columns) + " ON DUPLICATE KEY UPDATE " + std::string(on_conflict);
}

std::string conflictSql(const std::vector<std::string_view>& columns)
{
    std::string out;
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (i) out += ", ";
        out += columns[i];
        out += " = VALUES(";
        out += columns[i];
        out += ')';
    }
    return out;
}

std::string updateSql(std::string_view table, const std::vector<std::string_view>& columns,
                      std::string_view primary_key)
{
    std::string out = "UPDATE " + std::string(table) + " SET ";
    for (size_t i = 0; i < columns.size(); ++i)
    {
        if (i) out += ", ";
        out += columns[i];
        out += " = ?";
    }
    out += " WHERE " + std::string(primary_key) + " = ?";
    return out;
}

std::string deleteByIdSql(std::string_view table, std::string_view primary_key)
{
    return "DELETE FROM " + std::string(table) + " WHERE " + std::string(primary_key) + " = ?";
}

std::string deleteWhereSql(std::string_view table, std::string_view where)
{
    std::string out = "DELETE FROM " + std::string(table);
    if (!where.empty()) out += " WHERE " + std::string(where);
    return out;
}

} // namespace sql
} // namespace LogicDB
