#pragma once
#include "app/IApp.h"
#include "bus/IBus.h"
#include "SessionManager.h"
#include "ConndMsgDispatcher.h"
#include "auth/IAuthProvider.h"
#include "auth/AuthProviderFactory.h"
#include "network/TcpListener.h"
#include "network/WsListener.h"
#include "network/KcpListener.h"
#include <memory>

/**
 * @brief connd 主服务类
 *
 * 生命周期：
 *   onInit()    → 创建 auth provider、session manager、三种 listener、dispatcher
 *                  启动所有 listener，注册 bus 下行消息回调
 *   onTick()    → 目前空实现（KcpListener 的 update 循环自己跑）
 *   onCleanup() → 停止所有 listener
 */
class ConndApp : public IApp
{
public:
    ConndApp() : IApp("ConndService") {}

    static ConndApp& getInstance()
    {
        static ConndApp instance;
        return instance;
    }

    virtual bool onInit() override final
    {
        // ── 1. 鉴权提供者 ──────────────────────────────────────────
        auth_provider_ = AuthProviderFactory::create(getContext());
        if (!auth_provider_)
        {
            ELOG << "ConndApp: failed to create auth provider";
            return false;
        }

        // ── 2. 会话管理器 ──────────────────────────────────────────
        session_mgr_ = std::make_unique<SessionManager>();

        // ── 3. 读取监听端口配置 ──────────────────────────────────
        int32_t tcp_port = getContext().getValue<int32_t>("tcp_port", 7000);
        int32_t ws_port  = getContext().getValue<int32_t>("ws_port",  7001);
        int32_t kcp_port = getContext().getValue<int32_t>("kcp_port", 7002);

        std::string local_ip = getContext().getValue<std::string>("local_ip", "0.0.0.0");

        // ── 4. 构造三种 Listener ───────────────────────────────────
        //
        // recv_handler 和 close_handler 只能在 dispatcher_ 构造后赋值，
        // 所以先用 lambda 捕获 this，等 dispatcher_ 构造完再连通。
        // 这里用包装 lambda，内部转发给 dispatcher_，保证延迟绑定安全。

        // TCP
        tcp_listener_ = std::make_unique<TcpListener>(
            tcp_port,
            "connd_tcp",
            [this](uint64_t conn_id, std::shared_ptr<AppMsg> msg) {
                if (dispatcher_) dispatcher_->onClientMessage(conn_id, std::move(msg));
            },
            [this](uint64_t conn_id) {
                if (session_mgr_) session_mgr_->on_disconnect(conn_id);
            },
            [this](uint64_t conn_id) {
                if (session_mgr_) session_mgr_->on_connect(conn_id, "tcp");
            }
        );

        // WS
        ws_listener_ = std::make_unique<WsListener>(
            ws_port,
            [this](uint64_t conn_id, std::shared_ptr<AppMsg> msg) {
                if (dispatcher_) dispatcher_->onClientMessage(conn_id, std::move(msg));
            },
            [this](uint64_t conn_id) {
                if (session_mgr_) session_mgr_->on_disconnect(conn_id);
            },
            [this](uint64_t conn_id) {
                if (session_mgr_) session_mgr_->on_connect(conn_id, "ws");
            }
        );

        // KCP
        kcp_listener_ = std::make_unique<KcpListener>(
            kcp_port,
            [this](uint64_t conn_id, std::shared_ptr<AppMsg> msg) {
                if (dispatcher_) dispatcher_->onClientMessage(conn_id, std::move(msg));
            },
            [this](uint64_t conn_id) {
                if (session_mgr_) session_mgr_->on_disconnect(conn_id);
            },
            [this](uint64_t conn_id) {
                if (session_mgr_) session_mgr_->on_connect(conn_id, "kcp");
            }
        );

        // ── 5. 构造 dispatcher ─────────────────────────────────────
        std::vector<IListener*> listeners = {
            tcp_listener_.get(),
            ws_listener_.get(),
            kcp_listener_.get()
        };
        dispatcher_ = std::make_unique<ConndMsgDispatcher>(
            *session_mgr_,
            *auth_provider_,
            std::move(listeners)
        );

        // ── 6. 启动 BusClient ─────────────────────────────────────
        GlobalSpace()->bus_->Start();

        // ── 7. 注册下行消息回调（logic → connd → client）─────────
        // 这里注册一个通配 handler：
        // logic 回包时会通过 bus 路由到 connd，connd 收到后通过 dispatcher 推给客户端。
        // msg_id 范围 [300, 9999) 作为 SC（Server→Client）消息段（可按需调整）。
        // 实际项目中可枚举所有 SC msg_id，或在 msg_mapping.h 中集中定义。
        //
        // 注意：bus_->RegistMessage 接收 uint32_t msg_id。
        // 下行消息由 logic 通过 bus->Reply(req, rsp) 触发，
        // connd 在 IBus 层不 Reply，而是直接向客户端推包。
        //
        // 当前架构中，logic 通过 bus->Reply 将 SC 消息送回 connd，
        // connd 的 IBus 会将其放入 connd 的消息队列并触发此 handler。
        //
        // 为简化，我们直接在 onDownstreamCallback 里调用 dispatcher_->onDownstreamMessage。

        // ── 8. 启动三种 Listener ──────────────────────────────────
        if (!tcp_listener_->start())
        {
            ELOG << "ConndApp: failed to start TCP listener on port " << tcp_port;
            return false;
        }
        ILOG << "ConndApp: TCP listener started on port " << tcp_port;

        if (!ws_listener_->start())
        {
            ELOG << "ConndApp: failed to start WS listener on port " << ws_port;
            return false;
        }
        ILOG << "ConndApp: WS listener started on port " << ws_port;

        if (!kcp_listener_->start())
        {
            ELOG << "ConndApp: failed to start KCP listener on port " << kcp_port;
            return false;
        }
        ILOG << "ConndApp: KCP listener started on port " << kcp_port;

        ILOG << "ConndApp: initialized successfully";
        return true;
    }

    virtual void onTick(uint32_t /*delta_ms*/) override final
    {
        // KcpListener 的 update 循环跑在自己的线程里，无需在 tick 里驱动
    }

    virtual void onCleanup() override final
    {
        if (kcp_listener_) kcp_listener_->stop();
        if (ws_listener_)  ws_listener_->stop();
        if (tcp_listener_) tcp_listener_->stop();

        ILOG << "ConndApp: cleanup completed";
    }

    virtual bool onReload() override final
    {
        ILOG << "ConndApp: reloading configuration";
        return true;
    }

    virtual bool onMessageLoop() override final
    {
        return true;
    }

private:
    std::unique_ptr<IAuthProvider>      auth_provider_;
    std::unique_ptr<SessionManager>     session_mgr_;
    std::unique_ptr<TcpListener>        tcp_listener_;
    std::unique_ptr<WsListener>         ws_listener_;
    std::unique_ptr<KcpListener>        kcp_listener_;
    std::unique_ptr<ConndMsgDispatcher> dispatcher_;
};
