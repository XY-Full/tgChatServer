#pragma once
#include <string>

/**
 * @brief 鉴权结果
 */
struct AuthResult
{
    bool        ok      = false;
    std::string user_id;      // 鉴权成功后的用户标识
    std::string err_msg;      // 鉴权失败原因
};

/**
 * @brief 鉴权提供者接口
 *
 * 新增 SDK 只需实现此接口并在 AuthProviderFactory 中注册即可。
 * 例如：
 *   - JwtAuthProvider   : 本地 JWT HMAC-SHA256 验签
 *   - WechatAuthProvider: 微信 code 换 openid
 *   - FirebaseProvider  : Firebase ID Token 验证
 */
class IAuthProvider
{
public:
    virtual ~IAuthProvider() = default;

    /**
     * @brief 验证 token
     * @param token     客户端提交的凭证字符串
     * @param platform  平台标识（可选，某些 provider 需要区分渠道）
     * @return AuthResult
     */
    virtual AuthResult verify(const std::string& token,
                              const std::string& platform = "") = 0;

    /**
     * @brief 返回 provider 名称（用于日志）
     */
    virtual std::string name() const = 0;
};
