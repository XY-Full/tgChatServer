#include "LogicDB.h"

#include "GlobalSpace.h"
#include "bus/IBus.h"
#include "Log.h"
#include "ss_db.pb.h"
#include "ss_err_code.pb.h"

#include <cerrno>
#include <cstdlib>

namespace LogicDB
{

namespace
{

// 把绑定参数写进请求，顺序对应 SQL 中 ? 的出现顺序
template <typename Req>
void fillParams(const Params& params, Req* req)
{
    for (const auto& p : params)
    {
        auto* v = req->mutable_request()->add_params();
        if (const auto* i = std::get_if<int64_t>(&p))
        {
            v->set_int_val(*i);
        }
        else if (const auto* d = std::get_if<double>(&p))
        {
            v->set_double_val(*d);
        }
        else if (const auto* s = std::get_if<std::string>(&p))
        {
            v->set_str_val(*s);
        }
        else if (const auto* b = std::get_if<std::vector<uint8_t>>(&p))
        {
            v->set_blob_val(b->data(), b->size());
        }
        else
        {
            v->set_null_val(true);
        }
    }
}

template <typename T>
std::optional<T> toInt(const std::string& s)
{
    if (s.empty()) return std::nullopt;

    errno    = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (end == s.c_str() || errno == ERANGE) return std::nullopt;
    return static_cast<T>(v);
}

} // namespace

// ─────────────────────────────────────────────
// Row
// ─────────────────────────────────────────────
const std::optional<std::string>* Row::find(std::string_view column) const
{
    if (!columns_) return nullptr;

    for (size_t i = 0; i < columns_->size() && i < values_.size(); ++i)
    {
        if ((*columns_)[i] == column) return &values_[i];
    }
    return nullptr;
}

bool Row::isNull(std::string_view column) const
{
    const auto* v = find(column);
    return !v || !v->has_value();
}

std::optional<std::string> Row::asString(std::string_view column) const
{
    const auto* v = find(column);
    if (!v) return std::nullopt;
    return *v;
}

std::optional<int64_t> Row::asInt64(std::string_view column) const
{
    const auto* v = find(column);
    if (!v || !v->has_value()) return std::nullopt;
    return toInt<int64_t>(**v);
}

std::optional<int32_t> Row::asInt32(std::string_view column) const
{
    const auto* v = find(column);
    if (!v || !v->has_value()) return std::nullopt;
    return toInt<int32_t>(**v);
}

std::optional<double> Row::asDouble(std::string_view column) const
{
    const auto* v = find(column);
    if (!v || !v->has_value() || (*v)->empty()) return std::nullopt;

    errno     = 0;
    char* end = nullptr;
    const double d = std::strtod((*v)->c_str(), &end);
    if (end == (*v)->c_str() || errno == ERANGE) return std::nullopt;
    return d;
}

std::optional<bool> Row::asBool(std::string_view column) const
{
    const auto v = asInt64(column);
    if (!v) return std::nullopt;
    return *v != 0;
}

bool exec(std::string_view sql, const Params& params, uint64_t* affected_rows, uint64_t* last_insert_id,
          std::string* err)
{
    ss::DBExecReq req;
    req.mutable_request()->set_sql(std::string(sql));
    fillParams(params, &req);

    AppMsgPtr rsp;
    try
    {
        rsp = GlobalSpace()->bus_->Request("DBProxyService", req);
    }
    catch (const std::exception& e)
    {
        if (err) *err = e.what();
        ELOG << "LogicDB::exec: request failed: " << e.what();
        return false;
    }

    if (!rsp)
    {
        if (err) *err = "no response";
        return false;
    }

    ss::DBExecReq resp;
    if (!resp.ParseFromArray(rsp->data_, rsp->data_len_))
    {
        if (err) *err = "parse response failed";
        return false;
    }

    const auto& body = resp.response();
    if (body.err() != SSErrorCode::Error_success)
    {
        if (err) *err = body.err_msg();
        ELOG << "LogicDB::exec: db error: " << body.err_msg();
        return false;
    }

    if (affected_rows)  *affected_rows  = body.affected_rows();
    if (last_insert_id) *last_insert_id = body.last_insert_id();
    return true;
}

bool query(std::string_view sql, const Params& params, ResultSet& out, std::string* err)
{
    ss::DBQueryReq req;
    req.mutable_request()->set_sql(std::string(sql));
    fillParams(params, &req);

    AppMsgPtr rsp;
    try
    {
        rsp = GlobalSpace()->bus_->Request("DBProxyService", req);
    }
    catch (const std::exception& e)
    {
        if (err) *err = e.what();
        ELOG << "LogicDB::query: request failed: " << e.what();
        return false;
    }

    if (!rsp)
    {
        if (err) *err = "no response";
        return false;
    }

    ss::DBQueryReq resp;
    if (!resp.ParseFromArray(rsp->data_, rsp->data_len_))
    {
        if (err) *err = "parse response failed";
        return false;
    }

    if (resp.response().err() != SSErrorCode::Error_success)
    {
        if (err) *err = resp.response().err_msg();
        ELOG << "LogicDB::query: db error: " << resp.response().err_msg();
        return false;
    }

    auto columns = std::make_shared<Row::Columns>();
    columns->reserve(resp.response().columns_size());
    for (int c = 0; c < resp.response().columns_size(); ++c)
    {
        columns->push_back(resp.response().columns(c).name());
    }

    out.columns = columns;
    out.rows.clear();
    out.rows.reserve(resp.response().rows_size());

    for (int r = 0; r < resp.response().rows_size(); ++r)
    {
        const auto& row = resp.response().rows(r);
        std::vector<std::optional<std::string>> values;
        values.reserve(row.values_size());
        for (int c = 0; c < row.values_size(); ++c)
        {
            const auto& v = row.values(c);
            if (v.null_val())
            {
                values.emplace_back(std::nullopt);
            }
            else
            {
                values.emplace_back(v.str_val());
            }
        }
        out.rows.emplace_back(columns, std::move(values));
    }
    return true;
}

} // namespace LogicDB
