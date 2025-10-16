#include "app/IApp.h"

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
        return true;
    }

    virtual void onTick(uint32_t delta_ms) override final
    {
    }

    virtual void onCleanup() override final
    {
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

};
