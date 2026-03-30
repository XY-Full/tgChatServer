#pragma once
#include "IAuthProvider.h"
#include "app/ConfigManager.h"
#include <string>

/**
 * @brief JWT HS256 鉴权提供者
 *
 * 支持标准 JWT 格式：header.payload.signature（Base64url 编码）
 *
 * 验证步骤：
 *   1. 拆分三段，base64url 解码 header 和 payload
 *   2. 用 HMAC-SHA256(secret, header.payload) 计算签名
 *   3. 与 signature 比较（恒定时间比较，防止时序攻击）
 *   4. 检查 payload 中的 exp 字段是否过期
 *   5. 从 payload 中提取 sub（user_id）
 *
 * 配置项（config.json）：
 *   auth.jwt.secret   : 签名密钥
 */
class JwtAuthProvider : public IAuthProvider
{
public:
    explicit JwtAuthProvider(const ConfigManager& config);

    /**
     * @brief 测试专用构造函数：直接传入 secret，不依赖 ConfigManager
     */
    explicit JwtAuthProvider(const std::string& secret_str);

    AuthResult  verify(const std::string& token,
                       const std::string& platform = "") override;
    std::string name() const override { return "jwt"; }

    std::string secret_;
};
