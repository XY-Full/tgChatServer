#include "XmlParser.h"
#include "tinyxml2.h"
#include <filesystem>
#include <regex>
#include <sstream>
#include <stdexcept>

using namespace tinyxml2;
namespace fs = std::filesystem;

// ── 工具函数 ─────────────────────────────────────────────────────────────

static int parseVersionFromFilename(const std::string& filename)
{
    static const std::regex re(R"(^V(\d+)__.+\.xml$)", std::regex::icase);
    std::smatch m;
    if (!std::regex_match(filename, m, re))
        throw std::runtime_error("invalid filename format: " + filename +
                                 " (expected V{NNN}__description.xml)");
    return std::stoi(m[1].str());
}

// 读取属性，属性不存在时返回 fallback
static const char* attr(const XMLElement* e, const char* name, const char* fallback = "")
{
    const char* v = e->Attribute(name);
    return v ? v : fallback;
}

static bool attrBool(const XMLElement* e, const char* name)
{
    const char* v = e->Attribute(name);
    if (!v) return false;
    return v[0] == '1' || v[0] == 't' || v[0] == 'T';
}

// 将逗号分隔的列名字符串分割为 vector
static std::vector<std::string> splitColumns(const std::string& s)
{
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string tok;
    while (std::getline(iss, tok, ','))
    {
        if (!tok.empty()) result.push_back(tok);
    }
    return result;
}

// ── 解析 <column> 元素 ───────────────────────────────────────────────────
// <column name="id" type="BIGINT" not_null="1" auto_increment="1" primary_key="1"/>
// <column name="uid" type="BIGINT UNSIGNED" not_null="1" default="0"/>
static ColumnDef parseColumn(const XMLElement* e)
{
    ColumnDef col;
    col.name           = attr(e, "name");
    col.type           = attr(e, "type");
    col.not_null       = attrBool(e, "not_null");
    col.auto_increment = attrBool(e, "auto_increment");
    col.primary_key    = attrBool(e, "primary_key");
    col.default_val    = attr(e, "default");
    col.on_update      = attr(e, "on_update");
    if (col.name.empty()) throw std::runtime_error("<column> missing 'name' attribute");
    if (col.type.empty()) throw std::runtime_error("<column name='" + col.name + "'> missing 'type' attribute");
    return col;
}

// ── 解析 <index> 元素 ────────────────────────────────────────────────────
// <index name="uidx_uid" columns="uid" unique="1"/>
// <index name="idx_from" columns="from_uid,created_at"/>
static IndexDef parseIndex(const XMLElement* e)
{
    IndexDef idx;
    idx.name    = attr(e, "name");
    idx.unique  = attrBool(e, "unique");
    idx.columns = splitColumns(attr(e, "columns"));
    if (idx.name.empty())    throw std::runtime_error("<index> missing 'name' attribute");
    if (idx.columns.empty()) throw std::runtime_error("<index name='" + idx.name + "'> missing 'columns' attribute");
    return idx;
}

// ── 解析各操作元素 ───────────────────────────────────────────────────────

// <create_table name="users" engine="InnoDB" charset="utf8mb4" comment="...">
//   <column .../>
//   <index  .../>
// </create_table>
static Operation parseCreateTable(const XMLElement* e)
{
    Operation op;
    op.type = OpType::CreateTable;
    op.create_table.table   = attr(e, "name");
    op.create_table.engine  = attr(e, "engine",  "InnoDB");
    op.create_table.charset = attr(e, "charset", "utf8mb4");
    op.create_table.comment = attr(e, "comment");
    if (op.create_table.table.empty())
        throw std::runtime_error("<create_table> missing 'name' attribute");

    for (const XMLElement* child = e->FirstChildElement(); child; child = child->NextSiblingElement())
    {
        std::string tag = child->Name();
        if (tag == "column") op.create_table.columns.push_back(parseColumn(child));
        else if (tag == "index") op.create_table.indexes.push_back(parseIndex(child));
        // 忽略注释等其他子元素
    }
    return op;
}

// <alter_table name="users">
//   <add    name="col" type="VARCHAR(64)" not_null="1" default="''" after="prev_col"/>
//   <drop   name="old_col"/>
//   <modify name="col" type="INT" not_null="1" default="0"/>
// </alter_table>
static Operation parseAlterTable(const XMLElement* e)
{
    Operation op;
    op.type = OpType::AlterTable;
    op.alter_table.table = attr(e, "name");
    if (op.alter_table.table.empty())
        throw std::runtime_error("<alter_table> missing 'name' attribute");

    for (const XMLElement* child = e->FirstChildElement(); child; child = child->NextSiblingElement())
    {
        std::string tag = child->Name();
        if (tag == "add")
        {
            ColumnDef col = parseColumn(child);
            // <add> 特有的 after 属性
            if (op.alter_table.after_column.empty())
                op.alter_table.after_column = attr(child, "after");
            op.alter_table.add_columns.push_back(col);
        }
        else if (tag == "drop")
        {
            std::string name = attr(child, "name");
            if (name.empty()) throw std::runtime_error("<drop> missing 'name' attribute");
            op.alter_table.drop_columns.push_back(name);
        }
        else if (tag == "modify")
        {
            op.alter_table.modify_columns.push_back(parseColumn(child));
        }
    }
    return op;
}

// <create_index name="idx_uid" table="users" columns="uid,created_at" unique="1"/>
static Operation parseCreateIndex(const XMLElement* e)
{
    Operation op;
    op.type = OpType::CreateIndex;
    op.create_index.name    = attr(e, "name");
    op.create_index.table   = attr(e, "table");
    op.create_index.unique  = attrBool(e, "unique");
    op.create_index.columns = splitColumns(attr(e, "columns"));
    if (op.create_index.name.empty())    throw std::runtime_error("<create_index> missing 'name'");
    if (op.create_index.table.empty())   throw std::runtime_error("<create_index> missing 'table'");
    if (op.create_index.columns.empty()) throw std::runtime_error("<create_index> missing 'columns'");
    return op;
}

// <drop_index name="idx_old" table="users"/>
static Operation parseDropIndex(const XMLElement* e)
{
    Operation op;
    op.type = OpType::DropIndex;
    op.drop_index.name  = attr(e, "name");
    op.drop_index.table = attr(e, "table");
    if (op.drop_index.name.empty())  throw std::runtime_error("<drop_index> missing 'name'");
    if (op.drop_index.table.empty()) throw std::runtime_error("<drop_index> missing 'table'");
    return op;
}

// <drop_table name="old_table" if_exists="1"/>
static Operation parseDropTable(const XMLElement* e)
{
    Operation op;
    op.type = OpType::DropTable;
    op.drop_table.table     = attr(e, "name");
    op.drop_table.if_exists = attrBool(e, "if_exists") || true; // 默认 true
    if (op.drop_table.table.empty()) throw std::runtime_error("<drop_table> missing 'name'");
    return op;
}

// <seed_data table="config">
//   <row key="site_name" value="tgChat"/>
//   <row key="version"   value="1.0"/>
// </seed_data>
//
// 也支持多列行：
// <seed_data table="users">
//   <row uid="100" username="admin" status="1"/>
// </seed_data>
static Operation parseSeedData(const XMLElement* e)
{
    Operation op;
    op.type = OpType::SeedData;
    op.seed_data.table = attr(e, "table");
    if (op.seed_data.table.empty()) throw std::runtime_error("<seed_data> missing 'table'");

    for (const XMLElement* row = e->FirstChildElement("row"); row; row = row->NextSiblingElement("row"))
    {
        // 支持 key/value 简写（单列种子）
        const char* key_attr = row->Attribute("key");
        if (key_attr)
        {
            std::string val = attr(row, "value");
            op.seed_data.rows.push_back({{ key_attr, val }});
        }
        else
        {
            // 多列：每个属性就是一列
            std::vector<std::pair<std::string, std::string>> r;
            for (const XMLAttribute* a = row->FirstAttribute(); a; a = a->Next())
                r.emplace_back(a->Name(), a->Value());
            if (!r.empty()) op.seed_data.rows.push_back(std::move(r));
        }
    }
    return op;
}

// ── 公开接口 ──────────────────────────────────────────────────────────────

MigrationFile parseMigrationFile(const std::string& filepath)
{
    MigrationFile mf;
    mf.filepath = filepath;
    mf.filename = fs::path(filepath).filename().string();
    mf.version  = parseVersionFromFilename(mf.filename);

    XMLDocument doc;
    if (doc.LoadFile(filepath.c_str()) != XML_SUCCESS)
        throw std::runtime_error(filepath + ": XML parse error: " + doc.ErrorStr());

    const XMLElement* root = doc.FirstChildElement("migration");
    if (!root)
        throw std::runtime_error(filepath + ": root element must be <migration>");

    mf.description = attr(root, "description");

    for (const XMLElement* child = root->FirstChildElement(); child; child = child->NextSiblingElement())
    {
        std::string tag = child->Name();
        if      (tag == "create_table")  mf.operations.push_back(parseCreateTable(child));
        else if (tag == "alter_table")   mf.operations.push_back(parseAlterTable(child));
        else if (tag == "create_index")  mf.operations.push_back(parseCreateIndex(child));
        else if (tag == "drop_index")    mf.operations.push_back(parseDropIndex(child));
        else if (tag == "drop_table")    mf.operations.push_back(parseDropTable(child));
        else if (tag == "seed_data")     mf.operations.push_back(parseSeedData(child));
        else
            throw std::runtime_error(filepath + ": unknown operation element <" + tag + ">");
    }

    if (mf.operations.empty())
        throw std::runtime_error(filepath + ": no operations found");

    return mf;
}

std::vector<std::string> validateMigrationFile(const std::string& filepath)
{
    std::vector<std::string> errors;
    try
    {
        parseMigrationFile(filepath);
    }
    catch (const std::exception& e)
    {
        errors.push_back(e.what());
    }
    return errors;
}
