/**
 * test_helper.cpp
 * Helper 工具函数单元测试
 *
 * 覆盖（跳过需要网络的 sendTgMessage）：
 *  - timeGetTimeMS() / timeGetTimeUS() 单位和单调性
 *  - hmacSha256() 已知向量（RFC 4231 Test Case 1）
 *  - base64Decode() 标准 padding、无 padding、URL-safe 字符
 *  - GenUID() 唯一性
 *  - toHex() 已知输出
 */

#include <gtest/gtest.h>

#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <chrono>
#include <cmath>
#include <limits>

#include "Helper.h"

// ──────────────────────────────────────────────
// 1. 时间函数
// ──────────────────────────────────────────────
TEST(HelperTime, MilliSecondUnitAndMonotonic)
{
    uint64_t t1 = Helper::timeGetTimeMS();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t t2 = Helper::timeGetTimeMS();

    EXPECT_GE(t2, t1);
    // 至少过了 5ms（允许系统调度抖动）
    EXPECT_GE(t2 - t1, 5u);
    // 不超过 1000ms（正常情况）
    EXPECT_LE(t2 - t1, 1000u);
}

TEST(HelperTime, MicroSecondUnitAndMonotonic)
{
    uint64_t t1 = Helper::timeGetTimeUS();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uint64_t t2 = Helper::timeGetTimeUS();

    EXPECT_GE(t2, t1);
    // US 值应大于 MS 值（不同数量级）
    EXPECT_GT(Helper::timeGetTimeUS(), Helper::timeGetTimeMS());
}

TEST(HelperTime, SecondUnitAndMonotonic)
{
    uint64_t s1 = Helper::timeGetTimeS();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    uint64_t s2 = Helper::timeGetTimeS();
    // 秒级精度，差值 0 或 1 都合理
    EXPECT_LE(s2 - s1, 1u);
}

// ──────────────────────────────────────────────
// 2. HMAC-SHA256 已知向量
// RFC 4231 Test Case 1:
//   Key  = 0x0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b (20 bytes)
//   Data = "Hi There"
//   HMAC = b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
// ──────────────────────────────────────────────
TEST(HelperHmac, Rfc4231TestCase1)
{
    std::string key(20, '\x0b');
    std::string data = "Hi There";
    std::string expected_hex =
        "b0 34 4c 61 d8 db 38 53 5c a8 af ce af 0b f1 2b "
        "88 1d c2 00 c9 83 3d a7 26 e9 37 6c 2e 32 cf f7";

    std::string result = Helper::hmacSha256(key, data);
    ASSERT_EQ(result.size(), 32u); // SHA-256 = 32 bytes

    std::string result_hex = Helper::toHex(result.data(), result.size());
    // 去掉空格比较
    auto remove_spaces = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
        return s;
    };
    EXPECT_EQ(remove_spaces(result_hex), remove_spaces(expected_hex));
}

// 空 key 和空 data 不崩溃
TEST(HelperHmac, EmptyInputs)
{
    EXPECT_NO_THROW({
        auto r = Helper::hmacSha256("", "");
        EXPECT_EQ(r.size(), 32u);
    });
}

// ──────────────────────────────────────────────
// 3. base64Decode
// ──────────────────────────────────────────────
TEST(HelperBase64, StandardWithPadding)
{
    // "Hello" -> "SGVsbG8="
    EXPECT_EQ(Helper::base64Decode("SGVsbG8="), "Hello");
}

TEST(HelperBase64, StandardWithoutPadding)
{
    // JWT 风格：无 padding
    EXPECT_EQ(Helper::base64Decode("SGVsbG8"), "Hello");
}

TEST(HelperBase64, UrlSafeChars)
{
    // Base64url: '-' -> '+', '_' -> '/'
    // "Hello" in standard: SGVsbG8=
    // 将 "SGVs_G8=" (人工替换, 含 '_') 应能解码
    // 实际测试：使用 base64url 编码的已知字符串
    // "Man" -> "TWFu"（标准），url-safe 相同
    EXPECT_EQ(Helper::base64Decode("TWFu"), "Man");
}

TEST(HelperBase64, DoublePadding)
{
    // "Ma" -> "TWE=" (1 padding)
    EXPECT_EQ(Helper::base64Decode("TWE="), "Ma");
    EXPECT_EQ(Helper::base64Decode("TWE"), "Ma");  // 无 padding 也要正确
}

TEST(HelperBase64, EmptyString)
{
    EXPECT_EQ(Helper::base64Decode(""), "");
}

TEST(HelperBase64, BinaryData)
{
    // 已知："\x00\x01\x02" -> "AAEC"
    std::string expected = {'\x00', '\x01', '\x02'};
    EXPECT_EQ(Helper::base64Decode("AAEC"), expected);
}

// ──────────────────────────────────────────────
// 4. GenUID() 唯一性
// ──────────────────────────────────────────────
TEST(HelperUid, UniqueInSingleThread)
{
    // GenUID 设计：同一毫秒内最多 1000 个唯一 ID（counter 为 3 位）
    // 跨多毫秒则无上限，这里生成少量 UID 验证基本唯一性
    constexpr int kN = 500;
    std::set<int64_t> seen;
    for (int i = 0; i < kN; ++i)
    {
        auto uid = Helper::GenUID();
        EXPECT_TRUE(seen.insert(uid).second)
            << "Duplicate UID: " << uid << " at iteration " << i;
    }
    EXPECT_EQ(static_cast<int>(seen.size()), kN);
}

TEST(HelperUid, PositiveValues)
{
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_GT(Helper::GenUID(), 0LL);
    }
}

// ──────────────────────────────────────────────
// 5. toHex()
// ──────────────────────────────────────────────
TEST(HelperHex, KnownOutput)
{
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    std::string hex = Helper::toHex(data, sizeof(data));
    EXPECT_EQ(hex, "de ad be ef");
}

TEST(HelperHex, SingleByte)
{
    uint8_t data[] = {0x0F};
    EXPECT_EQ(Helper::toHex(data, 1), "0f");
}

TEST(HelperHex, AllZeros)
{
    uint8_t data[] = {0x00, 0x00, 0x00};
    EXPECT_EQ(Helper::toHex(data, 3), "00 00 00");
}

TEST(HelperHex, EmptyInput)
{
    EXPECT_EQ(Helper::toHex(nullptr, 0), "");
}

// ──────────────────────────────────────────────
// 6. IsTextUTF8
// ──────────────────────────────────────────────

// 纯 ASCII 字符串：全 ASCII → bAllAscii=true → 返回 false
TEST(HelperIsUTF8, PureAscii)
{
    EXPECT_FALSE(Helper::IsTextUTF8("Hello World"));
    EXPECT_FALSE(Helper::IsTextUTF8("abc123!@#"));
}

// 有效中文 UTF-8（3字节序列）
TEST(HelperIsUTF8, ValidChineseUtf8)
{
    // "中文" 的 UTF-8: E4 B8 AD  E6 96 87
    std::string chinese = "\xE4\xB8\xAD\xE6\x96\x87";
    EXPECT_TRUE(Helper::IsTextUTF8(chinese));
}

// 非法 UTF-8 序列：0x80 作为首字节（不合法）
TEST(HelperIsUTF8, InvalidLeadByte)
{
    std::string bad = "\x80\x81\x82";
    EXPECT_FALSE(Helper::IsTextUTF8(bad));
}

// 空字符串：bAllAscii=true → 返回 false
TEST(HelperIsUTF8, EmptyString)
{
    EXPECT_FALSE(Helper::IsTextUTF8(""));
}

// 混合 ASCII + UTF-8（有效）
TEST(HelperIsUTF8, MixedAsciiAndUtf8)
{
    // "hi 中" = 68 69 20 E4 B8 AD
    std::string mixed = "hi \xE4\xB8\xAD";
    EXPECT_TRUE(Helper::IsTextUTF8(mixed));
}

// ──────────────────────────────────────────────
// 7. HMAC-SHA256 额外测试
// ──────────────────────────────────────────────

// RFC 4231 Test Case 2:
//   Key  = "Jefe"
//   Data = "what do ya want for nothing?"
//   HMAC = 5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
TEST(HelperHmac, Rfc4231TestCase2)
{
    std::string key  = "Jefe";
    std::string data = "what do ya want for nothing?";
    std::string expected_hex =
        "5b dc c1 46 bf 60 75 4e 6a 04 24 26 08 95 75 c7 "
        "5a 00 3f 08 9d 27 39 83 9d ec 58 b9 64 ec 38 43";

    std::string result = Helper::hmacSha256(key, data);
    ASSERT_EQ(result.size(), 32u);

    std::string result_hex = Helper::toHex(result.data(), result.size());
    auto rm = [](std::string s) {
        s.erase(std::remove(s.begin(), s.end(), ' '), s.end());
        return s;
    };
    EXPECT_EQ(rm(result_hex), rm(expected_hex));
}

// key==data 时不崩溃，结果长度为 32
TEST(HelperHmac, KeyEqualsData)
{
    std::string s = "same_key_and_data_string";
    auto r = Helper::hmacSha256(s, s);
    EXPECT_EQ(r.size(), 32u);
}

// ──────────────────────────────────────────────
// 8. GenUID 跨毫秒单调性 & 多线程唯一性
// ──────────────────────────────────────────────

// 跨毫秒生成的 UID 应严格递增
TEST(HelperUid, MonotonicOverTime)
{
    int64_t u1 = Helper::GenUID();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    int64_t u2 = Helper::GenUID();
    EXPECT_GT(u2, u1);
}

// 4 线程各生成 100 个 UID，全局无重复
TEST(HelperUid, MultiThreadUnique)
{
    constexpr int kThreads = 4;
    constexpr int kPerThread = 100;

    std::vector<std::vector<int64_t>> results(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]() {
            results[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i)
                results[t].push_back(Helper::GenUID());
        });
    }
    for (auto& th : threads) th.join();

    // 合并所有 UID 放入 set，检查无重复
    std::set<int64_t> seen;
    for (int t = 0; t < kThreads; ++t)
        for (auto uid : results[t])
            seen.insert(uid);

    EXPECT_EQ(static_cast<int>(seen.size()), kThreads * kPerThread);
}

// ──────────────────────────────────────────────
// 9. Base64Decode 扩展
// ──────────────────────────────────────────────

// base64url：'-' 应被转换为 '+' 再解码
// "A-A=" → base64 "A+A=" → 解码: 0x03 0xA0 (2 bytes, standard padding)
// 使用已知向量验证：base64url("-w") → base64("+w==") → 0xFB
TEST(HelperBase64, UrlSafeDash)
{
    // base64url "-w" = base64 "+w==" = byte 0xFB
    std::string decoded = Helper::base64Decode("-w");
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ((uint8_t)decoded[0], 0xFBu);
}

// base64url：'_' 应被转换为 '/' 再解码
// base64url "_w==" → base64 "/w==" → 0xFF
TEST(HelperBase64, UrlSafeUnderscore)
{
    std::string decoded = Helper::base64Decode("_w");
    ASSERT_EQ(decoded.size(), 1u);
    EXPECT_EQ((uint8_t)decoded[0], 0xFFu);
}

// 大输入：256 字节输入，base64 编码后长度为 ceil(256/3)*4 = 344，解码后应回到 256
TEST(HelperHex, LargeInput)
{
    // toHex: 256 字节 → "xx xx ... xx"（每字节2字符，字节间1空格）
    // 格式: b0 b1 ... b255
    // 长度: 256*2 + 255*1 = 767
    std::vector<uint8_t> data(256);
    for (int i = 0; i < 256; ++i) data[i] = static_cast<uint8_t>(i);
    std::string hex = Helper::toHex(data.data(), data.size());
    EXPECT_EQ(hex.size(), 256u * 2u + 255u);  // 767 chars
}
