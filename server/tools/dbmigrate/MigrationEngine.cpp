#include "MigrationEngine.h"

#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <openssl/evp.h>
#include <sstream>

// ── 版本表 DDL ────────────────────────────────────────────────────────────
static constexpr const char* kCreateVersionTable = R"(
CREATE TABLE IF NOT EXISTS db_schema_version (
    version      INT          NOT NULL,
    description  VARCHAR(256) NOT NULL,
    filename     VARCHAR(256) NOT NULL,
    checksum     VARCHAR(64)  NOT NULL,
    applied_at   DATETIME     NOT NULL DEFAULT CURRENT_TIMESTAMP,
    execution_ms INT          NOT NULL DEFAULT 0,
    PRIMARY KEY (version)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='DB schema migration history';
)";

// ── SHA-256 ───────────────────────────────────────────────────────────────
static std::string sha256String(const std::string& text)
{
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, text.data(), text.size());

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;
    EVP_DigestFinal_ex(ctx, digest, &digest_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    for (unsigned int i = 0; i < digest_len; ++i)
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
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

bool MigrationEngine::connect(std::string& err)
{
    if (connected_) return true;

    unsigned int timeout = 10;
    mysql_options(&mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    MYSQL* r = mysql_real_connect(&mysql_,
                                  config_.host.c_str(),
                                  config_.user.c_str(),
                                  config_.password.c_str(),
                                  config_.database.c_str(),
                                  config_.port,
                                  nullptr, 0);
    if (!r)
    {
        err = mysql_error(&mysql_);
        return false;
    }

    mysql_set_character_set(&mysql_, "utf8mb4");
    mysql_autocommit(&mysql_, 1);
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

// ── SQL 执行 ──────────────────────────────────────────────────────────────
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
    std::string buf(s.size() * 2 + 1, '\0');
    unsigned long n = mysql_real_escape_string(&mysql_, &buf[0], s.c_str(),
                                               static_cast<unsigned long>(s.size()));
    buf.resize(n);
    return buf;
}

bool MigrationEngine::ensureVersionTable(std::string& err)
{
    return execSQL(kCreateVersionTable, err);
}

// ── 计划 ──────────────────────────────────────────────────────────────────
bool MigrationEngine::plan(std::vector<dbmigrate::Change>& changes, std::string& err)
{
    dbmigrate::SchemaDiffer differ(&mysql_, config_.database);
    if (!differ.loadLive(err)) return false;
    changes = differ.diff();
    return true;
}

int MigrationEngine::nextVersion()
{
    const char* sql = "SELECT COALESCE(MAX(version), 0) FROM db_schema_version";
    if (mysql_real_query(&mysql_, sql, std::strlen(sql)) != 0) return 1;

    MYSQL_RES* res = mysql_store_result(&mysql_);
    if (!res) return 1;

    int v = 1;
    MYSQL_ROW row = mysql_fetch_row(res);
    if (row && row[0]) v = std::stoi(row[0]) + 1;
    mysql_free_result(res);
    return v;
}

bool MigrationEngine::recordBatch(const std::vector<dbmigrate::Change>& changes, int64_t elapsed_ms,
                                  std::string& err)
{
    std::string all_sql;
    for (const auto& c : changes) all_sql += c.sql + "\n";

    std::string describe;
    for (size_t i = 0; i < changes.size(); ++i)
        describe += (i ? ", " : "") + changes[i].describe;
    if (describe.size() > 200) describe = describe.substr(0, 200) + " ...";

    std::ostringstream ss;
    ss << "INSERT INTO db_schema_version (version, description, filename, checksum, execution_ms) VALUES ("
       << nextVersion() << ", '"
       << escapeSQL(describe) << "', '"
       << escapeSQL("proto:public/db_proto") << "', '"
       << escapeSQL(sha256String(all_sql)) << "', "
       << elapsed_ms << ")";

    return execSQL(ss.str(), err);
}

// ── 应用 ──────────────────────────────────────────────────────────────────
bool MigrationEngine::apply(const std::vector<dbmigrate::Change>& changes, std::string& err)
{
    if (changes.empty()) return true;

    for (const auto& c : changes)
    {
        if (c.destructive && !config_.allow_destructive)
        {
            err = "destructive change requires --allow-destructive: " + c.describe;
            return false;
        }
    }

    const auto t0 = std::chrono::steady_clock::now();

    for (const auto& c : changes)
    {
        std::cout << "  -> " << c.describe << (c.destructive ? "  [DESTRUCTIVE]" : "") << "\n";
        if (config_.verbose) std::cout << "     " << c.sql << "\n";
        if (!execSQL(c.sql, err)) return false;
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();

    return recordBatch(changes, static_cast<int64_t>(ms), err);
}

// ── 历史 / 表列表 ─────────────────────────────────────────────────────────
bool MigrationEngine::history(std::vector<AppliedMigration>& out, std::string& err)
{
    const char* sql = "SELECT version, description, filename, checksum, applied_at "
                      "FROM db_schema_version ORDER BY version ASC";
    if (mysql_real_query(&mysql_, sql, std::strlen(sql)) != 0)
    {
        err = mysql_error(&mysql_);
        return false;
    }

    MYSQL_RES* res = mysql_store_result(&mysql_);
    if (!res) return true;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr)
    {
        AppliedMigration am;
        am.version     = row[0] ? std::stoi(row[0]) : 0;
        am.description = row[1] ? row[1] : "";
        am.filename    = row[2] ? row[2] : "";
        am.checksum    = row[3] ? row[3] : "";
        am.applied_at  = row[4] ? row[4] : "";
        out.push_back(am);
    }
    mysql_free_result(res);
    return true;
}

bool MigrationEngine::listTables(std::vector<std::string>& out, std::string& err)
{
    const char* sql = "SHOW TABLES";
    if (mysql_real_query(&mysql_, sql, std::strlen(sql)) != 0)
    {
        err = mysql_error(&mysql_);
        return false;
    }

    MYSQL_RES* res = mysql_store_result(&mysql_);
    if (!res) return true;

    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr)
    {
        if (row[0] && std::string(row[0]) != "db_schema_version") out.push_back(row[0]);
    }
    mysql_free_result(res);
    return true;
}
