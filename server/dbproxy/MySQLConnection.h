#pragma once

#include <mariadb/mysql.h>
#include <string>
#include <vector>
#include <cstdint>

struct MySQLConfig
{
    std::string host     = "127.0.0.1";
    uint16_t    port     = 3306;
    std::string user     = "root";
    std::string password = "";
    std::string database = "";
    int         connect_timeout_ms = 5000;
};

class MySQLConnection
{
public:
    explicit MySQLConnection(const MySQLConfig& config);
    ~MySQLConnection();

    // 禁止拷贝
    MySQLConnection(const MySQLConnection&)            = delete;
    MySQLConnection& operator=(const MySQLConnection&) = delete;

    bool connect();
    bool reconnect();
    bool ping();
    void close();
    bool isConnected() const { return connected_; }

    // ── 查询结果 ──
    struct QueryResult
    {
        std::vector<std::string>              column_names;
        std::vector<std::string>              column_types;
        std::vector<std::vector<std::string>> rows;
        std::vector<std::vector<bool>>        null_flags; // true 表示该位置为 NULL
    };

    // ── 执行结果 ──
    struct ExecResult
    {
        uint64_t affected_rows  = 0;
        uint64_t last_insert_id = 0;
    };

    // SELECT 查询，结果填入 result；失败返回 false，err_msg 携带错误信息
    bool query(const std::string& sql, QueryResult& result, std::string& err_msg);

    // INSERT/UPDATE/DELETE 执行
    bool execute(const std::string& sql, ExecResult& result, std::string& err_msg);

    // 事务控制
    bool beginTransaction(std::string& err_msg);
    bool commit(std::string& err_msg);
    bool rollback(std::string& err_msg);

    MYSQL* raw() { return &mysql_; }

private:
    MYSQL       mysql_;
    MySQLConfig config_;
    bool        connected_ = false;

    // 执行任意 SQL（内部用）
    bool execRaw(const std::string& sql, std::string& err_msg);
};
