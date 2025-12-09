#pragma once
#include "IBusNet.h"

class BusClientNet : public IBusNet
{
    virtual bool sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg = true);
};