#pragma once
#include "app/IApp.h"
#include "bus/IBus.h"
#include "network/TcpServer.h"
#include <memory>

class BusdApp : public IApp
{
public:
    BusdApp() : IApp("BusdService") {}

    static BusdApp &getInstance()
    {
        static BusdApp instance;
        return instance;
    }

    virtual bool onInit() override final
    {
        // 初始化BusClient，标记为daemon模式
        bus_client_ = std::make_unique<IBus::BusClient>(getContext(), true);
        
        if (!bus_client_->Start())
        {
            ELOG << "BusdApp: Failed to start BusClient";
            return false;
        }

        if (!bus_client_->WaitReady(std::chrono::seconds(10)))
        {
            ELOG << "BusdApp: BusClient not ready within timeout";
            return false;
        }

        // 启动TCP服务器，接收其他机器busd的连接
        std::string listen_ip = getContext().getValue<std::string>("local_ip", "0.0.0.0");
        int32_t listen_port = getContext().getValue<int32_t>("local_port", 3099);
        
        tcp_server_ = std::make_unique<TcpServer>(
            listen_port,
            [this](uint64_t conn_id, std::shared_ptr<AppMsg> msg) {
                this->onRecvFromRemoteBusd(conn_id, msg);
            },
            "busd_tcp_recv",
            [this](uint64_t conn_id) {
                this->onConnectionClosed(conn_id);
            },
            [this](uint64_t conn_id) {
                this->onNewConnection(conn_id);
            }
        );

        tcp_server_->start();

        ILOG << "BusdApp: Initialized successfully, listening on " << listen_ip << ":" << listen_port;
        return true;
    }

    virtual void onTick(uint32_t delta_ms) override final
    {
        // 定期检查服务健康状态等
        // 可以在这里添加心跳检测、连接维护等逻辑
    }

    virtual void onCleanup() override final
    {
        if (tcp_server_)
        {
            tcp_server_->stop();
        }

        if (bus_client_)
        {
            bus_client_->Stop();
        }

        ILOG << "BusdApp: Cleanup completed";
    }

    virtual bool onReload() override final
    {
        // 重新加载配置
        ILOG << "BusdApp: Reloading configuration";
        return true;
    }

    virtual bool onMessageLoop() override final
    {
        // 主消息循环
        // BusdNet的messageForwardLoop已在独立线程运行
        return true;
    }

private:
    // 处理来自其他机器busd的消息
    void onRecvFromRemoteBusd(uint64_t conn_id, std::shared_ptr<AppMsg> app_msg)
    {
        // 转发到本地服务
        std::string dst_service(app_msg->dst_name_, strnlen(app_msg->dst_name_, sizeof(app_msg->dst_name_)));
        
        DLOG << "BusdApp: Received message from remote busd for service: " << dst_service 
             << ", conn_id: " << conn_id;

        // 消息已经在TCP层被处理并放入正确的队列
        // 这里可以添加额外的日志或统计逻辑
    }

    // 处理远程busd连接关闭
    void onConnectionClosed(uint64_t conn_id)
    {
        ILOG << "BusdApp: Remote busd connection closed, conn_id: " << conn_id;
        
        // 清理连接相关的资源
        // 可以添加重连逻辑或通知相关服务
    }

    // 处理新的远程busd连接
    void onNewConnection(uint64_t conn_id)
    {
        ILOG << "BusdApp: New remote busd connection established, conn_id: " << conn_id;
        
        // 可以在这里进行握手验证等操作
    }

private:
    std::unique_ptr<IBus::BusClient> bus_client_;
    std::unique_ptr<TcpServer> tcp_server_;
};
