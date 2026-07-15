#pragma once

#include "XmlParser.h"
#include <mariadb/mysql.h>
#include <cstdint>
#include <string>
#include <vector>

// 已应用的迁移记录（从 db_schema_version 读取）
struct AppliedMigration
{
    int         version;
    std::string description;
    std::string filename;
    std::string checksum;   // SHA256 of file content
};

// 迁移引擎配置
struct MigrationConfig
{
    std::string host     = "127.0.0.1";
    uint16_t    port     = 3306;
    std::string user     = "root";
    std::string password = "";
    std::string database;
    std::string migrations_dir = "./db/migrations";
    bool        dry_run        = false;
    bool        verbose        = false;
};

// 迁移引擎：扫描文件、对比数据库记录、执行未应用的迁移
class MigrationEngine
{
public:
    explicit MigrationEngine(const MigrationConfig& config);
    ~MigrationEngine();

    // 执行所有未应用的迁移
    bool migrate();

    // 显示迁移状态（已应用/待应用）
    void status();

    // 创建新迁移 XML 文件骨架
    static void createMigrationFile(const std::string& dir, const std::string& name);

    // 读取 .sql 文件，识别 CREATE TABLE 并生成对应的迁移 XML 文件
    void importSchema(const std::string& sql_file);

    // 列出数据库所有用户表
    void listTables();

    // 清空所有用户表数据（TRUNCATE，保留结构）
    bool truncateAll();

    // 删除所有用户表，然后根据 sql_file 重建数据库
    bool rebuild(const std::string& sql_file);

private:
    bool connect();
    void disconnect();
    bool ensureVersionTable();
    std::vector<AppliedMigration> loadAppliedMigrations();
    std::vector<MigrationFile>    scanMigrationFiles();
    std::string computeChecksum(const std::string& filepath);
    bool applyMigration(const MigrationFile& mf);
    bool execSQL(const std::string& sql, std::string& err);
    std::string escapeSQL(const std::string& s); // mysql_real_escape_string 包装
    std::vector<std::string> getUserTables();

    MigrationConfig config_;
    MYSQL           mysql_;
    bool            connected_ = false;
};
