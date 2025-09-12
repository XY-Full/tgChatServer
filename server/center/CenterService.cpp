#include "app/IApp.h"
#include "ServiceRegistry.h"

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
        // 初始化服务注册中心
        serviceRegistry_ = std::make_unique<ServiceRegistry>();
        
        return true;
    }

    virtual void onTick(uint32_t delta_ms) override final
    {
        // 定期清理过期服务实例
        static uint32_t cleanupTimer = 0;
        cleanupTimer += delta_ms;
        if (cleanupTimer >= CLEANUP_INTERVAL_MS) {
            serviceRegistry_->cleanup_expired();
            cleanupTimer = 0;
        }
    }

    virtual void onCleanup() override final
    {
        // 清理服务注册中心
        if (serviceRegistry_) {
            serviceRegistry_.reset();
        }
    }

    virtual bool onReload() override final
    {
        return true;
    }

    virtual bool onMessageLoop() override final
    {
        return true;
    }

private:
    std::unique_ptr<ServiceRegistry> serviceRegistry_;
    static constexpr uint32_t CLEANUP_INTERVAL_MS = 5000; // 5秒清理一次
};

IAPP_MAIN(CenterApp);