#include "iApp.h"

class LogicSvr : public IApp
{
    virtual bool onInit();
    virtual void onTick(uint32_t delta_ms);
    virtual void onCleanup();
    virtual bool onReload();
    virtual bool onMessageLoop();
};