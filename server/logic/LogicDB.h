#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

/**
 * @brief 同步 DB 访问助手
 *
 * 内部通过 bus_->Request("DBProxyService", ...) 发起 SS 请求并在协程中挂起
 * 等待应答，返回时已拿到 DB 执行结果
 *
 * SQL 一律写成带 ? 占位符的模板，实参通过 Params 单独下发，dbproxy 侧走
 * MySQL 预处理语句绑定，业务层不需要再做任何转义
 *
 * 注意：本头文件刻意不包含 ss_db.pb.h / ss_err_code.pb.h，避免与
 * err_code.pb.h（CS 的 ErrorCode::Error_success）在全局命名空间冲突
 */
namespace LogicDB
{
    // 绑定参数：null / 整数 / 浮点 / 字符串 / 二进制
    using Value  = std::variant<std::monostate, int64_t, double, std::string, std::vector<uint8_t>>;
    using Params = std::vector<Value>;

    // 查询结果的一行，按列名取强类型值
    class Row
    {
    public:
        using Columns = std::vector<std::string>;

        Row(std::shared_ptr<const Columns> columns, std::vector<std::optional<std::string>> values)
            : columns_(std::move(columns)), values_(std::move(values))
        {
        }

        // 列缺失或值为 NULL 时返回 nullopt
        bool                        isNull(std::string_view column) const;
        std::optional<std::string>  asString(std::string_view column) const;
        std::optional<int64_t>      asInt64(std::string_view column) const;
        std::optional<int32_t>      asInt32(std::string_view column) const;
        std::optional<double>       asDouble(std::string_view column) const;
        std::optional<bool>         asBool(std::string_view column) const;

    private:
        const std::optional<std::string>* find(std::string_view column) const;

        std::shared_ptr<const Columns>          columns_;
        std::vector<std::optional<std::string>> values_;
    };

    // 查询结果集
    struct ResultSet
    {
        std::shared_ptr<const Row::Columns> columns;
        std::vector<Row>                    rows;

        bool   empty() const { return rows.empty(); }
        size_t size()  const { return rows.size(); }
    };

    bool exec(std::string_view sql, const Params& params = {}, uint64_t* affected_rows = nullptr,
              uint64_t* last_insert_id = nullptr, std::string* err = nullptr);

    bool query(std::string_view sql, const Params& params, ResultSet& out, std::string* err = nullptr);

} // namespace LogicDB
