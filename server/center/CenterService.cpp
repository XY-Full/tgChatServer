#include "app/IApp.h"

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
        return true;
    }

    virtual bool onMessageLoop() override final
    {
        return true;
    }
};

IAPP_MAIN(CenterApp);