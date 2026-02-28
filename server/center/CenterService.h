#include "GlobalSpace.h"
#include "app/IApp.h"
#include "ServiceRegistry.h"
#include "HttpServer.h"
#include "TcpRegistrar.h"

class CenterApp : public IApp
{
public:
    CenterApp() : IApp("CenterService") {}

    static CenterApp &getInstance()
    {
        static CenterApp instance;
        return instance;
    }

    virtual bool onInit() override final
    {
        // 启动bus客户端
        GlobalSpace()->bus_->Start();

        // 1. 初始化服务注册中心
        serviceRegistry_ = std::make_unique<ServiceRegistry>();

        // 2. 初始化并启动HTTP服务器
        // HttpServer 依赖 ServiceRegistry, HealthChecker, LBFactory
        // LBFactory 在HttpServer内部创建和管理
        // httpServer_ = std::make_unique<HttpServer>(*serviceRegistry_, 8080); // 默认端口8080
        // httpServer_->start();

        // 4. 初始化并启动TCP注册器
        // TcpRegistrar 依赖 ServiceRegistry
        tcpRegistrar_ = std::make_unique<TcpRegistrar>(*serviceRegistry_, 9090); // 默认端口9090
        tcpRegistrar_->start();

        return true;
    }

    virtual void onTick(uint32_t delta_ms) override final
    {
        // cleanup_expired() 已迁移至 TcpRegistrar::onUpdateServiceStatus，
        // 每次服务心跳时自动触发，无需在此额外调用
    }

    virtual void onCleanup() override final
    {
        ILOG << "CenterApp::onCleanup() called";
        
        // 1. 停止TCP注册器
        if (tcpRegistrar_) {
            ILOG << "Stopping TcpRegistrar...";
            tcpRegistrar_->stop();
            ILOG << "TcpRegistrar stopped";
            tcpRegistrar_.reset();
        }

        // 2. 停止HTTP服务器
        // if (httpServer_) {
        //     httpServer_->stop();
        //     httpServer_.reset();
        // }

        // 3. 清理服务注册中心
        if (serviceRegistry_) {
            ILOG << "Resetting ServiceRegistry...";
            serviceRegistry_.reset();
            ILOG << "ServiceRegistry reset";
        }
        
        ILOG << "CenterApp::onCleanup() completed";
    }

    virtual bool onReload() override final
    {
        // 重新加载逻辑，例如重新读取配置等
        // 对于服务发现，可能需要重新初始化某些组件或刷新配置
        // 这里暂时返回true，表示支持重新加载
        return true;
    }

    virtual bool onMessageLoop() override final
    {
        // 消息循环，如果应用程序有自己的消息处理机制，可以在这里实现
        // 对于服务发现，HttpServer和TcpRegistrar通常会在自己的线程中处理消息
        // 这里可以用于处理一些主线程的事件或保持主线程活跃
        return true;
    }

private:
    std::unique_ptr<ServiceRegistry> serviceRegistry_;
    // std::unique_ptr<HttpServer> httpServer_;
    std::unique_ptr<TcpRegistrar> tcpRegistrar_;
};
