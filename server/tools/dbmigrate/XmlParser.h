#pragma once

#include "SQLGenerator.h"
#include <string>
#include <vector>

// 单个迁移文件解析结果
struct MigrationFile
{
    int                    version;     // 从文件名提取，如 V001 -> 1
    std::string            description; // <migration description="..."> 属性
    std::string            filename;    // 文件名（不含路径）
    std::string            filepath;    // 完整路径
    std::vector<Operation> operations; // 解析后的操作列表
};

// 解析单个迁移 XML 文件
// 失败时抛出 std::runtime_error
MigrationFile parseMigrationFile(const std::string& filepath);

// 校验 XML 文件格式（不执行 SQL，不连接数据库）
// 返回错误描述列表（空则表示通过）
std::vector<std::string> validateMigrationFile(const std::string& filepath);
