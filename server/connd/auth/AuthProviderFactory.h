#pragma once
#include "IAuthProvider.h"
#include "JwtAuthProvider.h"
#include "app/ConfigManager.h"
#include "Log.h"
#include <memory>
#include <string>

/**
 * @brief 鉴权提供者工厂
 *
 * 根据配置文件中 auth.provider 字段创建对应的 IAuthProvider 实例。
 *
 * 当前支持：
 *   "jwt"  → JwtAuthProvider
 *
 * 新增 SDK 步骤：
 *   1. 实现 IAuthProvider 接口
 *   2. 在 create() 中添加对应的 else if 分支
 */
class AuthProviderFactory
{
public:
    static std::unique_ptr<IAuthProvider> create(const ConfigManager& config)
    {
        std::string type = config.getValue<std::string>("auth.provider", "jwt");

        if (type == "jwt")
        {
            ILOG << "AuthProviderFactory: creating JwtAuthProvider";
            return std::make_unique<JwtAuthProvider>(config);
        }
        // 后续扩展示例：
        // else if (type == "wechat")
        //     return std::make_unique<WechatAuthProvider>(config);
        // else if (type == "firebase")
        //     return std::make_unique<FirebaseAuthProvider>(config);

        ELOG << "AuthProviderFactory: unknown provider type '" << type << "', fallback to jwt";
        return std::make_unique<JwtAuthProvider>(config);
    }
};
