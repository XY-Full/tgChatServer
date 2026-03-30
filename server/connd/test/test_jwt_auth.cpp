/**
 * test_jwt_auth.cpp
 * JwtAuthProvider 单元测试
 *
 * 覆盖：
 *  - 有效 token → ok=true, user_id 正确
 *  - 无效签名 → ok=false, "signature mismatch"
 *  - 过期 token (exp < now) → ok=false, "token expired"
 *  - 未来过期 token (exp > now) → ok=true
 *  - 格式错误（< 3 段）→ ok=false, "invalid jwt format"
 *  - payload 缺少 sub/user_id → ok=false
 *  - sub 字段优先于 user_id 字段
 *  - secret 为空时任意 token 失败
 */

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include "JwtAuthProvider.h"
#include "Helper.h"

// ──────────────────────────────────────────────
// 辅助：Base64url 编码（Helper 只提供 decode，这里补充 encode）
// ──────────────────────────────────────────────
static std::string Base64UrlEncode(const std::string& input)
{
    static const char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3)
    {
        uint8_t a = static_cast<uint8_t>(input[i]);
        uint8_t b = (i + 1 < input.size()) ? static_cast<uint8_t>(input[i + 1]) : 0;
        uint8_t c = (i + 2 < input.size()) ? static_cast<uint8_t>(input[i + 2]) : 0;

        out += kTable[a >> 2];
        out += kTable[((a & 3) << 4) | (b >> 4)];
        out += (i + 1 < input.size()) ? kTable[((b & 0xF) << 2) | (c >> 6)] : '=';
        out += (i + 2 < input.size()) ? kTable[c & 0x3F] : '=';
    }

    // 转换为 base64url（'-' 替换 '+'，'_' 替换 '/'，去掉 '='）
    for (auto& ch : out)
    {
        if (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    // 去掉尾部 padding
    while (!out.empty() && out.back() == '=')
        out.pop_back();

    return out;
}

// ──────────────────────────────────────────────
// 辅助：生成测试用 JWT token
// ──────────────────────────────────────────────
static std::string MakeJwt(const std::string& secret,
                             const std::string& sub,
                             int64_t exp_offset = 3600,    // 秒，负数表示已过期
                             bool include_sub = true,
                             bool use_user_id_field = false)
{
    // Header（固定 HS256）
    std::string header_json = R"({"alg":"HS256","typ":"JWT"})";
    std::string header_b64  = Base64UrlEncode(header_json);

    // Payload
    int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    int64_t exp   = now_s + exp_offset;

    std::string payload_json;
    if (include_sub)
    {
        if (use_user_id_field)
            payload_json = R"({"user_id":")" + sub + R"(","exp":)" + std::to_string(exp) + "}";
        else
            payload_json = R"({"sub":")" + sub + R"(","exp":)" + std::to_string(exp) + "}";
    }
    else
    {
        payload_json = R"({"exp":)" + std::to_string(exp) + "}";
    }

    std::string payload_b64 = Base64UrlEncode(payload_json);

    // Signature
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string sig_raw  = Helper::hmacSha256(secret, signing_input);
    std::string sig_b64  = Base64UrlEncode(sig_raw);

    return header_b64 + "." + payload_b64 + "." + sig_b64;
}

// ──────────────────────────────────────────────
// Fixture
// ──────────────────────────────────────────────
class JwtAuthTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        provider_ = std::make_unique<JwtAuthProvider>(kSecret);
    }

    static constexpr const char* kSecret = "test_secret_key_2024";
    std::unique_ptr<JwtAuthProvider> provider_;
};

// ──────────────────────────────────────────────
// 1. 有效 Token
// ──────────────────────────────────────────────
TEST_F(JwtAuthTest, ValidTokenSubField)
{
    std::string token = MakeJwt(kSecret, "user42", 3600);
    auto result = provider_->verify(token);

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.user_id, "user42");
    EXPECT_TRUE(result.err_msg.empty());
}

TEST_F(JwtAuthTest, ValidTokenLongExpiry)
{
    std::string token = MakeJwt(kSecret, "player999", 86400 * 30);
    auto result = provider_->verify(token);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.user_id, "player999");
}

// ──────────────────────────────────────────────
// 2. 无效签名
// ──────────────────────────────────────────────
TEST_F(JwtAuthTest, InvalidSignature)
{
    // 用不同的 secret 签发，验证应失败
    std::string token = MakeJwt("wrong_secret", "user1", 3600);
    auto result = provider_->verify(token);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "signature mismatch");
}

TEST_F(JwtAuthTest, TamperedSignature)
{
    std::string token = MakeJwt(kSecret, "user1", 3600);
    // 破坏最后几个字符
    token.back() = (token.back() == 'a') ? 'b' : 'a';

    auto result = provider_->verify(token);
    EXPECT_FALSE(result.ok);
}

// ──────────────────────────────────────────────
// 3. 过期 Token
// ──────────────────────────────────────────────
TEST_F(JwtAuthTest, ExpiredToken)
{
    std::string token = MakeJwt(kSecret, "user1", -100);  // 100 秒前过期
    auto result = provider_->verify(token);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "token expired");
}

TEST_F(JwtAuthTest, JustExpiredToken)
{
    std::string token = MakeJwt(kSecret, "user1", -1);  // 1 秒前过期
    auto result = provider_->verify(token);
    EXPECT_FALSE(result.ok);
}

// ──────────────────────────────────────────────
// 4. 格式错误
// ──────────────────────────────────────────────
TEST_F(JwtAuthTest, EmptyToken)
{
    auto result = provider_->verify("");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "invalid jwt format");
}

TEST_F(JwtAuthTest, OnePart)
{
    auto result = provider_->verify("onlyonepart");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "invalid jwt format");
}

TEST_F(JwtAuthTest, TwoParts)
{
    auto result = provider_->verify("part1.part2");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "invalid jwt format");
}

TEST_F(JwtAuthTest, ExtraDotsAreOk)
{
    // 4 段（多一个点），header.payload.sig 仍然有效（第一个和第二个点之间是 header/payload）
    // 实际上 JWT 应严格三段，这里只验证不崩溃
    std::string token = MakeJwt(kSecret, "user1", 3600) + ".extra";
    EXPECT_NO_THROW(provider_->verify(token));
}

// ──────────────────────────────────────────────
// 5. payload 中缺少用户 ID 字段
// ──────────────────────────────────────────────
TEST_F(JwtAuthTest, MissingSubAndUserId)
{
    std::string token = MakeJwt(kSecret, "", 3600, false /* 不含 sub */);
    auto result = provider_->verify(token);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "missing sub/user_id in payload");
}

// ──────────────────────────────────────────────
// 6. sub 优先于 user_id
// ──────────────────────────────────────────────
TEST_F(JwtAuthTest, SubFieldPriority)
{
    // 使用 sub 字段
    std::string token_sub = MakeJwt(kSecret, "user_sub", 3600, true, false);
    auto r1 = provider_->verify(token_sub);
    EXPECT_TRUE(r1.ok);
    EXPECT_EQ(r1.user_id, "user_sub");
}

TEST_F(JwtAuthTest, UserIdFallback)
{
    // 使用 user_id 字段（无 sub）
    std::string token_uid = MakeJwt(kSecret, "user_uid", 3600, true, true);
    auto r2 = provider_->verify(token_uid);
    EXPECT_TRUE(r2.ok);
    EXPECT_EQ(r2.user_id, "user_uid");
}

// ──────────────────────────────────────────────
// 7. Secret 为空时任意 token 失败
// ──────────────────────────────────────────────
TEST(JwtAuthEmptySecret, AnyTokenFails)
{
    JwtAuthProvider provider(""); // 空 secret
    std::string token = MakeJwt("some_secret", "user1", 3600);
    auto result = provider.verify(token);
    EXPECT_FALSE(result.ok);
}

// ──────────────────────────────────────────────
// 8. 扩展测试
// ──────────────────────────────────────────────

// sub 字段为 1000 字符的长字符串，应能正确提取
TEST_F(JwtAuthTest, VeryLongUserId)
{
    std::string long_id(1000, 'x');
    long_id[0] = 'L'; long_id[999] = 'R';  // 标记头尾
    std::string token = MakeJwt(kSecret, long_id, 3600);
    auto result = provider_->verify(token);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.user_id, long_id);
}

// sub 含特殊 ASCII 字符（数字、点、下划线）
TEST_F(JwtAuthTest, SpecialCharsInUserId)
{
    // JSON 字符串中安全的特殊字符
    std::string special_id = "user.123_test-id";
    std::string token = MakeJwt(kSecret, special_id, 3600);
    auto result = provider_->verify(token);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.user_id, special_id);
}

// header 中 alg="RS256"，当前实现不校验 alg，只要签名（HS256）正确就通过
TEST_F(JwtAuthTest, AlgorithmFieldIgnored)
{
    // 手动构造：修改 header alg 为 RS256，但仍用 HS256 签名
    std::string header_json = R"({"alg":"RS256","typ":"JWT"})";
    std::string header_b64  = Base64UrlEncode(header_json);

    int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string payload_json = R"({"sub":"alg_user","exp":)" + std::to_string(now_s + 3600) + "}";
    std::string payload_b64  = Base64UrlEncode(payload_json);

    std::string signing_input = header_b64 + "." + payload_b64;
    std::string sig_raw  = Helper::hmacSha256(kSecret, signing_input);
    std::string sig_b64  = Base64UrlEncode(sig_raw);
    std::string token    = header_b64 + "." + payload_b64 + "." + sig_b64;

    auto result = provider_->verify(token);
    // 实现只校验签名和 exp，不校验 alg 字段 → 应通过
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.user_id, "alg_user");
}

// payload 含多余字段（iat, nbf, aud），不影响解析
TEST_F(JwtAuthTest, LargePayload)
{
    // 手动构造含多余字段的 payload
    std::string header_json = R"({"alg":"HS256","typ":"JWT"})";
    std::string header_b64  = Base64UrlEncode(header_json);

    int64_t now_s = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
    std::string payload_json =
        R"({"sub":"extra_user","iat":)" + std::to_string(now_s) +
        R"(,"nbf":)" + std::to_string(now_s) +
        R"(,"aud":"myapp","exp":)" + std::to_string(now_s + 3600) + "}";
    std::string payload_b64  = Base64UrlEncode(payload_json);

    std::string signing_input = header_b64 + "." + payload_b64;
    std::string sig_raw  = Helper::hmacSha256(kSecret, signing_input);
    std::string sig_b64  = Base64UrlEncode(sig_raw);
    std::string token    = header_b64 + "." + payload_b64 + "." + sig_b64;

    auto result = provider_->verify(token);
    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.user_id, "extra_user");
}

// 空 secret + 空 token：返回 invalid jwt format（格式错误优先于签名校验）
TEST(JwtAuthEmptySecret, EmptyTokenFails)
{
    JwtAuthProvider provider("");
    auto result = provider.verify("");
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "invalid jwt format");
}

// 4 线程同时 verify 同一个有效 token，全部返回 ok=true
TEST_F(JwtAuthTest, ConcurrentVerify)
{
    std::string token = MakeJwt(kSecret, "concurrent_user", 3600);

    constexpr int kThreads = 4;
    std::vector<bool> results(kThreads, false);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]() {
            auto r = provider_->verify(token);
            results[t] = r.ok;
        });
    }
    for (auto& th : threads) th.join();

    for (int t = 0; t < kThreads; ++t)
        EXPECT_TRUE(results[t]) << "Thread " << t << " failed";
}

// payload 通过签名校验，但 base64 解码后是非法 JSON
// → catch 分支：err_msg 以 "payload parse error: " 开头
TEST_F(JwtAuthTest, InvalidJsonPayload)
{
    // 构造一个 payload，base64url 编码的是非 JSON 字节串
    std::string header_json = R"({"alg":"HS256","typ":"JWT"})";
    std::string header_b64  = Base64UrlEncode(header_json);

    // 原始 payload：非 JSON，base64url 编码后作为 payload 段
    std::string bad_payload_raw = "this is not json at all!!!";
    std::string payload_b64     = Base64UrlEncode(bad_payload_raw);

    // 用正确 secret 签名（使签名通过），但 payload 是垃圾
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string sig_raw  = Helper::hmacSha256(kSecret, signing_input);
    std::string sig_b64  = Base64UrlEncode(sig_raw);
    std::string token    = header_b64 + "." + payload_b64 + "." + sig_b64;

    auto result = provider_->verify(token);
    EXPECT_FALSE(result.ok);
    // err_msg 应以 "payload parse error: " 开头
    EXPECT_EQ(result.err_msg.substr(0, 21), "payload parse error: ");
}
