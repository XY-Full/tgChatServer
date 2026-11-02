#include "MigrationEngine.h"

#include <cstring>
#include <iostream>
#include <string>

static void printHelp()
{
    std::cout << R"(
dbmigrate — 声明式 MySQL schema 迁移工具

表结构写在 public/db_proto/*.proto，由 db_model_gen.py 生成期望结构；
本工具与线上库比对后产出 DDL 计划

Usage:
  dbmigrate <command> [options]

Commands:
  diff         对比 proto 期望结构与线上库，打印待执行的 DDL
  migrate      执行待执行的 DDL 并记录到 db_schema_version
  status       显示迁移历史 + 当前待执行变更数
  list-tables  列出数据库里的用户表

Options:
  -H, --host              MySQL host     (default: 127.0.0.1)
  -P, --port              MySQL port     (default: 3306)
  -u, --user              MySQL user     (default: root)
  -p, --password          MySQL password (default: empty)
  -d, --database          目标数据库
      --dry-run           只打印 SQL，不执行
      --allow-destructive 允许破坏性变更（删列 / 缩类型）
  -v, --verbose           打印每条 SQL

退出码:
  diff  0 = 结构一致；2 = 有变更待执行

Examples:
  dbmigrate diff    -H 127.0.0.1 -u root -p pwd -d tgchat
  dbmigrate migrate -H 127.0.0.1 -u root -p pwd -d tgchat
  dbmigrate status  -d tgchat
)";
}

// 支持 "-H host" / "--host host" / "--host=host"
static bool takeArg(int argc, char* argv[], int& i, const char* short_flag, const char* long_flag,
                    std::string& out)
{
    const std::string arg = argv[i];
    if (arg == short_flag || arg == long_flag)
    {
        if (i + 1 >= argc) return false;
        out = argv[++i];
        return true;
    }
    const std::string prefix = std::string(long_flag) + "=";
    if (arg.rfind(prefix, 0) == 0)
    {
        out = arg.substr(prefix.size());
        return true;
    }
    return false;
}

int main(int argc, char* argv[])
{
    if (argc < 2) { printHelp(); return 1; }

    const std::string command = argv[1];
    if (command == "--help" || command == "-h") { printHelp(); return 0; }

    MigrationConfig cfg;

    for (int i = 2; i < argc; ++i)
    {
        std::string v;
        if (takeArg(argc, argv, i, "-H", "--host", v))          { cfg.host = v; continue; }
        if (takeArg(argc, argv, i, "-P", "--port", v))          { cfg.port = static_cast<uint16_t>(std::stoi(v)); continue; }
        if (takeArg(argc, argv, i, "-u", "--user", v))          { cfg.user = v; continue; }
        if (takeArg(argc, argv, i, "-p", "--password", v))      { cfg.password = v; continue; }
        if (takeArg(argc, argv, i, "-d", "--database", v))      { cfg.database = v; continue; }
        if (std::strcmp(argv[i], "--dry-run") == 0)             { cfg.dry_run = true; continue; }
        if (std::strcmp(argv[i], "--allow-destructive") == 0)   { cfg.allow_destructive = true; continue; }
        if (std::strcmp(argv[i], "-v") == 0 || std::strcmp(argv[i], "--verbose") == 0) { cfg.verbose = true; continue; }

        std::cerr << "[ERROR] unknown option: " << argv[i] << "\n";
        return 1;
    }

    if (cfg.database.empty())
    {
        std::cerr << "[ERROR] -d/--database is required\n";
        return 1;
    }

    if (command != "diff" && command != "migrate" && command != "status" && command != "list-tables")
    {
        std::cerr << "[ERROR] unknown command: " << command << "\n";
        printHelp();
        return 1;
    }

    MigrationEngine engine(cfg);
    std::string err;
    if (!engine.connect(err))
    {
        std::cerr << "[ERROR] connect failed: " << err << "\n";
        return 1;
    }
    if (!engine.ensureVersionTable(err))
    {
        std::cerr << "[ERROR] ensure version table failed: " << err << "\n";
        return 1;
    }

    // ── diff ──
    if (command == "diff")
    {
        std::vector<dbmigrate::Change> changes;
        if (!engine.plan(changes, err))
        {
            std::cerr << "[ERROR] " << err << "\n";
            return 1;
        }

        if (changes.empty())
        {
            std::cout << "schema up to date: no changes\n";
            return 0;
        }

        std::cout << "-- pending changes: " << changes.size() << "\n";
        for (const auto& c : changes)
            std::cout << "-- " << c.describe << (c.destructive ? "  [DESTRUCTIVE]" : "") << "\n"
                      << c.sql << "\n";
        return 2;
    }

    // ── migrate ──
    if (command == "migrate")
    {
        std::vector<dbmigrate::Change> changes;
        if (!engine.plan(changes, err))
        {
            std::cerr << "[ERROR] " << err << "\n";
            return 1;
        }

        if (changes.empty())
        {
            std::cout << "schema up to date: nothing to migrate\n";
            return 0;
        }

        std::cout << "applying " << changes.size() << " change(s):\n";
        if (!engine.apply(changes, err))
        {
            std::cerr << "[ERROR] " << err << "\n";
            return 1;
        }
        std::cout << "migration done\n";
        return 0;
    }

    // ── status ──
    if (command == "status")
    {
        std::vector<AppliedMigration> applied;
        if (!engine.history(applied, err))
        {
            std::cerr << "[ERROR] " << err << "\n";
            return 1;
        }

        std::cout << "applied: " << applied.size() << "\n";
        for (const auto& a : applied)
        {
            std::cout << "  V" << a.version << "  " << a.applied_at << "  " << a.description
                      << "  [" << a.filename << "]\n";
        }

        std::vector<dbmigrate::Change> changes;
        if (!engine.plan(changes, err))
        {
            std::cerr << "[ERROR] " << err << "\n";
            return 1;
        }
        std::cout << "pending: " << changes.size() << "\n";
        for (const auto& c : changes)
            std::cout << "  - " << c.describe << (c.destructive ? "  [DESTRUCTIVE]" : "") << "\n";
        return 0;
    }

    // ── list-tables ──
    std::vector<std::string> tables;
    if (!engine.listTables(tables, err))
    {
        std::cerr << "[ERROR] " << err << "\n";
        return 1;
    }
    for (const auto& t : tables) std::cout << t << "\n";
    return 0;
}
