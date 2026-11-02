#pragma once

#include "Repository.h"

/**
 * @brief 复杂条件构造器
 *
 * 只在需要 OR / 范围 / 多条件组合的源文件里 include，避免把模板铺开到所有 TU
 *
 * 用法：
 *   using Cols = dbmeta::ChatMessage;
 *   auto rows = ChatMessageRepo::select()
 *                   .where(eq(Cols::channel, channel) && (eq(Cols::from_uid, uid) || eq(Cols::to_uid, uid)))
 *                   .where_if(cursor > 0, gt(Cols::id, cursor))
 *                   .order(asc(Cols::id))
 *                   .limit(count)
 *                   .all(&err);
 */
namespace LogicDB
{

namespace detail
{

template <typename Col, typename W>
Clause compare(const Col& col, std::string_view op, W value)
{
    static_assert(std::is_convertible_v<W, typename Col::Value>,
                  "value type does not match the column type");
    return Clause(std::string(col.name) + " " + std::string(op) + " ?", Params{toValue(value)});
}

template <typename Col>
Clause nullTest(const Col& col, std::string_view op)
{
    return Clause(std::string(col.name) + " " + std::string(op), Params{});
}

} // namespace detail

template <typename Col, typename W> Clause eq(  const Col& c, W v) { return detail::compare(c, "=",      v); }
template <typename Col, typename W> Clause ne(  const Col& c, W v) { return detail::compare(c, "<>",     v); }
template <typename Col, typename W> Clause lt(  const Col& c, W v) { return detail::compare(c, "<",      v); }
template <typename Col, typename W> Clause le(  const Col& c, W v) { return detail::compare(c, "<=",     v); }
template <typename Col, typename W> Clause gt(  const Col& c, W v) { return detail::compare(c, ">",      v); }
template <typename Col, typename W> Clause ge(  const Col& c, W v) { return detail::compare(c, ">=",     v); }
template <typename Col, typename W> Clause like(const Col& c, W v) { return detail::compare(c, "LIKE",   v); }

template <typename Col> Clause isNull(   const Col& c) { return detail::nullTest(c, "IS NULL"); }
template <typename Col> Clause isNotNull(const Col& c) { return detail::nullTest(c, "IS NOT NULL"); }

/**
 * @brief 链式查询构造器：Repository<T>::select() 返回它
 */
template <typename T>
class Select
{
public:
    // 条件以 AND 累加
    Select& where(Clause c)             { cond_ = cond_ && std::move(c); return *this; }
    Select& where_if(bool on, Clause c) { return on ? where(std::move(c)) : *this; }
    Select& order(Order o)              { order_ = std::move(o); return *this; }
    Select& limit(int64_t n)            { limit_ = n; return *this; }

    std::optional<std::vector<T>> all(std::string* err = nullptr) const
    {
        return Repository<T>::find(cond_, {.order = order_, .limit = limit_, .err = err});
    }

    std::optional<T> one(std::string* err = nullptr) const
    {
        return Repository<T>::first(cond_, {.order = order_, .err = err});
    }

    std::optional<int64_t> count(std::string* err = nullptr) const
    {
        return Repository<T>::count(cond_, err);
    }

    std::optional<bool> exists(std::string* err = nullptr) const
    {
        return Repository<T>::exists(cond_, err);
    }

private:
    Clause  cond_;
    Order   order_;
    int64_t limit_ = 0;
};

} // namespace LogicDB
