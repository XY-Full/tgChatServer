#pragma once
#include "IBusNet.h"

class BusClientNet : public IBusNet
{
public:
    virtual void genServiceInfo() override;
    
    virtual bool sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg = true) override;
    
    virtual void broadCast(const AppMsgWrapper &msg) override;
    
    virtual bool sendMsgToGroup(const std::string_view &groupName, const AppMsgWrapper &msg) override;
};
