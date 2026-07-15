#include "MigrationEngine.h"
#include "SQLGenerator.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <openssl/evp.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace fs = std::filesystem;

// ── 版本表 DDL ────────────────────────────────────────────────────────────
static constexpr const char* kCreateVersionTable = R"(
CREATE TABLE IF NOT EXISTS `db_schema_version` (
    `version`      INT          NOT NULL,
    `description`  VARCHAR(256) NOT NULL,
    `filename`     VARCHAR(256) NOT NULL,
    `checksum`     VARCHAR(64)  NOT NULL,
    `applied_at`   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    `execution_ms` INT          NOT NULL DEFAULT 0,
    PRIMARY KEY (`version`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='DB schema migration history';
)";

// ── SHA-256 文件摘要 ──────────────────────────────────────────────────────
static std::string sha256File(const std::string& filepath)
{
    std::ifstream f(filepath, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open file: " + filepath);

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);

    char buf[8192];
    while (f.read(buf, sizeof(buf)) || f.gcount() > 0)
        EVP_DigestUpdate(ctx, buf, static_cast<size_t>(f.gcount()));

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    for (unsigned int i = 0; i < digest_len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(digest[i]);
    return ss.str();
}

// ── 构造 / 析构 ──────────────────────────────────────────────────────────

MigrationEngine::MigrationEngine(const MigrationConfig& config)
    : config_(config)
{
    mysql_init(&mysql_);
}

MigrationEngine::~MigrationEngine()
{
    disconnect();
}

bool MigrationEngine::connect()
{
    if (connected_) return true;

    unsigned int timeout = 10;
    mysql_options(&mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    MYSQL* r = mysql_real_connect(
        &mysql_,
        config_.host.c_str(),
        config_.user.c_str(),
        config_.password.c_str(),
        config_.database.c_str(),
        config_.port,
        nullptr, 0);

    if (!r)
    {
        std::cerr << "[ERROR] MySQL connect failed: " << mysql_error(&mysql_) << "\n";
        return false;
    }
    mysql_set_character_set(&mysql_, "utf8mb4");
    mysql_autocommit(&mysql_, 0);
    connected_ = true;
    return true;
}

void MigrationEngine::disconnect()
{
    if (connected_)
    {
        mysql_close(&mysql_);
        connected_ = false;
    }
}

// ── 执行单条 SQL ──────────────────────────────────────────────────────────

bool MigrationEngine::execSQL(const std::string& sql, std::string& err)
{
    if (config_.dry_run)
    {
        std::cout << "[DRY-RUN] " << sql << "\n";
        return true;
    }
    if (mysql_real_query(&mysql_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
    {
        err = mysql_error(&mysql_);
        return false;
    }
    MYSQL_RES* res = mysql_store_result(&mysql_);
    if (res) mysql_free_result(res);
    return true;
}

std::string MigrationEngine::escapeSQL(const std::string& s)
{
    // 最坏情况每个字符都被转义为两个字符
    std::vector<char> buf(s.size() * 2 + 1);
    unsigned long n = mysql_real_escape_string(&mysql_, buf.data(), s.c_str(),
                                               static_cast<unsigned long>(s.size()));
    return std::string(buf.data(), n);
}

// ── 确保版本表存在 ────────────────────────────────────────────────────────

bool MigrationEngine::ensureVersionTable()
{
    std::string err;
    mysql_autocommit(&mysql_, 1);
    bool ok = execSQL(kCreateVersionTable, err);
    mysql_autocommit(&mysql_, 0);
    if (!ok)
        std::cerr << "[ERROR] Failed to create db_schema_version: " << err << "\n";
    return ok;
}

// ── 读取已应用的迁移记录 ──────────────────────────────────────────────────

std::vector<AppliedMigration> MigrationEngine::loadAppliedMigrations()
{
    std::vector<AppliedMigration> result;
    const char* sql = "SELECT version, description, filename, checksum "
                      "FROM db_schema_version ORDER BY version ASC";
    if (mysql_query(&mysql_, sql) != 0) return result;

    MYSQL_RES* res = mysql_store_result(&mysql_);
    if (!res) return result;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
    {
        AppliedMigration am;
        am.version     = std::stoi(row[0] ? row[0] : "0");
        am.description = row[1] ? row[1] : "";
        am.filename    = row[2] ? row[2] : "";
        am.checksum    = row[3] ? row[3] : "";
        result.push_back(am);
    }
    mysql_free_result(res);
    return result;
}

// ── 获取用户表列表（排除版本表） ──────────────────────────────────────────

std::vector<std::string> MigrationEngine::getUserTables()
{
    std::vector<std::string> tables;
    if (mysql_query(&mysql_, "SHOW TABLES") != 0) return tables;

    MYSQL_RES* res = mysql_store_result(&mysql_);
    if (!res) return tables;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)))
    {
        if (row[0] && std::string(row[0]) != "db_schema_version")
            tables.push_back(row[0]);
    }
    mysql_free_result(res);
    return tables;
}

// ── 扫描迁移目录（匹配 .xml 文件） ───────────────────────────────────────

std::vector<MigrationFile> MigrationEngine::scanMigrationFiles()
{
    std::vector<MigrationFile> files;
    static const std::regex re(R"(^V(\d+)__.+\.xml$)", std::regex::icase);

    if (!fs::exists(config_.migrations_dir))
        throw std::runtime_error("migrations dir not found: " + config_.migrations_dir);

    for (const auto& entry : fs::directory_iterator(config_.migrations_dir))
    {
        if (!entry.is_regular_file()) continue;
        std::string fname = entry.path().filename().string();
        if (!std::regex_match(fname, re)) continue;
        files.push_back(parseMigrationFile(entry.path().string()));
    }

    std::sort(files.begin(), files.end(),
              [](const MigrationFile& a, const MigrationFile& b) {
                  return a.version < b.version;
              });
    return files;
}

std::string MigrationEngine::computeChecksum(const std::string& filepath)
{
    return sha256File(filepath);
}

// ── 应用单个迁移 ──────────────────────────────────────────────────────────

bool MigrationEngine::applyMigration(const MigrationFile& mf)
{
    std::cout << "Applying V" << std::setw(3) << std::setfill('0') << mf.version
              << " " << mf.description << " ...\n";

    auto t0 = std::chrono::steady_clock::now();

    std::string err;
    if (!execSQL("START TRANSACTION", err))
    {
        std::cerr << "[ERROR] START TRANSACTION: " << err << "\n";
        return false;
    }

    for (const auto& op : mf.operations)
    {
        std::string sql = operationToSQL(op);
        if (sql.empty()) continue;

        if (config_.verbose)
            std::cout << "  SQL: " << sql << "\n";

        std::istringstream iss(sql);
        std::string stmt;
        while (std::getline(iss, stmt, ';'))
        {
            auto start = stmt.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            stmt = stmt.substr(start);
            auto end = stmt.find_last_not_of(" \t\r\n");
            if (end != std::string::npos) stmt = stmt.substr(0, end + 1);
            if (stmt.empty()) continue;

            if (!execSQL(stmt, err))
            {
                std::cerr << "[ERROR] SQL failed: " << err << "\n"
                          << "  Statement: " << stmt << "\n";
                if (!config_.dry_run) mysql_rollback(&mysql_);
                return false;
            }
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    int  ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    std::string cksum = computeChecksum(mf.filepath);

    // description/filename 来自迁移 XML，必须转义后再拼接
    std::ostringstream insert_ss;
    insert_ss << "INSERT INTO db_schema_version "
              << "(version, description, filename, checksum, execution_ms) VALUES ("
              << mf.version << ", '"
              << escapeSQL(mf.description) << "', '"
              << escapeSQL(mf.filename) << "', '"
              << escapeSQL(cksum) << "', "
              << ms << ")";

    if (!execSQL(insert_ss.str(), err))
    {
        std::cerr << "[ERROR] Failed to record migration: " << err << "\n";
        if (!config_.dry_run) mysql_rollback(&mysql_);
        return false;
    }

    if (!config_.dry_run && mysql_commit(&mysql_) != 0)
    {
        std::cerr << "[ERROR] COMMIT failed: " << mysql_error(&mysql_) << "\n";
        mysql_rollback(&mysql_);
        return false;
    }

    std::cout << "  OK (" << ms << "ms)\n";
    return true;
}

// ── 公开接口 ──────────────────────────────────────────────────────────────

bool MigrationEngine::migrate()
{
    if (!connect()) return false;
    if (!ensureVersionTable()) return false;

    auto applied = loadAppliedMigrations();
    auto files   = scanMigrationFiles();

    std::unordered_map<int, AppliedMigration> applied_map;
    for (const auto& a : applied)
        applied_map[a.version] = a;

    for (const auto& a : applied)
    {
        bool found = false;
        for (const auto& f : files)
        {
            if (f.version == a.version)
            {
                found = true;
                std::string cksum = computeChecksum(f.filepath);
                if (cksum != a.checksum)
                {
                    std::cerr << "[ERROR] Checksum mismatch for V" << a.version
                              << " (" << a.filename << ")!\n"
                              << "  Applied: " << a.checksum << "\n"
                              << "  File:    " << cksum << "\n";
                    return false;
                }
                break;
            }
        }
        if (!found)
        {
            std::cerr << "[ERROR] Applied migration V" << a.version
                      << " (" << a.filename << ") file not found!\n";
            return false;
        }
    }

    int applied_count = 0;
    for (const auto& mf : files)
    {
        if (applied_map.count(mf.version)) continue;
        if (!applyMigration(mf)) return false;
        ++applied_count;
    }

    if (applied_count == 0)
        std::cout << "Nothing to migrate (all up-to-date).\n";
    else
        std::cout << "Migrated " << applied_count << " file(s).\n";

    return true;
}

void MigrationEngine::status()
{
    if (!connect()) return;
    if (!ensureVersionTable()) return;

    auto applied = loadAppliedMigrations();
    auto files   = scanMigrationFiles();

    std::unordered_map<int, AppliedMigration> applied_map;
    for (const auto& a : applied)
        applied_map[a.version] = a;

    std::cout << std::left
              << std::setw(6)  << "Ver"
              << std::setw(40) << "Description"
              << std::setw(12) << "Status"
              << "Checksum\n";
    std::cout << std::string(72, '-') << "\n";

    for (const auto& mf : files)
    {
        std::cout << std::left
                  << std::setw(6)  << ("V" + std::to_string(mf.version))
                  << std::setw(40) << mf.description;
        if (applied_map.count(mf.version))
            std::cout << std::setw(12) << "Applied"
                      << applied_map[mf.version].checksum.substr(0, 8) + "...\n";
        else
            std::cout << std::setw(12) << "Pending" << "-\n";
    }

    for (const auto& a : applied)
    {
        bool found = std::any_of(files.begin(), files.end(),
                                 [&](const MigrationFile& f) { return f.version == a.version; });
        if (!found)
            std::cout << "V" << a.version << " [ORPHAN - file missing] " << a.filename << "\n";
    }
}

// ── create ────────────────────────────────────────────────────────────────

void MigrationEngine::createMigrationFile(const std::string& dir, const std::string& name)
{
    int max_version = 0;
    static const std::regex re(R"(^V(\d+)__.+\.xml$)", std::regex::icase);
    if (fs::exists(dir))
    {
        for (const auto& entry : fs::directory_iterator(dir))
        {
            std::string fname = entry.path().filename().string();
            std::smatch m;
            if (std::regex_match(fname, m, re))
                max_version = std::max(max_version, std::stoi(m[1].str()));
        }
    }
    else
    {
        fs::create_directories(dir);
    }

    int next_version = max_version + 1;
    std::ostringstream fname;
    fname << "V" << std::setw(3) << std::setfill('0') << next_version
          << "__" << name << ".xml";

    std::string filepath = dir + "/" + fname.str();
    std::ofstream f(filepath);
    f << "<migration version=\"" << next_version << "\" description=\"" << name << "\">\n"
      << "\n"
      << "  <!-- create_table example:\n"
      << "  <create_table name=\"my_table\" engine=\"InnoDB\" charset=\"utf8mb4\" comment=\"\">\n"
      << "    <column name=\"id\"   type=\"BIGINT\"      not_null=\"1\" auto_increment=\"1\" primary_key=\"1\"/>\n"
      << "    <column name=\"name\" type=\"VARCHAR(64)\" not_null=\"1\" default=\"''\"/>\n"
      << "    <index  name=\"idx_name\" columns=\"name\"/>\n"
      << "  </create_table>\n"
      << "  -->\n"
      << "\n"
      << "</migration>\n";

    std::cout << "Created: " << filepath << "\n";
}

// ── import ────────────────────────────────────────────────────────────────
// 读取 .sql 文件，识别 CREATE TABLE 语句，生成迁移 XML 文件

void MigrationEngine::importSchema(const std::string& sql_file)
{
    std::ifstream f(sql_file);
    if (!f) { std::cerr << "[ERROR] Cannot open: " << sql_file << "\n"; return; }

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    // 提取所有 CREATE TABLE 语句中的表名
    static const std::regex re_table(
        R"(CREATE\s+TABLE\s+(?:IF\s+NOT\s+EXISTS\s+)?`?(\w+)`?)",
        std::regex::icase);

    std::sregex_iterator it(content.begin(), content.end(), re_table);
    std::sregex_iterator end;

    std::vector<std::string> tables;
    for (; it != end; ++it)
        tables.push_back((*it)[1].str());

    if (tables.empty())
    {
        std::cerr << "[WARN] No CREATE TABLE found in: " << sql_file << "\n";
        return;
    }

    // 确定下一个版本号
    int max_version = 0;
    static const std::regex re_ver(R"(^V(\d+)__.+\.xml$)", std::regex::icase);
    if (fs::exists(config_.migrations_dir))
    {
        for (const auto& entry : fs::directory_iterator(config_.migrations_dir))
        {
            std::string fname = entry.path().filename().string();
            std::smatch m;
            if (std::regex_match(fname, m, re_ver))
                max_version = std::max(max_version, std::stoi(m[1].str()));
        }
    }
    else
    {
        fs::create_directories(config_.migrations_dir);
    }

    int next = max_version + 1;
    std::ostringstream fname;
    fname << "V" << std::setw(3) << std::setfill('0') << next
          << "__import_from_sql.xml";

    std::string filepath = config_.migrations_dir + "/" + fname.str();
    std::ofstream out(filepath);
    out << "<migration version=\"" << next << "\" description=\"import from sql\">\n\n";

    for (const auto& tbl : tables)
    {
        std::cout << "  Found table: " << tbl << "\n";
        out << "  <!-- imported table: " << tbl << " -->\n"
            << "  <create_table name=\"" << tbl << "\" engine=\"InnoDB\" charset=\"utf8mb4\">\n"
            << "    <!-- TODO: fill in column definitions -->\n"
            << "    <column name=\"id\" type=\"BIGINT\" not_null=\"1\" auto_increment=\"1\" primary_key=\"1\"/>\n"
            << "  </create_table>\n\n";
    }

    out << "</migration>\n";
    std::cout << "Generated: " << filepath
              << " (" << tables.size() << " table(s) — fill in column details)\n";
}

// ── list-tables ───────────────────────────────────────────────────────────

void MigrationEngine::listTables()
{
    if (!connect()) return;

    auto tables = getUserTables();
    if (tables.empty())
    {
        std::cout << "(no user tables)\n";
        return;
    }
    std::cout << "Tables in `" << config_.database << "`:\n";
    for (const auto& t : tables)
        std::cout << "  " << t << "\n";
    std::cout << "Total: " << tables.size() << "\n";
}

// ── truncate-all ──────────────────────────────────────────────────────────

bool MigrationEngine::truncateAll()
{
    if (!connect()) return false;

    auto tables = getUserTables();
    if (tables.empty())
    {
        std::cout << "No user tables to truncate.\n";
        return true;
    }

    std::string err;
    // 临时禁用外键检查
    mysql_autocommit(&mysql_, 1);
    execSQL("SET FOREIGN_KEY_CHECKS = 0", err);

    int count = 0;
    for (const auto& t : tables)
    {
        if (execSQL("TRUNCATE TABLE `" + t + "`", err))
        {
            std::cout << "  Truncated: " << t << "\n";
            ++count;
        }
        else
        {
            std::cerr << "[ERROR] TRUNCATE " << t << ": " << err << "\n";
        }
    }

    execSQL("SET FOREIGN_KEY_CHECKS = 1", err);
    mysql_autocommit(&mysql_, 0);

    std::cout << "Truncated " << count << "/" << tables.size() << " table(s).\n";
    return count == static_cast<int>(tables.size());
}

// ── rebuild ───────────────────────────────────────────────────────────────

bool MigrationEngine::rebuild(const std::string& sql_file)
{
    if (!connect()) return false;

    std::ifstream f(sql_file);
    if (!f)
    {
        std::cerr << "[ERROR] Cannot open: " << sql_file << "\n";
        return false;
    }

    // 读取 SQL 文件全文
    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());

    auto tables = getUserTables();
    std::string err;

    // 1. 删除所有用户表
    mysql_autocommit(&mysql_, 1);
    execSQL("SET FOREIGN_KEY_CHECKS = 0", err);
    for (const auto& t : tables)
    {
        if (execSQL("DROP TABLE IF EXISTS `" + t + "`", err))
            std::cout << "  Dropped: " << t << "\n";
        else
            std::cerr << "[ERROR] DROP " << t << ": " << err << "\n";
    }
    // 也清理版本表
    execSQL("DROP TABLE IF EXISTS `db_schema_version`", err);
    execSQL("SET FOREIGN_KEY_CHECKS = 1", err);

    std::cout << "Dropped " << tables.size() << " table(s). Rebuilding...\n";

    // 2. 按 ';' 拆分并执行所有语句
    std::istringstream iss(content);
    std::string stmt;
    int ok_count = 0, fail_count = 0;
    while (std::getline(iss, stmt, ';'))
    {
        auto start = stmt.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) continue;
        stmt = stmt.substr(start);
        auto end = stmt.find_last_not_of(" \t\r\n");
        if (end != std::string::npos) stmt = stmt.substr(0, end + 1);
        if (stmt.empty() || stmt[0] == '-') continue; // skip comments

        if (execSQL(stmt, err))
            ++ok_count;
        else
        {
            std::cerr << "[ERROR] " << err << "\n  SQL: " << stmt.substr(0, 80) << "...\n";
            ++fail_count;
        }
    }

    mysql_autocommit(&mysql_, 0);
    std::cout << "Rebuild complete: " << ok_count << " OK, " << fail_count << " failed.\n";
    return fail_count == 0;
}
