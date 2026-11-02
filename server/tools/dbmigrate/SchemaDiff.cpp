#include "SchemaDiff.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <map>
#include <sstream>

namespace dbmigrate
{

namespace
{

static const char kQuote = 39;      // 单引号
static const char kBackslash = 92;  // 反斜杠
static const char kBacktick = 96;   // 反引号

std::string quoteIdent(const std::string& s)
{
    std::string out(1, kBacktick);
    for (char c : s)
    {
        if (c == kBacktick) out += kBacktick;
        out += c;
    }
    out += kBacktick;
    return out;
}

std::string quoteLiteral(const std::string& s)
{
    std::string out(1, kQuote);
    for (char c : s)
    {
        if (c == kQuote || c == kBackslash) out += kBackslash;
        out += c;
    }
    out += kQuote;
    return out;
}

std::string toUpper(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
    return s;
}

// 执行查询，返回行
bool queryRows(MYSQL* mysql, const std::string& sql,
               std::vector<std::vector<std::optional<std::string>>>& out, std::string& err)
{
    if (mysql_real_query(mysql, sql.c_str(), static_cast<unsigned long>(sql.size())) != 0)
    {
        err = mysql_error(mysql);
        return false;
    }

    MYSQL_RES* res = mysql_store_result(mysql);
    if (!res)
    {
        if (mysql_field_count(mysql) != 0) { err = mysql_error(mysql); return false; }
        return true;
    }

    const unsigned int ncols = mysql_num_fields(res);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(res)) != nullptr)
    {
        unsigned long* lens = mysql_fetch_lengths(res);
        std::vector<std::optional<std::string>> r(ncols);
        for (unsigned int i = 0; i < ncols; ++i)
            r[i] = row[i] ? std::optional<std::string>(std::string(row[i], lens[i])) : std::nullopt;
        out.push_back(std::move(r));
    }

    mysql_free_result(res);
    return true;
}

std::string escapeLiteral(MYSQL* mysql, const std::string& s)
{
    std::string out;
    out.resize(s.size() * 2 + 1);
    unsigned long len = mysql_real_escape_string(mysql, &out[0], s.c_str(), static_cast<unsigned long>(s.size()));
    out.resize(len);
    return out;
}

bool contains(const std::string& haystack, const std::string& needle)
{
    return toUpper(haystack).find(toUpper(needle)) != std::string::npos;
}

} // namespace

// ─────────────────────────────────────────────
// 归一化
// ─────────────────────────────────────────────
std::string normalizeType(const std::string& type)
{
    std::string s = toUpper(type);

    // 丢掉整型显示宽度：BIGINT(20) -> BIGINT
    static const std::vector<std::string> kIntTypes = {"TINYINT", "SMALLINT", "MEDIUMINT", "INT", "INTEGER", "BIGINT"};
    const auto lparen = s.find('(');
    std::string base = (lparen == std::string::npos) ? s : s.substr(0, lparen);
    while (!base.empty() && std::isspace(static_cast<unsigned char>(base.back()))) base.pop_back();

    if (std::find(kIntTypes.begin(), kIntTypes.end(), base) != kIntTypes.end() && lparen != std::string::npos)
    {
        const auto rparen = s.find(')', lparen);
        if (rparen != std::string::npos) s = base + s.substr(rparen + 1);
    }

    // 统一空白
    std::string out;
    bool last_space = true;
    for (char c : s)
    {
        if (std::isspace(static_cast<unsigned char>(c))) { if (!last_space) out += ' '; last_space = true; }
        else { out += c; last_space = false; }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string normalizeDefault(const std::string& value)
{
    std::string s = value;
    // 去掉首尾空白
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();

    // 去掉包裹的引号：'0' -> 0, '' -> ""
    if (s.size() >= 2 && s.front() == kQuote && s.back() == kQuote) s = s.substr(1, s.size() - 2);

    std::string up = toUpper(s);
    if (up == "CURRENT_TIMESTAMP()" || up == "CURRENT_TIMESTAMP") return "CURRENT_TIMESTAMP";
    if (up == "NULL") return "";
    return s;
}

std::string columnDefinition(const std::string& name, const std::string& type, bool not_null,
                             bool primary_key, bool auto_increment, bool has_default,
                             const std::string& default_value, const std::string& on_update,
                             const std::string& comment)
{
    std::ostringstream ss;
    ss << quoteIdent(name) << " " << type;
    if (not_null || primary_key) ss << " NOT NULL";
    if (has_default) ss << " DEFAULT " << default_value;
    if (auto_increment) ss << " AUTO_INCREMENT";
    if (!on_update.empty()) ss << " ON UPDATE " << on_update;
    if (!comment.empty()) ss << " COMMENT " << quoteLiteral(comment);
    return ss.str();
}

// ─────────────────────────────────────────────
// SchemaDiffer
// ─────────────────────────────────────────────
SchemaDiffer::SchemaDiffer(MYSQL* mysql, std::string database)
    : mysql_(mysql), database_(std::move(database))
{
}

const LiveTable* SchemaDiffer::findTable(const std::string& name) const
{
    for (const auto& t : live_)
        if (t.name == name) return &t;
    return nullptr;
}

bool SchemaDiffer::loadLive(std::string& err)
{
    const std::string db = escapeLiteral(mysql_, database_);

    // ── 表 ──
    std::vector<std::vector<std::optional<std::string>>> rows;
    std::string tables_sql =
        "SELECT TABLE_NAME, ENGINE, TABLE_COLLATION, TABLE_COMMENT FROM information_schema.TABLES"
        " WHERE TABLE_SCHEMA = '" + db + "' AND TABLE_TYPE = 'BASE TABLE' ORDER BY TABLE_NAME";
    if (!queryRows(mysql_, tables_sql, rows, err)) return false;

    for (const auto& r : rows)
    {
        LiveTable t;
        t.name    = r[0].value_or("");
        t.engine  = toUpper(r[1].value_or(""));
        t.comment = r[3].value_or("");
        const std::string collation = r[2].value_or("");
        const auto underscore = collation.find('_');
        t.charset = (underscore == std::string::npos) ? collation : collation.substr(0, underscore);
        live_.push_back(std::move(t));
    }

    // ── 列 ──
    std::vector<std::vector<std::optional<std::string>>> col_rows;
    std::string cols_sql =
        "SELECT TABLE_NAME, COLUMN_NAME, COLUMN_TYPE, IS_NULLABLE, COLUMN_DEFAULT, EXTRA, COLUMN_KEY, COLUMN_COMMENT"
        " FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = '" + db + "'"
        " ORDER BY TABLE_NAME, ORDINAL_POSITION";
    if (!queryRows(mysql_, cols_sql, col_rows, err)) return false;

    for (const auto& r : col_rows)
    {
        const std::string table = r[0].value_or("");
        LiveTable* t = const_cast<LiveTable*>(findTable(table));
        if (!t) continue;

        LiveColumn c;
        c.name           = r[1].value_or("");
        c.sql_type       = r[2].value_or("");
        c.not_null       = (toUpper(r[3].value_or("")) == "NO");
        c.has_default    = r[4].has_value();
        c.default_value  = r[4].value_or("");
        const std::string extra = r[5].value_or(""); // EXTRA
        c.auto_increment = contains(extra, "auto_increment");
        c.on_update      = contains(extra, "on update") ? "CURRENT_TIMESTAMP" : "";
        c.primary_key    = (toUpper(r[6].value_or("")) == "PRI");
        t->columns.push_back(std::move(c));
    }

    // ── 索引──
    std::vector<std::vector<std::optional<std::string>>> idx_rows;
    std::string idx_sql =
        "SELECT TABLE_NAME, INDEX_NAME, NON_UNIQUE, COLUMN_NAME FROM information_schema.STATISTICS"
        " WHERE TABLE_SCHEMA = '" + db + "' ORDER BY TABLE_NAME, INDEX_NAME, SEQ_IN_INDEX";
    if (!queryRows(mysql_, idx_sql, idx_rows, err)) return false;

    for (const auto& r : idx_rows)
    {
        const std::string table = r[0].value_or("");
        LiveTable* t = const_cast<LiveTable*>(findTable(table));
        if (!t) continue;

        const std::string name = r[1].value_or("");
        if (toUpper(name) == "PRIMARY") continue;

        const bool unique = (r[2].value_or("1") == "0");
        const std::string column = r[3].value_or("");

        auto it = std::find_if(t->indexes.begin(), t->indexes.end(),
                               [&](const LiveIndex& i) { return i.name == name; });
        if (it == t->indexes.end())
        {
            LiveIndex ix;
            ix.name   = name;
            ix.unique = unique;
            ix.columns.push_back(column);
            t->indexes.push_back(std::move(ix));
        }
        else
        {
            it->columns.push_back(column);
        }
    }

    return true;
}

std::vector<Change> SchemaDiffer::diff() const
{
    std::vector<Change> changes;

    for (const auto& want : dbschema::tables())
    {
        const LiveTable* has = findTable(want.name);

        // ── 表不存在：整表新建 ──
        if (!has)
        {
            std::ostringstream ss;
            ss << "CREATE TABLE " << quoteIdent(want.name) << " (\n";
            for (size_t i = 0; i < want.columns.size(); ++i)
            {
                const auto& c = want.columns[i];
                ss << "  " << columnDefinition(c.name, c.sql_type, c.not_null, c.primary_key,
                                               c.auto_increment, c.has_default, c.default_value,
                                               c.on_update, c.comment);
                const bool has_pk = std::any_of(want.columns.begin(), want.columns.end(),
                                                [](const dbschema::ColumnDef& c) { return c.primary_key; });
                const bool last = (i + 1 == want.columns.size()) && !has_pk && want.indexes.empty();
                ss << (last ? "\n" : ",\n");
            }

            std::vector<std::string> pk_cols;
            for (const auto& c : want.columns)
                if (c.primary_key) pk_cols.push_back(c.name);
            if (!pk_cols.empty())
            {
                ss << "  PRIMARY KEY (";
                for (size_t i = 0; i < pk_cols.size(); ++i)
                    ss << (i ? ", " : "") << quoteIdent(pk_cols[i]);
                ss << ")" << (want.indexes.empty() ? "\n" : ",\n");
            }

            for (size_t i = 0; i < want.indexes.size(); ++i)
            {
                const auto& ix = want.indexes[i];
                ss << "  " << (ix.unique ? "UNIQUE KEY " : "KEY ") << quoteIdent(ix.name) << " (";
                for (size_t j = 0; j < ix.columns.size(); ++j)
                    ss << (j ? ", " : "") << quoteIdent(ix.columns[j]);
                ss << ")" << (i + 1 == want.indexes.size() ? "\n" : ",\n");
            }

            ss << ") ENGINE=" << want.engine << " DEFAULT CHARSET=" << want.charset;
            if (!want.comment.empty()) ss << " COMMENT=" << quoteLiteral(want.comment);
            ss << ";";

            changes.push_back({ss.str(), "create table " + want.name, false});
            continue;
        }

        // ── 列 ──
        for (const auto& wc : want.columns)
        {
            const LiveColumn* lc = nullptr;
            for (const auto& c : has->columns)
                if (c.name == wc.name) { lc = &c; break; }

            if (!lc)
            {
                const std::string def = columnDefinition(wc.name, wc.sql_type, wc.not_null, wc.primary_key,
                                                         wc.auto_increment, wc.has_default, wc.default_value,
                                                         wc.on_update, wc.comment);
                changes.push_back({"ALTER TABLE " + quoteIdent(want.name) + " ADD COLUMN " + def + ";",
                                   "add column " + want.name + "." + wc.name, false});
                continue;
            }

            const bool want_not_null = wc.not_null || wc.primary_key;
            const bool type_same     = normalizeType(lc->sql_type) == normalizeType(wc.sql_type);
            const bool null_same     = lc->not_null == want_not_null;
            const bool ai_same       = lc->auto_increment == wc.auto_increment;
            const bool def_same      = lc->has_default == wc.has_default &&
                                       (!wc.has_default || normalizeDefault(lc->default_value) == normalizeDefault(wc.default_value));
            const bool upd_same      = normalizeDefault(lc->on_update) == normalizeDefault(wc.on_update);

            if (type_same && null_same && ai_same && def_same && upd_same) continue;

            const std::string def = columnDefinition(wc.name, wc.sql_type, wc.not_null, wc.primary_key,
                                                     wc.auto_increment, wc.has_default, wc.default_value,
                                                     wc.on_update, wc.comment);
            changes.push_back({"ALTER TABLE " + quoteIdent(want.name) + " MODIFY COLUMN " + def + ";",
                               "modify column " + want.name + "." + wc.name +
                                   " (live: " + normalizeType(lc->sql_type) + " -> want: " + normalizeType(wc.sql_type) + ")",
                               !type_same || (lc->not_null && !want_not_null)});
        }

        // 线上多出来的列：删列是破坏性的
        for (const auto& lc : has->columns)
        {
            bool found = false;
            for (const auto& wc : want.columns)
                if (wc.name == lc.name) { found = true; break; }
            if (found) continue;

            changes.push_back({"ALTER TABLE " + quoteIdent(want.name) + " DROP COLUMN " + quoteIdent(lc.name) + ";",
                               "drop column " + want.name + "." + lc.name + " (not described in proto)", true});
        }

        // ── 索引 ──
        for (const auto& wi : want.indexes)
        {
            const LiveIndex* li = nullptr;
            for (const auto& i : has->indexes)
                if (i.name == wi.name) { li = &i; break; }

            std::string cols;
            for (size_t j = 0; j < wi.columns.size(); ++j)
                cols += (j ? ", " : "") + quoteIdent(wi.columns[j]);

            if (!li)
            {
                changes.push_back({"ALTER TABLE " + quoteIdent(want.name) + " ADD " +
                                       (wi.unique ? "UNIQUE KEY " : "KEY ") + quoteIdent(wi.name) + " (" + cols + ");",
                                   "add index " + want.name + "." + wi.name, false});
                continue;
            }

            if (li->unique == wi.unique && li->columns == wi.columns) continue;

            changes.push_back({"ALTER TABLE " + quoteIdent(want.name) + " DROP INDEX " + quoteIdent(wi.name) + ";",
                               "drop index " + want.name + "." + wi.name + " (definition changed)", false});
            changes.push_back({"ALTER TABLE " + quoteIdent(want.name) + " ADD " +
                                   (wi.unique ? "UNIQUE KEY " : "KEY ") + quoteIdent(wi.name) + " (" + cols + ");",
                               "add index " + want.name + "." + wi.name, false});
        }

        for (const auto& li : has->indexes)
        {
            bool found = false;
            for (const auto& wi : want.indexes)
                if (wi.name == li.name) { found = true; break; }
            if (found) continue;

            changes.push_back({"ALTER TABLE " + quoteIdent(want.name) + " DROP INDEX " + quoteIdent(li.name) + ";",
                               "drop index " + want.name + "." + li.name + " (not described in proto)", false});
        }
    }

    return changes;
}

} // namespace dbmigrate
