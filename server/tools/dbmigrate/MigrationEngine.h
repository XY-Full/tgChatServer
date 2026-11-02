#pragma once

#include "SchemaDiff.h"

#include <mariadb/mysql.h>

#include <cstdint>
#include <string>
#include <vector>

// 已应用的迁移记录
struct AppliedMigration
{
    int         version;
    std::string description;
    std::string filename;
    std::string checksum;
    std::string applied_at;
};

struct MigrationConfig
{
    std::string host     = "127.0.0.1";
    uint16_t    port     = 3306;
    std::string user     = "root";
    std::string password = "";
    std::string database;
    bool        dry_run           = false;
    bool        verbose           = false;
    bool        allow_destructive = false; // 放行删列 / 缩类型等破坏性变更
};

/**
 * @brief 迁移引擎
 *
 * 期望结构来自 .proto 生成的 dbschema::tables()，与线上库比对得到 DDL 计划；
 * 本类负责执行计划并记录到 db_schema_version
 */
class MigrationEngine
{
public:
    explicit MigrationEngine(const MigrationConfig& config);
    ~MigrationEngine();

    bool connect(std::string& err);
    void disconnect();

    // 确保 db_schema_version 存在
    bool ensureVersionTable(std::string& err);

    // 计算待执行的 DDL 计划
    bool plan(std::vector<dbmigrate::Change>& changes, std::string& err);

    // 执行计划；含破坏性变更且未放行时返回 false
    bool apply(const std::vector<dbmigrate::Change>& changes, std::string& err);

    // 迁移历史
    bool history(std::vector<AppliedMigration>& out, std::string& err);

    // 用户表列表
    bool listTables(std::vector<std::string>& out, std::string& err);

private:
    bool execSQL(const std::string& sql, std::string& err);
    bool recordBatch(const std::vector<dbmigrate::Change>& changes, int64_t elapsed_ms, std::string& err);
    int  nextVersion();
    std::string escapeSQL(const std::string& s);

    MigrationConfig config_;
    MYSQL           mysql_;
    bool            connected_ = false;
};
