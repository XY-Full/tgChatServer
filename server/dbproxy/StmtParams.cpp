#include "MySQLConnection.h"


#include "Log.h"

// 单条连接上缓存的预处理语句上限


// 单条连接上缓存的预处理语句上限
constexpr size_t kMaxCachedStatements = 64;

// ── 参数绑定：同步与异步两条执行路径共用 ──────────────────────────────────
bool bindStmtParams(MYSQL_STMT* stmt, const ParamList& params, std::vector<MYSQL_BIND>& binds,
                    std::vector<unsigned long>& lengths, std::vector<my_bool>& is_null,
                    std::string& err_msg)
{
    const size_t expected = static_cast<size_t>(mysql_stmt_param_count(stmt));
    if (expected != params.size())
    {
        err_msg = "placeholder count mismatch: sql expects " + std::to_string(expected) +
                  ", got " + std::to_string(params.size());
        return false;
    }

    binds.assign(expected, MYSQL_BIND{});
    lengths.assign(expected, 0);
    is_null.assign(expected, 0);

    for (size_t i = 0; i < expected; ++i)
    {
        MYSQL_BIND& b = binds[i];
        b.length  = &lengths[i];
        b.is_null = &is_null[i];

        const ParamValue& p = params[i];
        if (const auto* v = std::get_if<int64_t>(&p))
        {
            b.buffer_type = MYSQL_TYPE_LONGLONG;
            b.buffer      = const_cast<int64_t*>(v);
        }
        else if (const auto* v = std::get_if<double>(&p))
        {
            b.buffer_type = MYSQL_TYPE_DOUBLE;
            b.buffer      = const_cast<double*>(v);
        }
        else if (const auto* v = std::get_if<std::string>(&p))
        {
            b.buffer_type   = MYSQL_TYPE_STRING;
            b.buffer        = const_cast<char*>(v->data());
            b.buffer_length = static_cast<unsigned long>(v->size());
            lengths[i]      = b.buffer_length;
        }
        else if (const auto* v = std::get_if<std::vector<uint8_t>>(&p))
        {
            b.buffer_type   = MYSQL_TYPE_BLOB;
            b.buffer        = const_cast<uint8_t*>(v->data());
            b.buffer_length = static_cast<unsigned long>(v->size());
            lengths[i]      = b.buffer_length;
        }
        else // std::monostate → NULL
        {
            b.buffer_type = MYSQL_TYPE_NULL;
            is_null[i]    = 1;
        }
    }

    if (mysql_stmt_bind_param(stmt, binds.data()) != 0)
    {
        err_msg = mysql_stmt_error(stmt);
        return false;
    }
    return true;
}

// ── 预处理语句缓存：异步路径按 SQL 复用 MYSQL_STMT ────────────────────────
MYSQL_STMT* MySQLConnection::findStatement(const std::string& sql) const
{
    const auto it = stmt_cache_.find(sql);
    return it == stmt_cache_.end() ? nullptr : it->second;
}

void MySQLConnection::cacheStatement(const std::string& sql, MYSQL_STMT* stmt)
{
    if (stmt_cache_.size() >= kMaxCachedStatements) clearStatementCache();
    stmt_cache_[sql] = stmt;
}

void MySQLConnection::dropStatement(const std::string& sql)
{
    const auto it = stmt_cache_.find(sql);
    if (it == stmt_cache_.end()) return;
    mysql_stmt_close(it->second);
    stmt_cache_.erase(it);
}
