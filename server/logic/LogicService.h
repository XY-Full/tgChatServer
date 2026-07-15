#pragma once
#include "app/IApp.h"
#include "bus/IBus.h"
#include "ModuleManager.h"
#include "network/TcpServer.h"
#include <memory>

class LogicApp : public IApp
{
public:
    LogicApp() : IApp("LogicService") {}

    static LogicApp &getInstance()
    {
        static LogicApp instance;
        return instance;
    }

    virtual bool onInit() override final
    {
        // 先注册各业务模块的消息处理器，再启动 bus，避免早到消息找不到 handler
        moduleMgr_.registerAllModule();

        if (!GlobalSpace()->bus_->Start())
        {
            ELOG << "LogicApp: bus start failed";
            return false;
        }
        return true;
    }

    virtual void onTick(uint32_t delta_ms) override final
    {
        // 定期检查服务健康状态等
        // 可以在这里添加心跳检测、连接维护等逻辑
    }

    virtual void onCleanup() override final
    {
        ILOG << "LogicApp: Cleanup completed";
    }

    virtual bool onReload() override final
    {
        // 重新加载配置
        ILOG << "LogicApp: Reloading configuration";
        return true;
    }

    virtual bool onMessageLoop() override final
    {
        // 主消息循环
        // BusdNet的messageForwardLoop已在独立线程运行
        return true;
    }

private:
    ModuleManager moduleMgr_;
};
