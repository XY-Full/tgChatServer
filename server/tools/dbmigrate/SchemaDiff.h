#pragma once

#include "db_schema.h"

#include <mariadb/mysql.h>

#include <optional>
#include <string>
#include <vector>

namespace dbmigrate
{

// ── 描述现有库结构 ──
struct LiveColumn
{
    std::string name;
    std::string sql_type;
    bool        not_null       = false;
    bool        has_default    = false;
    std::string default_value;
    bool        auto_increment = false;
    std::string on_update;
    bool        primary_key    = false;
};

struct LiveIndex
{
    std::string              name;
    bool                     unique  = false;
    std::vector<std::string> columns;
};

struct LiveTable
{
    std::string             name;
    std::string             engine;
    std::string             charset;
    std::string             comment;
    std::vector<LiveColumn> columns;
    std::vector<LiveIndex>  indexes;
};

// ── 变更计划 ──
struct Change
{
    std::string sql;
    std::string describe;
    bool        destructive = false; // 删列 / 缩类型 / 改主键等需要人工确认
};

/**
 * @brief 期望结构与线上库比对
 *
 * 只负责算差异 + 生成 DDL 文本，执行与记录由 MigrationEngine 负责。
 */
class SchemaDiffer
{
public:
    SchemaDiffer(MYSQL* mysql, std::string database);

    // 读取线上结构
    bool loadLive(std::string& err);

    // 产出 DDL 计划；无差异返回空
    std::vector<Change> diff() const;

    const std::vector<LiveTable>& live() const { return live_; }

private:
    const LiveTable* findTable(const std::string& name) const;

    MYSQL*                 mysql_;
    std::string            database_;
    std::vector<LiveTable> live_;
};

// 归一化：类型、默认值
std::string normalizeType(const std::string& type);
std::string normalizeDefault(const std::string& value);

// 列定义片段：col TYPE [NOT NULL] [DEFAULT x] [AUTO_INCREMENT] [ON UPDATE ...] [COMMENT ...]
std::string columnDefinition(const std::string& name, const std::string& type, bool not_null,
                             bool primary_key, bool auto_increment, bool has_default,
                             const std::string& default_value, const std::string& on_update,
                             const std::string& comment);

} // namespace dbmigrate
