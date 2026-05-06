#pragma once

#include <string>
#include <vector>

// 列定义
struct ColumnDef
{
    std::string name;
    std::string type;          // e.g. "VARCHAR(64)", "BIGINT", "DATETIME"
    bool        not_null      = false;
    bool        auto_increment = false;
    bool        primary_key   = false;
    std::string default_val;   // 空字符串表示无默认值
    std::string on_update;     // 仅 DATETIME 类型用，如 "CURRENT_TIMESTAMP"
};

// 索引定义
struct IndexDef
{
    std::string              name;
    std::vector<std::string> columns;
    bool                     unique = false;
};

// CREATE TABLE 操作
struct CreateTableOp
{
    std::string              table;
    std::vector<ColumnDef>   columns;
    std::vector<IndexDef>    indexes;
    std::string              engine  = "InnoDB";
    std::string              charset = "utf8mb4";
    std::string              comment;
};

// ALTER TABLE 操作
struct AlterTableOp
{
    std::string            table;
    std::vector<ColumnDef> add_columns;
    std::vector<std::string> drop_columns;
    std::vector<ColumnDef> modify_columns;
    // add_column 的 AFTER 子句通过 ColumnDef 扩展字段传递（after_column）
    std::string            after_column; // 仅对 add_columns[0] 有效（多列 AFTER 不常用）
};

// CREATE INDEX 操作
struct CreateIndexOp
{
    std::string              table;
    std::string              name;
    std::vector<std::string> columns;
    bool                     unique = false;
};

// DROP INDEX 操作
struct DropIndexOp
{
    std::string table;
    std::string name;
};

// DROP TABLE 操作
struct DropTableOp
{
    std::string table;
    bool        if_exists = true;
};

// SEED DATA 操作（INSERT IGNORE）
struct SeedDataOp
{
    std::string                                        table;
    std::vector<std::vector<std::pair<std::string, std::string>>> rows; // [{col, val}, ...]
};

// 操作类型标签
enum class OpType
{
    CreateTable,
    AlterTable,
    CreateIndex,
    DropIndex,
    DropTable,
    SeedData,
};

// 单个操作（tagged union）
struct Operation
{
    OpType type;

    CreateTableOp  create_table;
    AlterTableOp   alter_table;
    CreateIndexOp  create_index;
    DropIndexOp    drop_index;
    DropTableOp    drop_table;
    SeedDataOp     seed_data;
};

// 生成 CREATE TABLE SQL
std::string generateCreateTable(const CreateTableOp& op);

// 生成 ALTER TABLE SQL（可能返回多条语句，用 ';' 分隔）
std::string generateAlterTable(const AlterTableOp& op);

// 生成 CREATE [UNIQUE] INDEX SQL
std::string generateCreateIndex(const CreateIndexOp& op);

// 生成 DROP INDEX SQL
std::string generateDropIndex(const DropIndexOp& op);

// 生成 DROP TABLE SQL
std::string generateDropTable(const DropTableOp& op);

// 生成 INSERT IGNORE SQL（种子数据）
std::string generateSeedData(const SeedDataOp& op);

// 将 Operation 转换为 SQL 字符串（可能含多条语句）
std::string operationToSQL(const Operation& op);
