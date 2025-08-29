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

    uint8_t msg_name_len;   // 表示消息的前n个字节是消息名
};