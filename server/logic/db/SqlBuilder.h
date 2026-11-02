#pragma once

#include "Condition.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace LogicDB
{

/**
 * @brief SQL 文本拼装
 *
 * 只负责把表名、列名、条件片段拼成语句；值一律以 ? 占位，由调用方绑定
 */
namespace sql
{

std::string selectSql(std::string_view table, const std::vector<std::string_view>& columns,
                      std::string_view where, const std::vector<Order>& order, int64_t limit);

std::string countSql(std::string_view table, std::string_view where);

std::string insertSql(std::string_view table, const std::vector<std::string_view>& columns);

std::string upsertSql(std::string_view table, const std::vector<std::string_view>& columns,
                      std::string_view on_conflict);

// SET 全部业务列，按主键定位
// 冲突时把这些列覆盖为待插入的值
std::string conflictSql(const std::vector<std::string_view>& columns);

// SET 全部业务列，按主键定位
std::string updateSql(std::string_view table, const std::vector<std::string_view>& columns,
                      std::string_view primary_key);

std::string deleteByIdSql(std::string_view table, std::string_view primary_key);

std::string deleteWhereSql(std::string_view table, std::string_view where);

} // namespace sql

} // namespace LogicDB
