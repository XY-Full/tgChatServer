#pragma once

#include "LogicDB.h"
#include "db/Column.h"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace LogicDB
{

enum class Sort
{
    Asc,
    Desc,
};

struct Order
{
    std::string column;
    Sort        sort = Sort::Asc;

    bool empty() const { return column.empty(); }
};

template <typename Col>
Order asc(const Col& col)  { return Order{std::string(col.name), Sort::Asc}; }

template <typename Col>
Order desc(const Col& col) { return Order{std::string(col.name), Sort::Desc}; }

// 查询选项
// 排序/条数/错误输出，按需指派：{.order = asc(...), .limit = 20, .err = &err}
struct QueryOpt
{
    Order        order;
    int64_t      limit = 0;
    std::string* err   = nullptr;
};

/**
 * @brief 查询条件：一段不含 WHERE 的 SQL 条件 + 对应的绑定参数
 *
 * 业务代码不直接写它，而是用 Criteria.h 里的 eq/gt/&&/|| 组合出来；
 * 值永远走绑定参数，不会拼进 SQL 文本。
 */
class Clause
{
public:
    Clause() = default;
    Clause(std::string sql, Params params) : sql_(std::move(sql)), params_(std::move(params)) {}

    bool               empty() const   { return sql_.empty(); }
    const std::string& sql() const     { return sql_; }
    const Params&      params() const  { return params_; }

    // 组合；任一侧为空则直接返回另一侧
    Clause operator&&(const Clause& other) const;
    Clause operator||(const Clause& other) const;
    Clause operator!() const;

private:
    std::string sql_;
    Params      params_;
};

} // namespace LogicDB
