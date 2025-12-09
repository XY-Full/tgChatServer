#include "BusClientNet.h"

bool BusClientNet::sendMsgByServiceInfo(const ss::ServiceInfo &info, const AppMsgWrapper &msg, bool delete_msg)
{
    std::string local_ip = "127.0.0.1";
    std::string remote_ip = info.ip_();

    if (local_ip == remote_ip)
    {
        // 如果没有打开对端的ShmRingBuffer，那么打开一哈子
        if (LocalServiceMap_.find(info.id_()) == LocalServiceMap_.end())
        {
            // 直接new一个ShmRingBuffer指向对端的ringbuffer
            LocalServiceMap_[info.id_()] = new ShmRingBuffer<AppMsgWrapper>(info.shm_recv_buffer_name_());
        }

        // 如果对方在本地，则直接推送到对端的ShmRingBuffer中
        LocalServiceMap_[info.id_()]->Push(msg);
    }
    else
    {
        // 对方不在本机则直接推送到Busd
        LocalBusdShmBuffer_->Push(msg);
    }
    // 发送完之后释放内存
    if (delete_msg)
        Helper::DeleteSSPack(msg);
    return true;
}