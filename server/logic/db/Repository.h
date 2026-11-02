#pragma once

#include "Model.h"
#include "Condition.h"
#include "SqlBuilder.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace LogicDB
{

// 复杂条件查询构造器，定义在 db/Criteria.h
template <typename U> class Select;

// 实体可通过 Model<T>::onConflictUpdate() 定制 upsert 冲突时的更新内容
template <typename M>
concept HasConflictUpdate = requires { { M::onConflictUpdate() } -> std::convertible_to<std::string_view>; };

/**
 * @brief 表数据访问入口：protobuf 表消息进，protobuf 表消息出，业务层不写 SQL
 *
 * 简单场景：get / findBy / findAllBy / findLike
 * 复杂场景：select() 链式构造，或把 eq/gt/&&/|| 组合出的 Clause 传给 find / count / removeWhere
 */
template <typename T>
class Repository
{
public:
    using Entity = T;

    // 复杂查询入口
    static Select<T> select() { return Select<T>{}; }

    // ── 查 ──

    // 按主键取一行
    static std::optional<T> get(int64_t id, std::string* err = nullptr)
    {
        auto pk = modelPkName<T>();
        if (!pk)
        {
            if (err) *err = "model has no primary key column";
            return std::nullopt;
        }

        auto rows = find(Clause(*pk + " = ?", Params{Value(id)}), {.limit = 1, .err = err});
        if (!rows || rows->empty()) return std::nullopt;
        return std::move(rows->front());
    }

    // 按单个字段等值查一行：findBy(dbmeta::User::username, account, &err)
    template <typename Col, typename W>
    static std::optional<T> findBy(const Col& col, W value, std::string* err = nullptr)
    {
        static_assert(std::is_same_v<T, typename Col::Message>, "column does not belong to this table");
        static_assert(std::is_convertible_v<W, typename Col::Value>,
                      "value type does not match the column type");

        auto rows = find(Clause(std::string(col.name) + " = ?", Params{toValue(value)}),
                         {.limit = 1, .err = err});
        if (!rows || rows->empty()) return std::nullopt;
        return std::move(rows->front());
    }

    // 按单个字段等值查多条
    template <typename Col, typename W>
    static std::optional<std::vector<T>> findAllBy(const Col& col, W value, const QueryOpt& opt = {})
    {
        static_assert(std::is_same_v<T, typename Col::Message>, "column does not belong to this table");
        static_assert(std::is_convertible_v<W, typename Col::Value>,
                      "value type does not match the column type");

        return find(Clause(std::string(col.name) + " = ?", Params{toValue(value)}), opt);
    }

    // like查询：被赋值的字段按 AND 组合（proto3 optional 用 has_xxx 判断）
    static std::optional<std::vector<T>> findLike(const T& example, const QueryOpt& opt = {})
    {
        return find(exampleClause(example), opt);
    }

    // 条件查询；cond 为空表示全表
    static std::optional<std::vector<T>> find(const Clause& cond = {}, const QueryOpt& opt = {})
    {
        std::vector<Order> orders;
        if (!opt.order.empty()) orders.push_back(opt.order);

        const std::string sql = sql::selectSql(Model<T>::table(), modelColumns<T>(), cond.sql(), orders, opt.limit);

        ResultSet rs;
        if (!query(sql, cond.params(), rs, opt.err)) return std::nullopt;

        std::vector<T> out;
        out.reserve(rs.size());
        for (const auto& row : rs.rows) out.push_back(modelFromRow<T>(row));
        return out;
    }

    // 条件查询取首行
    static std::optional<T> first(const Clause& cond = {}, const QueryOpt& opt = {})
    {
        QueryOpt one = opt;
        one.limit    = 1;
        auto rows    = find(cond, one);
        if (!rows || rows->empty()) return std::nullopt;
        return std::move(rows->front());
    }

    static std::optional<int64_t> count(const Clause& cond = {}, std::string* err = nullptr)
    {
        const std::string sql = sql::countSql(Model<T>::table(), cond.sql());

        ResultSet rs;
        if (!query(sql, cond.params(), rs, err)) return std::nullopt;
        if (rs.rows.empty()) return 0;
        return rs.rows.front().asInt64("cnt").value_or(0);
    }

    static std::optional<bool> exists(const Clause& cond, std::string* err = nullptr)
    {
        auto n = count(cond, err);
        if (!n) return std::nullopt;
        return *n > 0;
    }

    // ── 增 / 改 / 删 ──

    // 插入，返回自增主键；失败返回 0
    static int64_t insert(const T& entity, std::string* err = nullptr)
    {
        const std::string sql = sql::insertSql(Model<T>::table(), modelWriteColumns<T>());

        uint64_t last_id = 0;
        if (!exec(sql, modelValues(entity), nullptr, &last_id, err)) return 0;
        return static_cast<int64_t>(last_id);
    }

    // 插入，主键/唯一键冲突时转更新
    static bool upsert(const T& entity, std::string* err = nullptr)
    {
        const auto        columns = modelWriteColumns<T>();
        const std::string sql     = sql::upsertSql(Model<T>::table(), columns, conflictClause(columns));

        return exec(sql, modelValues(entity), nullptr, nullptr, err);
    }

    // 按主键更新全部业务列
    static bool update(const T& entity, std::string* err = nullptr)
    {
        auto pk = modelPkName<T>();
        if (!pk)
        {
            if (err) *err = "model has no primary key column";
            return false;
        }

        Params params = modelValues(entity);
        params.push_back(modelPkValue(entity));

        const std::string sql = sql::updateSql(Model<T>::table(), modelWriteColumns<T>(), *pk);
        return exec(sql, params, nullptr, nullptr, err);
    }

    // 按主键删除
    static bool remove(const T& entity, std::string* err = nullptr)
    {
        auto pk = modelPkName<T>();
        if (!pk)
        {
            if (err) *err = "model has no primary key column";
            return false;
        }

        const std::string sql = sql::deleteByIdSql(Model<T>::table(), *pk);
        return exec(sql, Params{modelPkValue(entity)}, nullptr, nullptr, err);
    }

    // 按条件删除；无条件时拒绝执行，避免误删全表
    static bool removeWhere(const Clause& cond, std::string* err = nullptr)
    {
        if (cond.empty())
        {
            if (err) *err = "removeWhere requires a non-empty condition";
            return false;
        }
        const std::string sql = sql::deleteWhereSql(Model<T>::table(), cond.sql());
        return exec(sql, cond.params(), nullptr, nullptr, err);
    }

private:
    static std::string conflictClause(const std::vector<std::string_view>& columns)
    {
        if constexpr (HasConflictUpdate<Model<T>>) return std::string(Model<T>::onConflictUpdate());
        else                                       return sql::conflictSql(columns);
    }

    // 结构体里被赋值的字段 -> AND 条件
    template <typename Col>
    static void addIfSet(const T& e, const Col& col, std::vector<std::string>& parts, Params& params)
    {
        using V = typename Col::Value;

        bool set = false;
        if (col.has)                                     set = col.has(e);
        else if constexpr (std::is_same_v<V, std::string>) set = !col.get(e).empty();
        else                                               set = col.get(e) != V{};

        if (!set) return;

        parts.push_back(std::string(col.name) + " = ?");
        params.push_back(toValue(col.get(e)));
    }

    static Clause exampleClause(const T& example)
    {
        std::vector<std::string> parts;
        Params                   params;

        std::apply([&](const auto&... col) { (..., addIfSet(example, col, parts, params)); }, Model<T>::all);

        std::string sql;
        for (size_t i = 0; i < parts.size(); ++i)
        {
            if (i) sql += " AND ";
            sql += parts[i];
        }
        return Clause(std::move(sql), std::move(params));
    }
};

} // namespace LogicDB
