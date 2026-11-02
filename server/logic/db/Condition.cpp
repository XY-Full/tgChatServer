#include "Condition.h"

#include <utility>

namespace LogicDB
{

namespace
{

Clause combine(const Clause& a, const Clause& b, const char* op)
{
    if (a.empty()) return b;
    if (b.empty()) return a;

    Params params = a.params();
    params.insert(params.end(), b.params().begin(), b.params().end());
    return Clause("(" + a.sql() + " " + op + " " + b.sql() + ")", std::move(params));
}

} // namespace

Clause Clause::operator&&(const Clause& other) const
{
    return combine(*this, other, "AND");
}

Clause Clause::operator||(const Clause& other) const
{
    return combine(*this, other, "OR");
}

Clause Clause::operator!() const
{
    if (empty()) return *this;
    return Clause("NOT (" + sql_ + ")", params_);
}

} // namespace LogicDB
