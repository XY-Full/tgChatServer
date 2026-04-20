#pragma once
/**
 * AccountService — JWT 签发微服务
 *
 * 职责：
 *   - 监听 bus，注册 CS_PLAYER_APPLY_TOKEN (msg_id=1) handler
 *   - 收到请求后，从 request.account 取账号名，签发 HS256 JWT
 *   - 通过 bus->Reply 将 PlayerApplyToken::Response{err=0, token=jwt} 发回 connd
 *   - connd 收到后通过 onDownstreamMessage 推给客户端
 *
 * 消息流：
 *   Client --CS_PLAYER_APPLY_TOKEN(account="foo")--> connd::forwardToAccount()
 *       --ForwardRawAppMsg("AccountService")--> AccountService::onApplyToken()
 *       --Reply()--> connd::onDownstreamMessage() --listener->send()--> Client
 *
 * config.json 字段：
 *   auth.jwt.secret      : HMAC-SHA256 密钥（与 connd/JwtAuthProvider 一致）
 *   auth.jwt.ttl_seconds : token 有效期（默认 86400 秒 = 24 小时）
 */

#include "app/IApp.h"
#include "bus/IBus.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "Log.h"
#include "network/AppMsg.h"
#include "login.pb.h"
#include "msg_id.pb.h"
#include "err_code.pb.h"
#include <memory>
#include <string>

class AccountApp : public IApp
{
public:
    AccountApp() : IApp("AccountService") {}

    static AccountApp& getInstance()
    {
        static AccountApp instance;
        return instance;
    }

    virtual bool onInit() override final;
    virtual void onTick(uint32_t /*delta_ms*/) override final {}
    virtual void onCleanup() override final;
    virtual bool onReload() override final { return true; }
    virtual bool onMessageLoop() override final { return true; }

private:
    /**
     * @brief 处理客户端申请 JWT token 的请求
     */
    void onApplyToken(const AppMsg& msg);

    /**
     * @brief 根据账号名签发 JWT（HS256）
     * @param account 账号名（填入 sub 字段）
     * @return JWT token 字符串
     */
    std::string signJwt(const std::string& account);
};
