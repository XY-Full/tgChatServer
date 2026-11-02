#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 .proto 生成 DB 列定义

表结构写在 proto 里：
    message User {
        option (db.table) = "users";
        int64 id = 1 [(db.flags) = "pk,auto"];
    }

本脚本用 protoc 导出 FileDescriptorSet 并解码为文本，再生成 C++ 头文件：
    - 每个表一个列定义集合（列名 / getter / setter / has / flags）
    - LogicDB::Model<db::Xxx> 特化，供 Repository 使用
    - 声明了 (db.mirror) 的表额外生成 ToCs() 转换函数
"""

import argparse
import os
import subprocess
import sys
import tempfile

TYPE_MAP = {
    "TYPE_INT64": "int64_t",
    "TYPE_UINT64": "int64_t",
    "TYPE_FIXED64": "int64_t",
    "TYPE_SFIXED64": "int64_t",
    "TYPE_SINT64": "int64_t",
    "TYPE_INT32": "int32_t",
    "TYPE_UINT32": "int32_t",
    "TYPE_FIXED32": "int32_t",
    "TYPE_SFIXED32": "int32_t",
    "TYPE_SINT32": "int32_t",
    "TYPE_BOOL": "int32_t",
    "TYPE_DOUBLE": "double",
    "TYPE_FLOAT": "double",
    "TYPE_STRING": "std::string",
    "TYPE_BYTES": "std::string",
    "TYPE_ENUM": "int32_t",
}


# ──────────────────────────────────────────
# FileDescriptorSet 文本格式解析
# ──────────────────────────────────────────
def parse_blocks(lines, start, indent):
    """把 protoc 的文本输出解析成嵌套 dict；重复键收集为 list"""
    node = {}
    i = start
    while i < len(lines):
        line = lines[i]
        if not line.strip():
            i += 1
            continue
        cur = len(line) - len(line.lstrip())
        if cur < indent:
            break
        s = line.strip()
        if s == "}":
            i += 1
            continue
        if s.endswith("{"):
            key = s[:-1].strip()
            child, i = parse_blocks(lines, i + 1, cur + 2)
            node.setdefault(key, []).append(child)
        else:
            key, _, val = s.partition(":")
            key = key.strip()
            val = val.strip().strip('"')
            if key in node:  # 重复键（如 repeated option）收集成 list
                if isinstance(node[key], list):
                    node[key].append(val)
                else:
                    node[key] = [node[key], val]
            else:
                node[key] = val
            i += 1
    return node, i


def dump_descriptor(protoc, proto_paths, files, extra_schema):
    """protoc -> FileDescriptorSet -> 文本"""
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
        bin_path = tmp.name

    cmd = [protoc]
    for p in proto_paths:
        cmd += ["--proto_path", p]
    cmd += ["--descriptor_set_out", bin_path, "--include_imports"] + files
    subprocess.run(cmd, check=True)

    cmd = [protoc]
    for p in proto_paths:
        cmd += ["--proto_path", p]
    cmd += ["--decode=google.protobuf.FileDescriptorSet"]
    cmd += ["google/protobuf/descriptor.proto"] + extra_schema
    with open(bin_path, "rb") as f:
        text = subprocess.run(cmd, stdin=f, check=True, capture_output=True, text=True).stdout

    os.unlink(bin_path)
    lines = [l for l in text.splitlines() if l.strip()]
    root, _ = parse_blocks(lines, 0, 0)
    return root.get("file", [])


DEFAULT_SQL_TYPE = {
    "TYPE_INT64": "BIGINT",
    "TYPE_UINT64": "BIGINT UNSIGNED",
    "TYPE_FIXED64": "BIGINT UNSIGNED",
    "TYPE_SFIXED64": "BIGINT UNSIGNED",
    "TYPE_SINT64": "BIGINT",
    "TYPE_INT32": "INT",
    "TYPE_UINT32": "INT UNSIGNED",
    "TYPE_FIXED32": "INT UNSIGNED",
    "TYPE_SFIXED32": "INT UNSIGNED",
    "TYPE_SINT32": "INT",
    "TYPE_BOOL": "TINYINT",
    "TYPE_DOUBLE": "DOUBLE",
    "TYPE_FLOAT": "FLOAT",
    "TYPE_STRING": "VARCHAR(255)",
    "TYPE_BYTES": "BLOB",
    "TYPE_ENUM": "INT",
}


def parse_index(expr, table):
    """unique:uidx_name(col1,col2) / index(col1,col2) → 索引定义"""
    kind, sep, rest = expr.partition(":")
    if not sep:
        rest = kind
        kind = "index"
    name = ""
    if "(" in rest:
        name, _, cols = rest.partition("(")
        cols = cols.rstrip(")")
    else:
        cols = rest
    columns = [c.strip() for c in cols.split(",") if c.strip()]
    if not name:
        name = ("uidx_" if kind == "unique" else "idx_") + table + "_" + "_".join(columns)
    return {"name": name, "unique": kind == "unique", "columns": columns}


def opt_list(node, key):
    """取重复 option，返回 list"""
    options = node.get("options")
    if isinstance(options, list):
        options = options[0]
    if not options:
        return []
    val = options.get(key)
    if val is None:
        return []
    return val if isinstance(val, list) else [val]


def opt(node, key):
    """取 option：node['options'] 里的 [db.xxx]"""
    options = node.get("options")
    if isinstance(options, list):
        options = options[0]
    if not options:
        return None
    return options.get(key)


def fields_of(message):
    f = message.get("field", [])
    return f if isinstance(f, list) else [f]


def messages_of(file_node):
    m = file_node.get("message_type", [])
    return m if isinstance(m, list) else [m]


def public_dir():
    return os.path.dirname(os.path.abspath(__file__))


# ──────────────────────────────────────────
# 生成
# ──────────────────────────────────────────
def emit_schema(tables, out_path):
    """生成 DDL 描述（db_schema.h），供 dbmigrate 与线上库比对"""
    out = []
    out.append("// Auto-generated by db_model_gen.py")
    out.append("// DO NOT EDIT MANUALLY")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("#include <string>")
    out.append("#include <vector>")
    out.append("")
    out.append("// .proto 描述的期望表结构：dbmigrate 拿它与线上库比对，生成 DDL 计划")
    out.append("namespace dbschema")
    out.append("{")
    out.append("")
    out.append("struct ColumnDef")
    out.append("{")
    out.append("    std::string name;")
    out.append("    std::string sql_type;")
    out.append("    bool        not_null       = false;")
    out.append("    bool        primary_key    = false;")
    out.append("    bool        auto_increment = false;")
    out.append("    bool        has_default    = false;")
    out.append("    std::string default_value;")
    out.append("    std::string on_update;")
    out.append("    std::string comment;")
    out.append("};")
    out.append("")
    out.append("struct IndexDef")
    out.append("{")
    out.append("    std::string              name;")
    out.append("    bool                     unique  = false;")
    out.append("    std::vector<std::string> columns;")
    out.append("};")
    out.append("")
    out.append("struct TableDef")
    out.append("{")
    out.append("    std::string            name;")
    out.append("    std::string            engine  = \"InnoDB\";")
    out.append("    std::string            charset = \"utf8mb4\";")
    out.append("    std::string            comment;")
    out.append("    std::vector<ColumnDef> columns;")
    out.append("    std::vector<IndexDef>  indexes;")
    out.append("};")
    out.append("")
    out.append("inline const std::vector<TableDef>& tables()")
    out.append("{")
    out.append("    static const std::vector<TableDef> kTables = {")
    for t in tables:
        out.append("        TableDef{")
        out.append(f'            "{t["table"]}", "{t["engine"]}", "{t["charset"]}", "{t["comment"]}",')
        out.append("            {")
        for c in t["cols"]:
            nn = "true" if c["not_null"] else "false"
            pk = "true" if c["pk"] else "false"
            ai = "true" if c["auto"] else "false"
            hd = "true" if c["has_default"] else "false"
            dv = c["default_value"] or ""
            out.append(f'                ColumnDef{{"{c["name"]}", "{c["sql_type"]}", {nn}, {pk}, {ai}, {hd}, "{dv}", "{c["on_update"]}", "{c["comment"]}"}},')
        out.append("            },")
        out.append("            {")
        for ix in t["indexes"]:
            cols = ", ".join(f'"{c}"' for c in ix["columns"])
            uniq = "true" if ix["unique"] else "false"
            out.append(f'                IndexDef{{"{ix["name"]}", {uniq}, {{{cols}}}}},')
        out.append("            },")
        out.append("        },")
    out.append("    };")
    out.append("    return kTables;")
    out.append("}")
    out.append("")
    out.append("} // namespace dbschema")
    out.append("")
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")


def main():

    here = public_dir()
    root = os.path.dirname(here)
    ap = argparse.ArgumentParser()
    ap.add_argument("--protoc", default=os.path.join(root, "third/protobuf/bin/protoc"))
    ap.add_argument("--db-dir", default="db_proto")
    ap.add_argument("--proto-dir", default="proto")
    ap.add_argument("--out", default="proto_files/db_models.h")
    args = ap.parse_args()

    db_dir = os.path.join(here, args.db_dir)
    cs_dir = os.path.join(here, args.proto_dir)
    out_path = os.path.join(here, args.out)

    proto_paths = [db_dir, cs_dir, os.path.join(root, "third/protobuf/include")]
    db_files = sorted(f for f in os.listdir(db_dir) if f.endswith(".proto"))
    cs_files = sorted(f for f in os.listdir(cs_dir) if f.endswith(".proto"))

    files = dump_descriptor(args.protoc, proto_paths,
                            db_files + cs_files,
                            ["db_options.proto"])

    # 全名 -> (所在 proto 文件, message 节点)
    type_map = {}
    for f in files:
        pkg = f.get("package", "")
        for m in messages_of(f):
            full = f"{pkg}.{m['name']}" if pkg else m["name"]
            type_map[full] = (f["name"], m)

    tables = []
    for f in files:
        if not f["name"].endswith(".proto") or os.path.dirname(f["name"]):
            continue
        if f["name"] not in db_files:
            continue
        pkg = f.get("package", "")
        for m in messages_of(f):
            table = opt(m, "[db.table]") or m["name"]
            cols = []
            for fd in fields_of(m):
                cpp_type = TYPE_MAP.get(fd.get("type"))
                if cpp_type is None:
                    print(f"  skip unsupported field {m['name']}.{fd['name']} ({fd.get('type')})", file=sys.stderr)
                    continue
                flags = (opt(fd, "[db.flags]") or "").replace(" ", "")
                cols.append({
                    "name": opt(fd, "[db.column]") or fd["name"],
                    "field": fd["name"],
                    "cpp": cpp_type,
                    "proto_type": fd.get("type"),
                    "pk": "primary_key" in flags.split(","),
                    "auto": "auto_increment" in flags.split(","),
                    "optional": fd.get("proto3_optional") == "true",
                    "cs_field": opt(fd, "[db.cs_field]"),
                    "sql_type": opt(fd, "[db.type]") or DEFAULT_SQL_TYPE.get(fd.get("type"), "VARCHAR(255)"),
                    "not_null": opt(fd, "[db.not_null]") == "true",
                    "default_value": opt(fd, "[db.default_value]"),
                    "has_default": opt(fd, "[db.default_value]") is not None,
                    "on_update": opt(fd, "[db.on_update]") or "",
                    "comment": opt(fd, "[db.comment]") or "",
                })
            tables.append({
                "msg": f"{pkg.replace('.', '::')}::{m['name']}" if pkg else m["name"],
                "short": m["name"],
                "table": table,
                "mirror": opt(m, "[db.mirror]"),
                "on_conflict": opt(m, "[db.on_conflict]"),
                "engine": opt(m, "[db.engine]") or "InnoDB",
                "charset": opt(m, "[db.charset]") or "utf8mb4",
                "comment": opt(m, "[db.comment]") or "",
                "indexes": [parse_index(x, table) for x in opt_list(m, "[db.index]")],
                "cols": cols,
            })

    if not tables:
        print("no db table message found", file=sys.stderr)
        return 1

    out = []
    out.append("// Auto-generated by db_model_gen.py")
    out.append("// DO NOT EDIT MANUALLY")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append('#include "db/Model.h"')
    out.append('#include "db/Repository.h"')
    out.append("")
    out.append("#include <string>")
    out.append("#include <tuple>")
    out.append("")

    includes = sorted({f"db_{os.path.splitext(n)[0]}.pb.h" if False else os.path.splitext(n)[0] + ".pb.h"
                       for n in db_files if n != "db_options.proto"})
    for inc in includes:
        out.append(f'#include "{inc}"')

    mirror_includes = set()
    for t in tables:
        if t["mirror"] and t["mirror"] in type_map:
            mirror_includes.add(os.path.splitext(type_map[t["mirror"]][0])[0] + ".pb.h")
    for inc in sorted(mirror_includes):
        out.append(f'#include "{inc}"')
    out.append("")

    out.append("namespace dbmeta")
    out.append("{")
    for t in tables:
        msg = t["msg"]
        out.append("")
        out.append(f"// ── {msg} → {t['table']} ──")
        out.append(f"struct {t['short']}")
        out.append("{")
        out.append(f"    using Message = {msg};")
        out.append(f'    static constexpr std::string_view table() {{ return "{t["table"]}"; }}')
        out.append("")
        for c in t["cols"]:
            flags = []
            if c["pk"]:
                flags.append("LogicDB::FieldFlag::PrimaryKey")
            if c["auto"]:
                flags.append("LogicDB::FieldFlag::AutoIncrement")
            flags_expr = " | ".join(flags) if flags else "LogicDB::FieldFlag::None"

            if c["cpp"] == "std::string":
                getter = f'+[](const {msg}& m) -> std::string {{ return m.{c["field"]}(); }}'
                setter = f'+[]({msg}& m, std::string v) {{ m.set_{c["field"]}(std::move(v)); }}'
            elif c["proto_type"] == "TYPE_BOOL":
                getter = f'+[](const {msg}& m) -> int32_t {{ return m.{c["field"]}() ? 1 : 0; }}'
                setter = f'+[]({msg}& m, int32_t v) {{ m.set_{c["field"]}(v != 0); }}'
            else:
                getter = f'+[](const {msg}& m) -> {c["cpp"]} {{ return m.{c["field"]}(); }}'
                setter = f'+[]({msg}& m, {c["cpp"]} v) {{ m.set_{c["field"]}(v); }}'
            has = f'+[](const {msg}& m) -> bool {{ return m.has_{c["field"]}(); }}' if c["optional"] else "nullptr"

            out.append(f"    static constexpr LogicDB::Column<{msg}, {c['cpp']}> {c['name']}{{")
            out.append(f'        "{c["name"]}",')
            out.append(f"        {getter},")
            out.append(f"        {setter},")
            out.append(f"        {has},")
            out.append(f"        {flags_expr}}};")

        names = ", ".join(c["name"] for c in t["cols"])
        out.append("")
        out.append(f"    static constexpr auto all = std::tuple{{{names}}};")
        if t["mirror"]:
            out.append(f'    static constexpr std::string_view mirror() {{ return "{t["mirror"]}"; }}')
        if t.get("on_conflict"):
            out.append(f'    static constexpr std::string_view onConflictUpdate() {{ return "{t["on_conflict"]}"; }}')
        out.append("};")
    out.append("")
    out.append("} // namespace dbmeta")
    out.append("")

    # Model 特化
    out.append("namespace LogicDB")
    out.append("{")
    for t in tables:
        out.append(f"template <> struct Model<{t['msg']}> : dbmeta::{t['short']} {{}};")
        out.append(f"using {t['short']}Repo = Repository<{t['msg']}>;")
    out.append("")
    out.append("} // namespace LogicDB")
    out.append("")

    # mirror → 转换函数
    for t in tables:
        if not t["mirror"] or t["mirror"] not in type_map:
            continue
        mirror_msg = t["mirror"].replace('.', '::')  # C++ 类型
        mirror_fields = {f["name"]: f for f in fields_of(type_map[t["mirror"]][1])}
        out.append("namespace dbmeta")
        out.append("{")
        out.append("")
        out.append(f"// {t['msg']} → {mirror_msg}")
        out.append(f"inline {mirror_msg} ToCs(const {t['msg']}& src)")
        out.append("{")
        out.append(f"    {mirror_msg} dst;")
        for c in t["cols"]:
            target = c["cs_field"] or c["name"]
            mf = mirror_fields.get(target)
            if not mf:
                continue
            src_expr = f"src.{c['field']}()"
            if c["proto_type"] == "TYPE_BOOL":
                src_expr = f"(src.{c['field']}() ? 1 : 0)"
            mtype = TYPE_MAP.get(mf.get("type"))
            if mtype and mtype != c["cpp"] and mtype != "std::string":
                src_expr = f"static_cast<{mtype}>({src_expr})"
            out.append(f"    dst.set_{target}({src_expr});")
        out.append("    return dst;")
        out.append("}")
        out.append("")
        out.append("} // namespace dbmeta")
        out.append("")

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")

    print(f"generated {out_path} ({len(tables)} tables)")

    schema_path = os.path.join(here, "proto_files/db_schema.h")
    emit_schema(tables, schema_path)
    print(f"generated {schema_path} ({len(tables)} tables)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
