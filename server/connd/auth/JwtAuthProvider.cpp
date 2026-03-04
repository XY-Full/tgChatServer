#include "JwtAuthProvider.h"
#include "Log.h"
#include "../../third/nlohmann/json.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <openssl/crypto.h>
#include <string>
#include "Helper.h"
#include <vector>

using json = nlohmann::json;

// ─────────────────────────────────────────────
// JwtAuthProvider
// ─────────────────────────────────────────────

JwtAuthProvider::JwtAuthProvider(const ConfigManager& config)
{
    secret_ = config.getValue<std::string>("auth.jwt.secret", "");
    if (secret_.empty())
        WLOG << "JwtAuthProvider: jwt.secret is empty, all tokens will fail verification";
}

AuthResult JwtAuthProvider::verify(const std::string& token, const std::string& /*platform*/)
{
    AuthResult result;

    // ── 拆分三段 ──
    auto dot1 = token.find('.');
    if (dot1 == std::string::npos) { result.err_msg = "invalid jwt format"; return result; }
    auto dot2 = token.find('.', dot1 + 1);
    if (dot2 == std::string::npos) { result.err_msg = "invalid jwt format"; return result; }

    std::string header_b64    = token.substr(0, dot1);
    std::string payload_b64   = token.substr(dot1 + 1, dot2 - dot1 - 1);
    std::string signature_b64 = token.substr(dot2 + 1);

    // ── 验签 ──
    std::string signing_input = header_b64 + "." + payload_b64;
    std::string computed_sig  = Helper::hmacSha256(secret_, signing_input);

    std::string expected_sig = Helper::base64Decode(signature_b64);

    // 恒定时间比较，防时序攻击
    if (computed_sig.size() != expected_sig.size() ||
        CRYPTO_memcmp(computed_sig.data(), expected_sig.data(), computed_sig.size()) != 0)
    {
        result.err_msg = "signature mismatch";
        return result;
    }

    // ── 解析 payload ──
    std::string payload_json = Helper::base64Decode(payload_b64);
    try
    {
        auto payload = json::parse(payload_json);

        // 检查过期时间
        if (payload.contains("exp"))
        {
            int64_t exp = payload["exp"].get<int64_t>();
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now > exp)
            {
                result.err_msg = "token expired";
                return result;
            }
        }

        // 提取 user_id（优先 sub，其次 user_id 字段）
        if (payload.contains("sub"))
            result.user_id = payload["sub"].get<std::string>();
        else if (payload.contains("user_id"))
            result.user_id = payload["user_id"].get<std::string>();
        else
        {
            result.err_msg = "missing sub/user_id in payload";
            return result;
        }
    }
    catch (const std::exception& e)
    {
        result.err_msg = std::string("payload parse error: ") + e.what();
        return result;
    }

    result.ok = true;
    return result;
}
