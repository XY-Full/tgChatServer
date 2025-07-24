
#pragma once

#include <memory>

namespace std
{
template <typename T> struct remove_constref
{
    typedef T type;
};

template <typename T> struct remove_constref<const T>
{
    typedef T type;
};

template <typename T> struct remove_constref<const T &>
{
    typedef T type;
};

template <typename T> struct remove_constref<const T &&>
{
    typedef T type;
};

template <typename T> struct remove_const_sharedptr_ref
{
};

template <typename T> struct remove_const_sharedptr_ref<const std::shared_ptr<T> &>
{
    typedef T type;
};
} // namespace std
