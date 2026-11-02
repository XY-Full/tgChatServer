#include "MySQLConnection.h"
#include "Log.h"
#include <algorithm>
#include <cstring>

constexpr size_t kMaxCachedStatements = 64;
constexpr unsigned long kMaxColumnBuffer = 4 * 1024 * 1024;


std::atomic<int64_t> MySQLConnection::s_violations{0};

MySQLConnection::MySQLConnection(const MySQLConfig& config)
    : config_(config)
{
    mysql_init(&mysql_);
    // 设置连接超时
    unsigned int timeout_sec = (config_.connect_timeout_ms + 999) / 1000;
    mysql_options(&mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);
    // 不启用自动重连：异步执行期间若底层悄悄重连，缓存的预处理语句会全部失效
    bool reconnect_flag = false;
    mysql_options(&mysql_, MYSQL_OPT_RECONNECT, &reconnect_flag);

    if (config_.read_timeout_ms > 0)
    {
        unsigned int t = static_cast<unsigned int>(config_.read_timeout_ms / 1000);
        if (t == 0) t = 1;
        mysql_options(&mysql_, MYSQL_OPT_READ_TIMEOUT, &t);
    }
    if (config_.write_timeout_ms > 0)
    {
        unsigned int t = static_cast<unsigned int>(config_.write_timeout_ms / 1000);
        if (t == 0) t = 1;
        mysql_options(&mysql_, MYSQL_OPT_WRITE_TIMEOUT, &t);
    }
}

MySQLConnection::~MySQLConnection()
{
    close();
}

bool MySQLConnection::connect()
{
    if (connected_) return true;

    // 指定认证插件的查找目录
    mysql_options(&mysql_, MYSQL_PLUGIN_DIR, MARIADB_PLUGIN_DIR);

    if (config_.use_ssl)
    {
        mysql_ssl_set(&mysql_, nullptr, nullptr,
                      config_.ssl_ca.empty() ? nullptr : config_.ssl_ca.c_str(),
                      nullptr, nullptr);
        if (!config_.ssl_verify_server_cert)
        {
            my_bool verify = 0;
            mysql_options(&mysql_, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verify);
        }
    }

    MYSQL* result = mysql_real_connect(
        &mysql_,
        config_.host.c_str(),
        config_.user.c_str(),
        config_.password.c_str(),
        config_.database.c_str(),
        config_.port,
        nullptr,  // unix socket
        0         // client flags
    );

    if (!result)
    {
        ELOG << "MySQLConnection: connect failed: " << mysql_error(&mysql_)
             << " (host=" << config_.host << ":" << config_.port << ")";
        return false;
    }

    // 设置字符集为 utf8mb4
    mysql_set_character_set(&mysql_, "utf8mb4");

    // 打开非阻塞能力，必须在连接建立之后设置
    size_t async_stack_size = 0;
    if (mysql_options(&mysql_, MYSQL_OPT_NONBLOCK, &async_stack_size) != 0)
    {
        ELOG << "MySQLConnection: failed to enable non-blocking mode";
        return false;
    }

    connected_ = true;
    ILOG << "MySQLConnection: connected to " << config_.host << ":" << config_.port
         << " db=" << config_.database;
    return true;
}

bool MySQLConnection::reconnect()
{
    close();
    mysql_init(&mysql_);
    unsigned int timeout_sec = (config_.connect_timeout_ms + 999) / 1000;
    mysql_options(&mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);
    bool reconnect_flag = true;
    mysql_options(&mysql_, MYSQL_OPT_RECONNECT, &reconnect_flag);
    return connect();
}

bool MySQLConnection::ping()
{
    if (!connected_) return false;
    if (mysql_ping(&mysql_) == 0) return true;
    // ping 失败，连接已断开
    connected_ = false;
    ELOG << "MySQLConnection: ping failed, connection lost";
    return false;
}

void MySQLConnection::close()
{
    clearStatementCache();
    if (connected_)
    {
        mysql_close(&mysql_);
        connected_ = false;
    }
}

bool MySQLConnection::execRaw(const std::string& sql, std::string& err_msg)
{
    if (mysql_real_query(&mysql_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
    {
        err_msg = mysql_error(&mysql_);
        return false;
    }
    return true;
}

bool MySQLConnection::query(const std::string& sql, const ParamList& params, QueryResult& result,
                            std::string& err_msg)
{
    if (!connected_)
    {
        err_msg = "not connected";
        return false;
    }

    MYSQL_STMT* stmt = prepareStatement(sql, err_msg);
    if (!stmt)
    {
        ELOG << "MySQLConnection::query prepare failed: " << err_msg << " sql=" << sql;
        return false;
    }

    std::vector<MYSQL_BIND>    binds;
    std::vector<unsigned long> lengths;
    std::vector<my_bool>       is_null;
    if (!bindStmtParams(stmt, params, binds, lengths, is_null, err_msg))
    {
        ELOG << "MySQLConnection::query bind failed: " << err_msg << " sql=" << sql;
        return false;
    }

    if (mysql_stmt_execute(stmt) != 0)
    {
        err_msg = mysql_stmt_error(stmt);
        ELOG << "MySQLConnection::query execute failed: " << err_msg << " sql=" << sql;
        return false;
    }

    MYSQL_RES* meta = mysql_stmt_result_metadata(stmt);
    if (!meta)
    {
        err_msg = "statement returns no result set";
        ELOG << "MySQLConnection::query " << err_msg << " sql=" << sql;
        return false;
    }

    const unsigned int num_fields = mysql_num_fields(meta);
    MYSQL_FIELD*       fields     = mysql_fetch_fields(meta);

    // 结果缓冲区按列的最大长度一次性分配，之后逐行复用
    std::vector<std::vector<char>> buffers(num_fields);
    std::vector<MYSQL_BIND>        result_binds(num_fields);
    std::vector<unsigned long>     result_lengths(num_fields, 0);
    std::vector<my_bool>           result_null(num_fields, 0);

    for (unsigned int i = 0; i < num_fields; ++i)
    {
        result.column_names.emplace_back(fields[i].name);
        result.column_types.emplace_back(
            std::to_string(fields[i].type)); // 使用 enum_field_types 数值

        unsigned long cap = fields[i].length + 1;
        if (cap > kMaxColumnBuffer) cap = kMaxColumnBuffer;
        buffers[i].resize(cap);

        MYSQL_BIND& b = result_binds[i];
        std::memset(&b, 0, sizeof(b));
        b.buffer_type   = MYSQL_TYPE_STRING; // 服务端统一按字符串返回，避免类型差异
        b.buffer        = buffers[i].data();
        b.buffer_length = cap;
        b.length        = &result_lengths[i];
        b.is_null       = &result_null[i];
    }

    bool ok = true;
    if (num_fields > 0 && mysql_stmt_bind_result(stmt, result_binds.data()) != 0)
    {
        err_msg = mysql_stmt_error(stmt);
        ok      = false;
    }
    else if (mysql_stmt_store_result(stmt) != 0)
    {
        err_msg = mysql_stmt_error(stmt);
        ok      = false;
    }

    while (ok)
    {
        int rc = mysql_stmt_fetch(stmt);
        if (rc == MYSQL_NO_DATA) break;
        if (rc != 0 && rc != MYSQL_DATA_TRUNCATED)
        {
            err_msg = mysql_stmt_error(stmt);
            ok      = false;
            break;
        }

        std::vector<std::string> row_data(num_fields);
        std::vector<bool>        null_data(num_fields, false);
        for (unsigned int i = 0; i < num_fields; ++i)
        {
            if (result_null[i])
            {
                null_data[i] = true;
                continue;
            }
            const unsigned long len = std::min(result_lengths[i], static_cast<unsigned long>(buffers[i].size()));
            row_data[i].assign(buffers[i].data(), len);
        }
        result.rows.push_back(std::move(row_data));
        result.null_flags.push_back(std::move(null_data));
    }

    mysql_stmt_free_result(stmt);
    mysql_free_result(meta);

    if (!ok)
    {
        ELOG << "MySQLConnection::query failed: " << err_msg << " sql=" << sql;
        return false;
    }
    return true;
}

bool MySQLConnection::execute(const std::string& sql, const ParamList& params, ExecResult& result,
                              std::string& err_msg)
{
    if (!connected_)
    {
        err_msg = "not connected";
        return false;
    }

    MYSQL_STMT* stmt = prepareStatement(sql, err_msg);
    if (!stmt)
    {
        ELOG << "MySQLConnection::execute prepare failed: " << err_msg << " sql=" << sql;
        return false;
    }

    std::vector<MYSQL_BIND>    binds;
    std::vector<unsigned long> lengths;
    std::vector<my_bool>       is_null;
    if (!bindStmtParams(stmt, params, binds, lengths, is_null, err_msg))
    {
        ELOG << "MySQLConnection::execute bind failed: " << err_msg << " sql=" << sql;
        return false;
    }

    if (mysql_stmt_execute(stmt) != 0)
    {
        err_msg = mysql_stmt_error(stmt);
        ELOG << "MySQLConnection::execute failed: " << err_msg << " sql=" << sql;
        return false;
    }

    result.affected_rows  = static_cast<uint64_t>(mysql_stmt_affected_rows(stmt));
    result.last_insert_id = static_cast<uint64_t>(mysql_stmt_insert_id(stmt));
    return true;
}

MYSQL_STMT* MySQLConnection::prepareStatement(const std::string& sql, std::string& err_msg)
{
    auto it = stmt_cache_.find(sql);
    if (it != stmt_cache_.end()) return it->second;

    if (stmt_cache_.size() >= kMaxCachedStatements) clearStatementCache();

    MYSQL_STMT* stmt = mysql_stmt_init(&mysql_);
    if (!stmt)
    {
        err_msg = "mysql_stmt_init failed";
        return nullptr;
    }

    if (mysql_stmt_prepare(stmt, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
    {
        err_msg = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        return nullptr;
    }

    stmt_cache_.emplace(sql, stmt);
    return stmt;
}

void MySQLConnection::clearStatementCache()
{
    for (auto& kv : stmt_cache_) mysql_stmt_close(kv.second);
    stmt_cache_.clear();
}

bool MySQLConnection::beginTransaction(std::string& err_msg)
{
    return execRaw("START TRANSACTION", err_msg);
}

bool MySQLConnection::commit(std::string& err_msg)
{
    return execRaw("COMMIT", err_msg);
}

bool MySQLConnection::rollback(std::string& err_msg)
{
    return execRaw("ROLLBACK", err_msg);
}
