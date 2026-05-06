#include "SQLGenerator.h"
#include <sstream>
#include <stdexcept>

// ── 辅助函数 ────────────────────────────────────────────────────────────

static std::string quoteIdent(const std::string& s)
{
    return "`" + s + "`";
}

static std::string columnDefSQL(const ColumnDef& col)
{
    std::ostringstream ss;
    ss << "    " << quoteIdent(col.name) << " " << col.type;

    if (col.not_null)       ss << " NOT NULL";
    if (col.auto_increment) ss << " AUTO_INCREMENT";

    if (!col.default_val.empty())
    {
        // CURRENT_TIMESTAMP 不加引号，其他字符串值视情况
        const auto& dv = col.default_val;
        bool is_expr = (dv == "CURRENT_TIMESTAMP" || dv == "NULL" ||
                        (dv.size() > 0 && dv[0] == '('));
        if (is_expr)
            ss << " DEFAULT " << dv;
        else
            ss << " DEFAULT " << dv; // 调用方已负责添加适当引号
    }

    if (!col.on_update.empty())
        ss << " ON UPDATE " << col.on_update;

    return ss.str();
}

// ── CREATE TABLE ────────────────────────────────────────────────────────

std::string generateCreateTable(const CreateTableOp& op)
{
    std::ostringstream ss;
    ss << "CREATE TABLE IF NOT EXISTS " << quoteIdent(op.table) << " (\n";

    // 列定义
    bool has_pk = false;
    for (size_t i = 0; i < op.columns.size(); ++i)
    {
        ss << columnDefSQL(op.columns[i]);
        if (op.columns[i].primary_key) has_pk = true;
        if (i + 1 < op.columns.size() || has_pk || !op.indexes.empty())
            ss << ",";
        ss << "\n";
    }

    // PRIMARY KEY
    for (const auto& col : op.columns)
    {
        if (col.primary_key)
        {
            ss << "    PRIMARY KEY (" << quoteIdent(col.name) << ")";
            if (!op.indexes.empty()) ss << ",";
            ss << "\n";
            break;
        }
    }

    // 索引
    for (size_t i = 0; i < op.indexes.size(); ++i)
    {
        const auto& idx = op.indexes[i];
        ss << "    ";
        if (idx.unique) ss << "UNIQUE ";
        ss << "INDEX " << quoteIdent(idx.name) << " (";
        for (size_t j = 0; j < idx.columns.size(); ++j)
        {
            if (j) ss << ", ";
            ss << quoteIdent(idx.columns[j]);
        }
        ss << ")";
        if (i + 1 < op.indexes.size()) ss << ",";
        ss << "\n";
    }

    ss << ") ENGINE=" << op.engine
       << " DEFAULT CHARSET=" << op.charset;
    if (!op.comment.empty())
        ss << " COMMENT='" << op.comment << "'";
    ss << ";";
    return ss.str();
}

// ── ALTER TABLE ────────────────────────────────────────────────────────

std::string generateAlterTable(const AlterTableOp& op)
{
    std::ostringstream ss;
    std::string tbl = quoteIdent(op.table);
    bool first = true;

    auto sep = [&]() -> std::ostringstream& {
        if (!first) ss << ";\n";
        first = false;
        ss << "ALTER TABLE " << tbl;
        return ss;
    };

    for (const auto& col : op.add_columns)
    {
        sep() << " ADD COLUMN\n" << columnDefSQL(col);
        if (!op.after_column.empty())
            ss << " AFTER " << quoteIdent(op.after_column);
    }

    for (const auto& col_name : op.drop_columns)
        sep() << " DROP COLUMN " << quoteIdent(col_name);

    for (const auto& col : op.modify_columns)
        sep() << " MODIFY COLUMN\n" << columnDefSQL(col);

    if (!first) ss << ";";
    return ss.str();
}

// ── CREATE INDEX ───────────────────────────────────────────────────────

std::string generateCreateIndex(const CreateIndexOp& op)
{
    std::ostringstream ss;
    ss << "CREATE ";
    if (op.unique) ss << "UNIQUE ";
    ss << "INDEX " << quoteIdent(op.name)
       << " ON " << quoteIdent(op.table) << " (";
    for (size_t i = 0; i < op.columns.size(); ++i)
    {
        if (i) ss << ", ";
        ss << quoteIdent(op.columns[i]);
    }
    ss << ");";
    return ss.str();
}

// ── DROP INDEX ────────────────────────────────────────────────────────

std::string generateDropIndex(const DropIndexOp& op)
{
    std::ostringstream ss;
    ss << "DROP INDEX " << quoteIdent(op.name)
       << " ON " << quoteIdent(op.table) << ";";
    return ss.str();
}

// ── DROP TABLE ────────────────────────────────────────────────────────

std::string generateDropTable(const DropTableOp& op)
{
    std::ostringstream ss;
    ss << "DROP TABLE ";
    if (op.if_exists) ss << "IF EXISTS ";
    ss << quoteIdent(op.table) << ";";
    return ss.str();
}

// ── SEED DATA ─────────────────────────────────────────────────────────

std::string generateSeedData(const SeedDataOp& op)
{
    if (op.rows.empty()) return "";
    std::ostringstream ss;

    // 从第一行推断列名
    const auto& first_row = op.rows[0];
    ss << "INSERT IGNORE INTO " << quoteIdent(op.table) << " (";
    for (size_t i = 0; i < first_row.size(); ++i)
    {
        if (i) ss << ", ";
        ss << quoteIdent(first_row[i].first);
    }
    ss << ") VALUES\n";

    for (size_t r = 0; r < op.rows.size(); ++r)
    {
        ss << "    (";
        for (size_t c = 0; c < op.rows[r].size(); ++c)
        {
            if (c) ss << ", ";
            const auto& val = op.rows[r][c].second;
            // 数字和关键字不加引号
            bool is_numeric = !val.empty() && (std::isdigit(val[0]) || val[0] == '-');
            bool is_keyword = (val == "NULL" || val == "TRUE" || val == "FALSE" ||
                               val == "CURRENT_TIMESTAMP");
            if (is_numeric || is_keyword)
                ss << val;
            else
                ss << "'" << val << "'";
        }
        ss << ")";
        if (r + 1 < op.rows.size()) ss << ",";
        ss << "\n";
    }
    ss << ";";
    return ss.str();
}

// ── 分发 ────────────────────────────────────────────────────────────────

std::string operationToSQL(const Operation& op)
{
    switch (op.type)
    {
    case OpType::CreateTable:  return generateCreateTable(op.create_table);
    case OpType::AlterTable:   return generateAlterTable(op.alter_table);
    case OpType::CreateIndex:  return generateCreateIndex(op.create_index);
    case OpType::DropIndex:    return generateDropIndex(op.drop_index);
    case OpType::DropTable:    return generateDropTable(op.drop_table);
    case OpType::SeedData:     return generateSeedData(op.seed_data);
    default:
        throw std::runtime_error("unknown operation type");
    }
}
