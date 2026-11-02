#pragma once

#include <mariadb/mysql.h>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>
#include <vector>
#include <atomic>
#include <cstdint>

// SQL 绑定参数：null / 整型 / 浮点 / 字符串 / 二进制，对应 ss::DBValue
using ParamValue = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;
using ParamList  = std::vector<ParamValue>;

struct MySQLConfig
{
    std::string host     = "127.0.0.1";
    uint16_t    port     = 3306;
    std::string user     = "root";
    std::string password = "";
    std::string database = "";
    int         connect_timeout_ms = 5000;

    // TLS：use_ssl=true 时建立加密连接；ssl_ca 为空则用系统默认 CA
    bool        use_ssl                  = false;
    std::string ssl_ca;                  // CA 证书路径
    bool        ssl_verify_server_cert    = false; // 自签证书场景关闭校验

    // 网络读写超时（秒级精度，0 = 用库默认）。异步等待超时最终由它兜底
    int         read_timeout_ms  = 0;
    int         write_timeout_ms = 0;
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

    // SELECT 查询，结果填入 result
    bool query(const std::string& sql, const ParamList& params, QueryResult& result, std::string& err_msg);

    // INSERT/UPDATE/DELETE 执行
    bool execute(const std::string& sql, const ParamList& params, ExecResult& result, std::string& err_msg);

    // 事务控制
    bool beginTransaction(std::string& err_msg);
    bool commit(std::string& err_msg);
    bool rollback(std::string& err_msg);

    MYSQL* raw() { return &mysql_; }

    int socketFd() const { return mysql_.net.fd; }

    // MySQL 服务端线程 id
    unsigned long threadId() const { return mysql_thread_id(const_cast<MYSQL*>(&mysql_)); }

    // 执行一条文本协议语句
    bool execRawSql(const std::string& sql, std::string& err_msg) { return execRaw(sql, err_msg); }

    // 连接已不可用，归还时不再放回空闲队列，直接销毁重建
    void markBroken() { broken_.store(true); }
    bool isBroken() const { return broken_.load(); }

    // 占用标记，确保一条连接同一时刻只能被一个异步操作持有
    bool markBusy()
    {
        if (busy_.exchange(true)) { ++s_violations; return false; }
        return true;
    }
    void markIdle() { busy_.store(false); }
    static int64_t busyViolations() { return s_violations.load(); }

    // 预处理语句缓存
    MYSQL_STMT* findStatement(const std::string& sql) const;
    void        cacheStatement(const std::string& sql, MYSQL_STMT* stmt);
    void        dropStatement(const std::string& sql);

private:
    MYSQL             mysql_;
    MySQLConfig       config_;
    bool              connected_ = false;
    std::atomic<bool> busy_{false};
    std::atomic<bool> broken_{false};

    static std::atomic<int64_t> s_violations;

    // 预处理语句缓存，连接断开/重建时清空
    std::unordered_map<std::string, MYSQL_STMT*> stmt_cache_;

    // 取某条 SQL 对应的预处理语句
    MYSQL_STMT* prepareStatement(const std::string& sql, std::string& err_msg);

    // 清空所有缓存的预处理语句
    void clearStatementCache();

    // 执行任意 SQL
    bool execRaw(const std::string& sql, std::string& err_msg);
};

// 按参数类型填充 MYSQL_BIND，调用方需保证参数生命周期
bool bindStmtParams(MYSQL_STMT* stmt, const ParamList& params, std::vector<MYSQL_BIND>& binds,
                    std::vector<unsigned long>& lengths, std::vector<my_bool>& is_null,
                    std::string& err_msg);
