#include "AccountService.h"
#include "app/IApp.h"
#include <chrono>
#include <cstdint>
#include <string>

// ─────────────────────────────────────────────
// 内部 Base64url 编码（与 JwtAuthProvider 对称）
// ─────────────────────────────────────────────

static std::string base64UrlEncode(const std::string& input)
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

    // 转换为 base64url
    for (auto& ch : out)
    {
        if      (ch == '+') ch = '-';
        else if (ch == '/') ch = '_';
    }
    // 去掉尾部 padding
    while (!out.empty() && out.back() == '=')
        out.pop_back();

    return out;
}

// ─────────────────────────────────────────────
// AccountApp 实现
// ─────────────────────────────────────────────

bool AccountApp::onInit()
{
    GlobalSpace()->bus_->Start();

    // 注册 CS_PLAYER_APPLY_TOKEN handler
    GlobalSpace()->bus_->RegistMessage(
        static_cast<uint32_t>(MsgID::CS_PLAYER_APPLY_TOKEN),
        [this](const AppMsg& msg) { this->onApplyToken(msg); });

    ILOG << "AccountApp: initialized, listening for CS_PLAYER_APPLY_TOKEN (msg_id="
         << static_cast<int>(MsgID::CS_PLAYER_APPLY_TOKEN) << ")";
    return true;
}

void AccountApp::onCleanup()
{
    ILOG << "AccountApp: cleanup completed";
}

void AccountApp::onApplyToken(const AppMsg& msg)
{
    cs::PlayerApplyToken pb;
    if (!pb.ParsePartialFromArray(msg.data_, msg.data_len_))
    {
        WLOG << "AccountApp: failed to parse PlayerApplyToken request";
        return;
    }

    const std::string& account = pb.request().account();
    ILOG << "AccountApp: apply token request, account=" << account
         << " conn_id=" << msg.header_.conn_id_;

    cs::PlayerApplyToken rsp;
    auto* rsp_body = rsp.mutable_response();

    if (account.empty())
    {
        WLOG << "AccountApp: empty account, rejecting";
        rsp_body->set_err(ErrorCode::Error_auth_failed);
    }
    else
    {
        rsp_body->set_err(ErrorCode::Error_success);
        rsp_body->set_token(signJwt(account));
        ILOG << "AccountApp: token issued for account=" << account;
    }

    // Reply 回 connd（通过 bus Reply 机制，src_name_ = "ConndService"）
    GlobalSpace()->bus_->Reply(msg, rsp);
}

std::string AccountApp::signJwt(const std::string& account)
{
    const std::string secret =
        GlobalSpace()->configMgr_->getValue<std::string>("auth.jwt.secret", "");
    int64_t ttl =
        GlobalSpace()->configMgr_->getValue<int64_t>("auth.jwt.ttl_seconds", 86400);

    if (secret.empty())
        WLOG << "AccountApp: jwt.secret is empty, token will fail verification";

    int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    // Header
    const std::string header_json  = R"({"alg":"HS256","typ":"JWT"})";
    // Payload：sub = account，exp = now + ttl
    const std::string payload_json =
        R"({"sub":")" + account +
        R"(","exp":)" + std::to_string(now + ttl) + "}";

    std::string h   = base64UrlEncode(header_json);
    std::string p   = base64UrlEncode(payload_json);
    std::string sig = base64UrlEncode(Helper::hmacSha256(secret, h + "." + p));

    return h + "." + p + "." + sig;
}

// ─────────────────────────────────────────────
// 程序入口
// ─────────────────────────────────────────────
IAPP_MAIN(AccountApp)
