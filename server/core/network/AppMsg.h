#include "PackBase.h"

class AppMsg : public PackBase
{
public:
    AppMsg() = default;
    virtual ~AppMsg()
    {
        if(data_)
        {
            delete[] data_;
            data_ = nullptr;
        }
    }

    char src_name[16];      // 包来源服务名
    char dst_name[16];      // 包目的服务(名)
};