#pragma once

#include "StdExtend.h"
#include <assert.h>
#include <iostream>
#include <limits>
#include <string.h>
#include <tuple>
#include <typeinfo>
#include <unordered_map>
#include <vector>

class Variant;

template <typename T> struct VariantValue
{
    T GetVal(const Variant &var)
    {
        assert(0);
    }
};

namespace VariantType
{
template <typename T> constexpr uint8_t GetType()
{
    if (std::is_same<typename std::remove_constref<T>::type, bool>::value)
        return 0;
    if (std::is_same<typename std::remove_constref<T>::type, int8_t>::value)
        return 1;
    if (std::is_same<typename std::remove_constref<T>::type, uint8_t>::value)
        return 2;
    if (std::is_same<typename std::remove_constref<T>::type, int16_t>::value)
        return 3;
    if (std::is_same<typename std::remove_constref<T>::type, uint16_t>::value)
        return 4;
    if (std::is_same<typename std::remove_constref<T>::type, int32_t>::value)
        return 5;
    if (std::is_same<typename std::remove_constref<T>::type, uint32_t>::value)
        return 6;
    if (std::is_same<typename std::remove_constref<T>::type, int64_t>::value)
        return 7;
    if (std::is_same<typename std::remove_constref<T>::type, uint64_t>::value)
        return 8;
    if (std::is_same<typename std::remove_constref<T>::type, float>::value)
        return 9;
    if (std::is_same<typename std::remove_constref<T>::type, double>::value)
        return 10;
    if (std::is_same<typename std::remove_constref<T>::type, const char *>::value)
        return 11;
    if (std::is_same<typename std::remove_constref<T>::type, std::string>::value)
        return 12;
    return std::numeric_limits<uint8_t>::max();
}
}; // namespace VariantType

class Variant
{
    template <typename T> friend struct VariantValue;

#pragma pack(1)
    union {
        bool boolVal_;
        int8_t int8Val_;
        uint8_t uint8Val_;
        int16_t int16Val_;
        uint16_t uint16Val_;
        int32_t int32Val_;
        uint32_t uint32Val_;
        int64_t int64Val_;
        uint64_t uint64Val_;
        float floatVal_;
        double doubleVal_;
        const char *cstrPointer_;
        std::string *stringVal_{nullptr};
    };

    uint8_t type_{std::numeric_limits<uint8_t>::max()};
#pragma pack()

public:
    Variant(void)
    {
    }
    Variant(bool val) : boolVal_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(int8_t val) : int8Val_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(uint8_t val) : uint8Val_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(int16_t val) : int16Val_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(uint16_t val) : uint16Val_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(int32_t val) : int32Val_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(uint32_t val) : uint32Val_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(int64_t val) : int64Val_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(uint64_t val) : uint64Val_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(float val) : floatVal_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }
    Variant(double val) : doubleVal_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }

    Variant(const char *val) : cstrPointer_(val), type_(VariantType::GetType<decltype(val)>())
    {
    }

    Variant(const std::string &val) : type_(VariantType::GetType<decltype(val)>())
    {
        if (!stringVal_)
        {
            stringVal_ = new std::string;
        }
        *stringVal_ = val;
    }

    Variant(Variant &&that)
    {
        *this = std::move(that);
    }

    ~Variant(void)
    {
        constexpr uint8_t getType = VariantType::GetType<std::remove_pointer<decltype(stringVal_)>::type>();
        if (type_ == getType && stringVal_)
        {
            delete stringVal_;
            stringVal_ = nullptr;
        }
    }

    Variant &operator=(Variant &&that)
    {
        memcpy(this, &that, sizeof(Variant));
        constexpr uint8_t getType = VariantType::GetType<std::remove_pointer<decltype(stringVal_)>::type>();
        if (that.type_ == getType && that.stringVal_)
        {
            that.stringVal_ = nullptr;
            that.type_ = std::numeric_limits<uint8_t>::max();
        }
        return *this;
    }

    Variant(const Variant &that) = delete;
    Variant &operator=(const Variant &that) = delete;

    template <typename T> T GetVal(void)
    {
        constexpr uint8_t getType = VariantType::GetType<T>();
        // 获取值时, 数字类的没必要检查那么严格
        if (getType > 10)
        {
            if (getType != type_)
                assert(0);
        }
        return VariantValue<T>().GetVal(*this);
    }

    template <typename T> operator T()
    {
        constexpr uint8_t getType = VariantType::GetType<T>();
        // 获取值时, 数字类的没必要检查那么严格
        if (getType > 10)
        {
            if (getType != type_)
                assert(0);
        }
        return VariantValue<T>().GetVal(*this);
    }

    bool operator==(const Variant &that)
    {
        if (type_ == 12 && that.type_ == 12)
        {
            return *stringVal_ == *that.stringVal_;
        }
        return memcmp(this, &that, sizeof(Variant)) == 0;
    }

    friend std::ostream &operator<<(std::ostream &out, Variant &var)
    {
        switch (var.type_)
        {
        case 0:
            out << var.boolVal_;
            break;
        case 1:
            out << (int32_t)var.int8Val_;
            break;
        case 2:
            out << (uint32_t)var.uint8Val_;
            break;
        case 3:
            out << var.int16Val_;
            break;
        case 4:
            out << var.uint16Val_;
            break;
        case 5:
            out << var.int32Val_;
            break;
        case 6:
            out << var.uint32Val_;
            break;
        case 7:
            out << var.int64Val_;
            break;
        case 8:
            out << var.uint64Val_;
            break;
        case 9:
            out << var.floatVal_;
            break;
        case 10:
            out << var.doubleVal_;
            break;
        case 11:
            out << var.cstrPointer_;
            break;
        case 12:
            out << *var.stringVal_;
            break;
        default:
            out << "invalid type";
            break;
        }
        return out;
    }
};

template <> struct VariantValue<bool>
{
    bool GetVal(const Variant &var)
    {
        return var.boolVal_;
    }
};

template <> struct VariantValue<int8_t>
{
    int8_t GetVal(const Variant &var)
    {
        return var.int8Val_;
    }
};

template <> struct VariantValue<uint8_t>
{
    uint8_t GetVal(const Variant &var)
    {
        return var.uint8Val_;
    }
};

template <> struct VariantValue<int16_t>
{
    int16_t GetVal(const Variant &var)
    {
        return var.int16Val_;
    }
};

template <> struct VariantValue<uint16_t>
{
    uint16_t GetVal(const Variant &var)
    {
        return var.uint16Val_;
    }
};

template <> struct VariantValue<int32_t>
{
    int32_t GetVal(const Variant &var)
    {
        return var.int32Val_;
    }
};

template <> struct VariantValue<uint32_t>
{
    uint32_t GetVal(const Variant &var)
    {
        return var.uint32Val_;
    }
};

template <> struct VariantValue<int64_t>
{
    int64_t GetVal(const Variant &var)
    {
        return var.int64Val_;
    }
};

template <> struct VariantValue<uint64_t>
{
    uint64_t GetVal(const Variant &var)
    {
        return var.uint64Val_;
    }
};

template <> struct VariantValue<float>
{
    float GetVal(const Variant &var)
    {
        return var.floatVal_;
    }
};

template <> struct VariantValue<double>
{
    double GetVal(const Variant &var)
    {
        return var.doubleVal_;
    }
};

template <> struct VariantValue<const char *>
{
    const char *GetVal(const Variant &var)
    {
        return var.cstrPointer_;
    }
};

template <> struct VariantValue<const std::string &>
{
    const std::string &GetVal(const Variant &var)
    {
        return *var.stringVal_;
    }
};

template <> struct VariantValue<std::string &>
{
    std::string &GetVal(const Variant &var)
    {
        return *var.stringVal_;
    }
};

template <> struct VariantValue<std::string>
{
    std::string GetVal(const Variant &var)
    {
        return *var.stringVal_;
    }
};
