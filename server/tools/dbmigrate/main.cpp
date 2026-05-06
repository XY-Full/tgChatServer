#include "MigrationEngine.h"
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

static void printHelp()
{
    std::cout << R"(
dbmigrate — MySQL schema migration tool

Usage:
  dbmigrate <command> [options]

Commands:
  migrate       Execute all pending XML migrations
  status        Show migration history
  validate      Validate migration XML files (no DB connection)
  create        Create a new migration file skeleton
  import        Read a .sql file and generate a migration XML skeleton
  list-tables   List all user tables in the database
  truncate-all  Truncate all user table data (keep structure) [--confirm required]
  rebuild       Drop all tables and rebuild from a .sql file

Options:
  -H, --host       MySQL host       (default: 127.0.0.1)
  -P, --port       MySQL port       (default: 3306)
  -u, --user       MySQL user       (default: root)
  -p, --password   MySQL password   (default: empty)
  -d, --database   Target database  (required for most commands)
  -D, --dir        Migrations dir   (default: ./db/migrations)
  -f, --file       SQL file         (required for import/rebuild)
  -n, --name       Migration name   (required for create)
      --dry-run    Print SQL without executing
      --confirm    Confirm destructive operations (truncate-all)
  -v, --verbose    Verbose output

Examples:
  dbmigrate migrate      -H 127.0.0.1 -d mydb -D ./db/migrations
  dbmigrate status       -d mydb
  dbmigrate validate     -D ./db/migrations
  dbmigrate create       -D ./db/migrations -n add_users_table
  dbmigrate import       -f schema.sql -D ./db/migrations
  dbmigrate list-tables  -d mydb
  dbmigrate truncate-all -d mydb --confirm
  dbmigrate rebuild      -d mydb -f schema.sql
)";
}

static bool parseArg(int argc, char* argv[], int& i,
                     const char* short_flag, const char* long_flag,
                     std::string& out)
{
    if ((strcmp(argv[i], short_flag) == 0 || strcmp(argv[i], long_flag) == 0) && i + 1 < argc)
    {
        out = argv[++i];
        return true;
    }
    std::string arg   = argv[i];
    std::string prefix = std::string(long_flag) + "=";
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

    std::string command = argv[1];
    if (command == "--help" || command == "-h") { printHelp(); return 0; }

    MigrationConfig cfg;
    std::string migration_name;
    std::string sql_file;
    bool        confirm = false;

    for (int i = 2; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (parseArg(argc, argv, i, "-H", "--host",     cfg.host))           continue;
        if (parseArg(argc, argv, i, "-u", "--user",     cfg.user))           continue;
        if (parseArg(argc, argv, i, "-p", "--password", cfg.password))       continue;
        if (parseArg(argc, argv, i, "-d", "--database", cfg.database))       continue;
        if (parseArg(argc, argv, i, "-D", "--dir",      cfg.migrations_dir)) continue;
        if (parseArg(argc, argv, i, "-n", "--name",     migration_name))     continue;
        if (parseArg(argc, argv, i, "-f", "--file",     sql_file))           continue;

        if (arg == "-P" || arg == "--port")
        {
            if (i + 1 < argc) cfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
            continue;
        }
        if (arg == "--dry-run") { cfg.dry_run = true; continue; }
        if (arg == "--confirm") { confirm = true;      continue; }
        if (arg == "-v" || arg == "--verbose") { cfg.verbose = true; continue; }

        std::cerr << "[WARN] Unknown option: " << arg << "\n";
    }

    // ── validate（无需数据库） ────────────────────────────────────────────
    if (command == "validate")
    {
        bool ok = true;
        if (!fs::exists(cfg.migrations_dir))
        {
            std::cerr << "[ERROR] Directory not found: " << cfg.migrations_dir << "\n";
            return 1;
        }
        for (const auto& entry : fs::directory_iterator(cfg.migrations_dir))
        {
            std::string fname = entry.path().filename().string();
            if (fname.size() < 4 || fname.substr(fname.size() - 4) != ".xml") continue;
            auto errors = validateMigrationFile(entry.path().string());
            if (errors.empty())
                std::cout << "  OK: " << fname << "\n";
            else
            {
                ok = false;
                std::cout << "FAIL: " << fname << "\n";
                for (const auto& e : errors)
                    std::cout << "      " << e << "\n";
            }
        }
        return ok ? 0 : 1;
    }

    // ── create（无需数据库） ──────────────────────────────────────────────
    if (command == "create")
    {
        if (migration_name.empty())
        {
            std::cerr << "[ERROR] --name is required for 'create'\n";
            return 1;
        }
        MigrationEngine::createMigrationFile(cfg.migrations_dir, migration_name);
        return 0;
    }

    // ── import（无需数据库连接，但需要 -f） ───────────────────────────────
    if (command == "import")
    {
        if (sql_file.empty())
        {
            std::cerr << "[ERROR] --file (-f) is required for 'import'\n";
            return 1;
        }
        MigrationEngine engine(cfg);
        engine.importSchema(sql_file);
        return 0;
    }

    // ── 需要数据库连接的命令 ──────────────────────────────────────────────
    if (cfg.database.empty())
    {
        std::cerr << "[ERROR] --database (-d) is required for '" << command << "'\n";
        return 1;
    }

    MigrationEngine engine(cfg);

    if (command == "migrate")
        return engine.migrate() ? 0 : 1;

    if (command == "status")
    {
        engine.status();
        return 0;
    }

    if (command == "list-tables")
    {
        engine.listTables();
        return 0;
    }

    if (command == "truncate-all")
    {
        if (!confirm)
        {
            std::cerr << "[ERROR] truncate-all requires --confirm (this will delete all data!)\n";
            return 1;
        }
        return engine.truncateAll() ? 0 : 1;
    }

    if (command == "rebuild")
    {
        if (sql_file.empty())
        {
            std::cerr << "[ERROR] --file (-f) is required for 'rebuild'\n";
            return 1;
        }
        return engine.rebuild(sql_file) ? 0 : 1;
    }

    std::cerr << "[ERROR] Unknown command: " << command << "\n";
    printHelp();
    return 1;
}
