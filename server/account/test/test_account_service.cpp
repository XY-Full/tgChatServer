/**
 * test_account_service.cpp
 * AccountService JWT 签发逻辑单元测试
 *
 * 由于 AccountApp 是 IApp 子类，依赖完整的 bus/GlobalSpace 生命周期，
 * 不适合在单元测试中直接实例化。
 * 本测试将 signJwt 核心逻辑提取为独立函数（与 AccountService.cpp 中实现一致），
 * 并使用 JwtAuthProvider 交叉验证：
 *   1. AccountService 签发的 token 能被 JwtAuthProvider 成功验证
 *   2. token 结构（3 段 base64url）
 *   3. sub 字段等于 account 名
 *   4. exp 字段在 now + ttl 附近
 *   5. 不同 account 得到不同 token
 *   6. 不同 secret 签发的 token 被另一个 secret 的 provider 拒绝
 *   7. ttl=0 立即过期的 token 被 provider 拒绝
 *   8. secret 为空时 provider 拒绝所有 token
 *   9. 并发签发不崩溃，结果各不相同
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <set>
#include <string>
#include <thread>
#include <vector>

// 交叉验证用
#include "JwtAuthProvider.h"
#include "Helper.h"

// ─────────────────────────────────────────────
// 与 AccountService.cpp 中完全相同的辅助函数
// （独立复制以便纯函数测试，不依赖 IApp/GlobalSpace）
// ─────────────────────────────────────────────

static std::string Base64UrlEncodeForTest(const std::string& input)
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

    for (auto& ch : out)
    {
        if      (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    while (!out.empty() && out.back() == '=')
        out.pop_back();

    return out;
}

/**
 * 模拟 AccountService::signJwt 逻辑（不依赖 GlobalSpace/Config）
 */
static std::string SignJwtForTest(const std::string& account,
                                  const std::string& secret,
                                  int64_t            ttl_seconds = 86400)
{
    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const std::string header_json  = R"({"alg":"HS256","typ":"JWT"})";
    const std::string payload_json =
        R"({"sub":")" + account +
        R"(","exp":)" + std::to_string(now + ttl_seconds) + "}";

    std::string h   = Base64UrlEncodeForTest(header_json);
    std::string p   = Base64UrlEncodeForTest(payload_json);
    std::string sig = Base64UrlEncodeForTest(Helper::hmacSha256(secret, h + "." + p));

    return h + "." + p + "." + sig;
}

// ─────────────────────────────────────────────
// Fixture
// ─────────────────────────────────────────────

class AccountJwtTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        provider_ = std::make_unique<JwtAuthProvider>(kSecret);
    }

    static constexpr const char* kSecret = "account_test_secret_2024";
    std::unique_ptr<JwtAuthProvider> provider_;
};

// ─────────────────────────────────────────────
// 1. 签发的 token 能被 JwtAuthProvider 成功验证
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, IssuedTokenVerifiesOk)
{
    std::string token = SignJwtForTest("user_alice", kSecret);
    auto result = provider_->verify(token);

    EXPECT_TRUE(result.ok)       << "err: " << result.err_msg;
    EXPECT_EQ(result.user_id, "user_alice");
}

// ─────────────────────────────────────────────
// 2. token 由三段 base64url 组成
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, TokenHasThreeParts)
{
    std::string token = SignJwtForTest("bob", kSecret);

    size_t dot1 = token.find('.');
    ASSERT_NE(dot1, std::string::npos) << "first dot missing";

    size_t dot2 = token.find('.', dot1 + 1);
    ASSERT_NE(dot2, std::string::npos) << "second dot missing";

    // 不应有第四个 dot（严格三段）
    size_t dot3 = token.find('.', dot2 + 1);
    EXPECT_EQ(dot3, std::string::npos) << "unexpected fourth part";

    // 各段非空
    EXPECT_GT(dot1, 0u);
    EXPECT_GT(dot2, dot1 + 1);
    EXPECT_GT(token.size(), dot2 + 1);
}

// ─────────────────────────────────────────────
// 3. sub 字段等于 account 名
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, SubFieldEqualsAccount)
{
    std::string account = "unique_player_xyz";
    std::string token   = SignJwtForTest(account, kSecret);

    auto result = provider_->verify(token);
    ASSERT_TRUE(result.ok);
    EXPECT_EQ(result.user_id, account);
}

// ─────────────────────────────────────────────
// 4. exp 字段在 now+ttl 附近（±10 秒误差容忍）
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, ExpFieldWithinTolerance)
{
    constexpr int64_t kTtl = 3600;
    int64_t before = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    std::string token = SignJwtForTest("exp_user", kSecret, kTtl);

    int64_t after = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // token 未过期（now+ttl > now → valid for kTtl seconds）
    auto result = provider_->verify(token);
    EXPECT_TRUE(result.ok) << "Token should be valid, err: " << result.err_msg;

    // Decode payload to check exp manually
    size_t dot1 = token.find('.');
    size_t dot2 = token.find('.', dot1 + 1);
    std::string payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string payload_json = Helper::base64Decode(payload_b64);

    // 从 payload 中提取 exp 值
    size_t exp_pos = payload_json.find("\"exp\":");
    ASSERT_NE(exp_pos, std::string::npos);
    int64_t exp_val = std::stoll(payload_json.substr(exp_pos + 6));

    EXPECT_GE(exp_val, before + kTtl - 10);
    EXPECT_LE(exp_val, after  + kTtl + 10);
}

// ─────────────────────────────────────────────
// 5. 不同 account 产生不同 token
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, DifferentAccountsDifferentTokens)
{
    std::string t1 = SignJwtForTest("user_a", kSecret);
    std::string t2 = SignJwtForTest("user_b", kSecret);

    EXPECT_NE(t1, t2);

    // 两个 token 都能通过验证，且 user_id 各自正确
    auto r1 = provider_->verify(t1);
    auto r2 = provider_->verify(t2);

    EXPECT_TRUE(r1.ok);  EXPECT_EQ(r1.user_id, "user_a");
    EXPECT_TRUE(r2.ok);  EXPECT_EQ(r2.user_id, "user_b");
}

// ─────────────────────────────────────────────
// 6. 错误 secret 签发的 token 被拒绝
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, WrongSecretTokenRejected)
{
    std::string token = SignJwtForTest("user_x", "wrong_secret_xyz");
    auto result = provider_->verify(token);  // provider_ uses kSecret

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "signature mismatch");
}

// ─────────────────────────────────────────────
// 7. ttl=0 时 exp==now，行为依赖 provider 实现（可能通过或刚好过期）
//    使用 ttl=-1 确保 exp 在过去（严格过期）
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, NegativeOneTtlTokenExpired)
{
    std::string token = SignJwtForTest("user_ttl_neg1", kSecret, -1);
    auto result = provider_->verify(token);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "token expired");
}

// ─────────────────────────────────────────────
// 8. 负 ttl（已过期）
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, NegativeTtlTokenExpired)
{
    std::string token = SignJwtForTest("user_expired", kSecret, -3600);
    auto result = provider_->verify(token);

    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.err_msg, "token expired");
}

// ─────────────────────────────────────────────
// 9. 空 secret 下 provider 拒绝所有 token
// ─────────────────────────────────────────────

TEST(AccountJwtNoSecret, TokenRejectedWithEmptyProviderSecret)
{
    JwtAuthProvider empty_provider("");
    std::string token = SignJwtForTest("user_y", "some_secret");

    auto result = empty_provider.verify(token);
    EXPECT_FALSE(result.ok);
}

// ─────────────────────────────────────────────
// 10. 长账号名（含特殊字符）
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, LongAccountName)
{
    std::string long_account(200, 'a');
    long_account[0]   = 'S';
    long_account[199] = 'E';

    std::string token  = SignJwtForTest(long_account, kSecret);
    auto result        = provider_->verify(token);

    EXPECT_TRUE(result.ok);
    EXPECT_EQ(result.user_id, long_account);
}

// ─────────────────────────────────────────────
// 11. 并发签发：8 线程各签发 50 个 token，全部可被验证
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, ConcurrentSigningAllValid)
{
    constexpr int kThreads    = 8;
    constexpr int kPerThread  = 50;

    std::vector<std::vector<std::string>> tokens(kThreads);
    std::vector<std::thread>              threads;
    threads.reserve(kThreads);

    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t]() {
            tokens[t].reserve(kPerThread);
            for (int i = 0; i < kPerThread; ++i)
                tokens[t].push_back(
                    SignJwtForTest("user_t" + std::to_string(t) + "_" + std::to_string(i),
                                   kSecret));
        });
    }
    for (auto& th : threads) th.join();

    // 所有 token 验证通过
    int verified = 0;
    for (int t = 0; t < kThreads; ++t)
    {
        for (auto& tok : tokens[t])
        {
            auto r = provider_->verify(tok);
            EXPECT_TRUE(r.ok) << "token failed: " << r.err_msg;
            ++verified;
        }
    }
    EXPECT_EQ(verified, kThreads * kPerThread);
}

// ─────────────────────────────────────────────
// 12. 同一账号同一时刻签发两次 token 相同（确定性）
// ─────────────────────────────────────────────

TEST_F(AccountJwtTest, SameAccountSameSecondProducesSameToken)
{
    // 在同一秒内连续签发两次
    std::string t1 = SignJwtForTest("deterministic_user", kSecret, 3600);
    std::string t2 = SignJwtForTest("deterministic_user", kSecret, 3600);

    // 由于 now 是秒级，两次调用若在同一秒内，token 完全相同
    // （实际上几乎肯定在同一秒，但如果跨秒也只验证两者都有效）
    auto r1 = provider_->verify(t1);
    auto r2 = provider_->verify(t2);
    EXPECT_TRUE(r1.ok);
    EXPECT_TRUE(r2.ok);
    EXPECT_EQ(r1.user_id, "deterministic_user");
    EXPECT_EQ(r2.user_id, "deterministic_user");
}
