
#include "HeartHandler.h"
#include "GlobalSpace.h"
#include "ILogic.h"
#include "gate.pb.h"

HeartHandler::HeartHandler()
{
    registerHandlers();
}

void HeartHandler::registerHandlers()
{
    GlobalSpace()->bus_->RegistMessage(MsgID::CS_HEART_BEAT, std::bind(&HeartHandler::onHeart, this, std::placeholders::_1));
}

void HeartHandler::onHeart(const AppMsg &msg)
{
    PROCESS_NETPACK_BEGIN(Heart);

    response->set_err(ErrorCode::Error_success);
    response->set_timestamp(time(nullptr));

    PROCESS_NETPACK_END();
}