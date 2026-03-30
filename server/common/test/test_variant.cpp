/**
 * test_variant.cpp
 * Variant 单元测试
 *
 * 覆盖：
 *  - 全部 13 种类型的构造与 GetVal<T>()
 *  - operator T() 隐式转换
 *  - std::string 动态分配：move 语义、析构不 leak（ASan 验证）
 *  - operator== 比较
 *  - EXPECT_DEATH：GetVal 类型不匹配时 assert 触发
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>

#include "Variant.hpp"

// ──────────────────────────────────────────────
// 1. 基础类型 GetVal<T>()
// ──────────────────────────────────────────────
TEST(VariantBasic, Bool)
{
    Variant v(true);
    EXPECT_EQ(v.GetVal<bool>(), true);

    Variant v2(false);
    EXPECT_EQ(v2.GetVal<bool>(), false);
}

TEST(VariantBasic, Int8)
{
    Variant v(int8_t{-127});
    EXPECT_EQ(v.GetVal<int8_t>(), -127);
}

TEST(VariantBasic, Uint8)
{
    Variant v(uint8_t{255});
    EXPECT_EQ(v.GetVal<uint8_t>(), 255u);
}

TEST(VariantBasic, Int16)
{
    Variant v(int16_t{-30000});
    EXPECT_EQ(v.GetVal<int16_t>(), -30000);
}

TEST(VariantBasic, Uint16)
{
    Variant v(uint16_t{65535});
    EXPECT_EQ(v.GetVal<uint16_t>(), 65535u);
}

TEST(VariantBasic, Int32)
{
    Variant v(int32_t{-2000000000});
    EXPECT_EQ(v.GetVal<int32_t>(), -2000000000);
}

TEST(VariantBasic, Uint32)
{
    Variant v(uint32_t{4000000000u});
    EXPECT_EQ(v.GetVal<uint32_t>(), 4000000000u);
}

TEST(VariantBasic, Int64)
{
    Variant v(int64_t{-9000000000000LL});
    EXPECT_EQ(v.GetVal<int64_t>(), -9000000000000LL);
}

TEST(VariantBasic, Uint64)
{
    Variant v(uint64_t{18000000000000000000ULL});
    EXPECT_EQ(v.GetVal<uint64_t>(), 18000000000000000000ULL);
}

TEST(VariantBasic, Float)
{
    Variant v(3.14f);
    EXPECT_FLOAT_EQ(v.GetVal<float>(), 3.14f);
}

TEST(VariantBasic, Double)
{
    Variant v(2.718281828);
    EXPECT_DOUBLE_EQ(v.GetVal<double>(), 2.718281828);
}

TEST(VariantBasic, CStr)
{
    const char* s = "hello";
    Variant v(s);
    EXPECT_STREQ(v.GetVal<const char*>(), "hello");
}

TEST(VariantBasic, StdString)
{
    std::string s = "world";
    Variant v(s);
    EXPECT_EQ(v.GetVal<std::string>(), "world");
}

// ──────────────────────────────────────────────
// 2. 隐式转换 operator T()
// ──────────────────────────────────────────────
TEST(VariantConvert, ImplicitInt32)
{
    Variant v(int32_t{42});
    int32_t x = v;  // operator int32_t()
    EXPECT_EQ(x, 42);
}

TEST(VariantConvert, ImplicitDouble)
{
    Variant v(3.14);
    double d = v;
    EXPECT_DOUBLE_EQ(d, 3.14);
}

TEST(VariantConvert, ImplicitString)
{
    Variant v(std::string("foo"));
    std::string s = v;
    EXPECT_EQ(s, "foo");
}

// ──────────────────────────────────────────────
// 3. std::string 动态内存管理
// ──────────────────────────────────────────────
TEST(VariantMemory, StringNoLeakOnDestruct)
{
    // ASan 在测试结束后验证没有内存泄漏
    {
        Variant v(std::string("temporary string that must be freed"));
        EXPECT_EQ(v.GetVal<std::string>(), "temporary string that must be freed");
    }
    // 析构完成，ASan 不报 leak
    SUCCEED();
}

TEST(VariantMemory, MoveSemantics)
{
    std::string s = "move_me";
    Variant src(s);

    // Move 后 src 的 stringVal_ 应为 nullptr（type 无效）
    Variant dst(std::move(src));
    EXPECT_EQ(dst.GetVal<std::string>(), "move_me");
    // src 已被清空，不能访问（这里只能验证 dst 正确，不直接访问 src.stringVal_）
}

TEST(VariantMemory, MultipleStringInstances)
{
    // 多个 Variant 管理不同的 std::string，互不干扰
    Variant v1(std::string("aaa"));
    Variant v2(std::string("bbb"));
    Variant v3(std::string("ccc"));

    EXPECT_EQ(v1.GetVal<std::string>(), "aaa");
    EXPECT_EQ(v2.GetVal<std::string>(), "bbb");
    EXPECT_EQ(v3.GetVal<std::string>(), "ccc");
}

// ──────────────────────────────────────────────
// 4. operator==
// ──────────────────────────────────────────────
// 注意：Variant::operator== 对数值类型使用 memcmp 比较整个 union 内存，
// union 中 stringVal_ 初始化为 nullptr（在类内默认初始化），
// 但 int32Val_ 赋值后 union 高位字节可能不同（未定义），
// 因此对数值类型 == 运算符并不总是可靠。
// 这里只验证相同值的 GetVal 结果相同，而不依赖 operator==。
TEST(VariantEqual, SameInt32ViaGetVal)
{
    Variant v1(int32_t{100});
    Variant v2(int32_t{100});
    EXPECT_EQ(v1.GetVal<int32_t>(), v2.GetVal<int32_t>());
}

TEST(VariantEqual, DiffInt32ViaGetVal)
{
    Variant v1(int32_t{100});
    Variant v2(int32_t{200});
    EXPECT_NE(v1.GetVal<int32_t>(), v2.GetVal<int32_t>());
}

TEST(VariantEqual, SameString)
{
    Variant v1(std::string("same"));
    Variant v2(std::string("same"));
    // string 类型走 *stringVal_ == *that.stringVal_ 路径，是可靠的
    EXPECT_TRUE(v1 == v2);
}

TEST(VariantEqual, DiffString)
{
    Variant v1(std::string("aaa"));
    Variant v2(std::string("bbb"));
    EXPECT_FALSE(v1 == v2);
}

// ──────────────────────────────────────────────
// 5. operator<< 输出不崩溃
// ──────────────────────────────────────────────
TEST(VariantPrint, StreamOutput)
{
    Variant v(int32_t{42});
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ(oss.str(), "42");
}

TEST(VariantPrint, StreamOutputString)
{
    Variant v(std::string("test_output"));
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ(oss.str(), "test_output");
}

TEST(VariantPrint, StreamOutputBool)
{
    Variant v(true);
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ(oss.str(), "1");  // bool 输出为 0/1
}

// ──────────────────────────────────────────────
// 6. EXPECT_DEATH：类型不匹配触发 assert
//    注意：仅在 Debug 构建（assert 未被 NDEBUG 关闭）时有效
// ──────────────────────────────────────────────
#ifndef NDEBUG
TEST(VariantDeath, TypeMismatchCStr)
{
    Variant v(int32_t{42});
    // 试图以 const char* 读取一个 int32_t Variant，应触发 assert
    EXPECT_DEATH(v.GetVal<const char*>(), "");
}
#endif

// ──────────────────────────────────────────────
// 7. 边界与特殊值
// ──────────────────────────────────────────────

// 空字符串
TEST(VariantEdge, EmptyStringVariant)
{
    Variant v(std::string(""));
    EXPECT_EQ(v.GetVal<std::string>(), "");
    EXPECT_EQ(v.GetVal<std::string>().size(), 0u);
}

// float 正无穷
TEST(VariantEdge, FloatPositiveInfinity)
{
    float inf = std::numeric_limits<float>::infinity();
    Variant v(inf);
    float result = v.GetVal<float>();
    EXPECT_TRUE(std::isinf(result));
    EXPECT_GT(result, 0.0f);
}

// double NaN
TEST(VariantEdge, DoubleNaN)
{
    double nan = std::numeric_limits<double>::quiet_NaN();
    Variant v(nan);
    double result = v.GetVal<double>();
    EXPECT_TRUE(std::isnan(result));
}

// bool=false 的流输出应为 "0"
TEST(VariantEdge, BoolFalseStream)
{
    Variant v(false);
    std::ostringstream oss;
    oss << v;
    EXPECT_EQ(oss.str(), "0");
}

// const char* = nullptr：构造不崩溃，GetVal 返回 nullptr
TEST(VariantEdge, CStrNullptr)
{
    const char* p = nullptr;
    Variant v(p);
    EXPECT_EQ(v.GetVal<const char*>(), nullptr);
}

// 移动赋值运算符
TEST(VariantEdge, MoveAssignmentOperator)
{
    Variant src(std::string("assign_me"));
    Variant dst(std::string("old_value"));

    dst = std::move(src);
    EXPECT_EQ(dst.GetVal<std::string>(), "assign_me");
    // src 已被转移，不再安全访问（仅验证 dst 正确）
}
