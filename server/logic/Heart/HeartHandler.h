#include "ILogic.h"
#include "network/AppMsg.h"

class HeartHandler : public ILogic
{
public:
    HeartHandler();

    void registerHandlers() override;

private:
    void onHeart(const AppMsg &msg);
};