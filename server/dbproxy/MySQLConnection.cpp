#include "MySQLConnection.h"
#include "Log.h"
#include <cstring>

MySQLConnection::MySQLConnection(const MySQLConfig& config)
    : config_(config)
{
    mysql_init(&mysql_);
    // 设置连接超时（秒，向上取整）
    unsigned int timeout_sec = (config_.connect_timeout_ms + 999) / 1000;
    mysql_options(&mysql_, MYSQL_OPT_CONNECT_TIMEOUT, &timeout_sec);
    // 断连后自动重连（mysql_ping 触发）
    bool reconnect_flag = true;
    mysql_options(&mysql_, MYSQL_OPT_RECONNECT, &reconnect_flag);
}

MySQLConnection::~MySQLConnection()
{
    close();
}

bool MySQLConnection::connect()
{
    if (connected_) return true;

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

bool MySQLConnection::query(const std::string& sql, QueryResult& result, std::string& err_msg)
{
    if (!connected_)
    {
        err_msg = "not connected";
        return false;
    }

    if (mysql_real_query(&mysql_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
    {
        err_msg = mysql_error(&mysql_);
        ELOG << "MySQLConnection::query failed: " << err_msg << " sql=" << sql;
        return false;
    }

    MYSQL_RES* res = mysql_store_result(&mysql_);
    if (!res)
    {
        // mysql_store_result 可能返回 nullptr（无结果集或出错）
        if (mysql_field_count(&mysql_) != 0)
        {
            err_msg = mysql_error(&mysql_);
            ELOG << "MySQLConnection::query store_result failed: " << err_msg;
            return false;
        }
        // 无结果集（如 SELECT 0 rows）正常
        return true;
    }

    // 填充列元数据
    unsigned int num_fields = mysql_num_fields(res);
    MYSQL_FIELD* fields     = mysql_fetch_fields(res);
    for (unsigned int i = 0; i < num_fields; ++i)
    {
        result.column_names.emplace_back(fields[i].name);
        result.column_types.emplace_back(
            std::to_string(fields[i].type)); // 使用 enum_field_types 数值
    }

    // 填充行数据
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr)
    {
        unsigned long* lengths = mysql_fetch_lengths(res);
        std::vector<std::string> row_data(num_fields);
        std::vector<bool>        null_data(num_fields, false);
        for (unsigned int i = 0; i < num_fields; ++i)
        {
            if (row[i] == nullptr)
            {
                null_data[i] = true;
            }
            else
            {
                row_data[i] = std::string(row[i], lengths[i]);
            }
        }
        result.rows.push_back(std::move(row_data));
        result.null_flags.push_back(std::move(null_data));
    }

    mysql_free_result(res);
    return true;
}

bool MySQLConnection::execute(const std::string& sql, ExecResult& result, std::string& err_msg)
{
    if (!connected_)
    {
        err_msg = "not connected";
        return false;
    }

    if (mysql_real_query(&mysql_, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
    {
        err_msg = mysql_error(&mysql_);
        ELOG << "MySQLConnection::execute failed: " << err_msg << " sql=" << sql;
        return false;
    }

    result.affected_rows  = static_cast<uint64_t>(mysql_affected_rows(&mysql_));
    result.last_insert_id = static_cast<uint64_t>(mysql_insert_id(&mysql_));
    return true;
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
