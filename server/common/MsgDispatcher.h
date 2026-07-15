#include "../../public/proto_files//msg_mapping.h"
#include "../../public/proto_files//msg_mapping_ss.h"
#include "Log.h"
#include "google/protobuf/message.h"
#include <vector>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include "../core/network/AppMsg.h"

namespace google { namespace protobuf { class Message; } }

using MessageHandler = std::function<void(const AppMsg&)>;

class MsgDispatcher
{
public:
    bool RegistMessage(uint32_t msg_id, const MessageHandler &handler)
    {
        std::unique_lock lk(mu_);
        handlers_[msg_id].push_back(handler);

        ILOG << "Subscribed to msg_id: " << msg_id;
        return true;
    }

    bool UnregistMessage(uint32_t msg_id)
    {
        std::unique_lock lk(mu_);
        handlers_.erase(msg_id);

        ILOG << "Unsubscribed from msg_id: " << msg_id;
        return true;
    }

    bool onMsg(const AppMsg &msg)
    {
        // 拷贝 handler 列表后解锁再执行，避免回调内再注册/注销造成死锁
        std::vector<MessageHandler> handlers;
        {
            std::shared_lock lk(mu_);
            auto it = handlers_.find(msg.msg_id_);
            if (it == handlers_.end())
                return true;
            handlers = it->second;
        }

        for (const auto &handler : handlers)
        {
            try
            {
                handler(msg);
            }
            catch (const std::exception &e)
            {
                ELOG << "Handler exception: " << e.what();
                return false;
            }
        }
        return true;
    }

private:
    // 注册发生在各服务 onInit，而分发跑在 bus 线程，必须加锁
    mutable std::shared_mutex mu_;
    std::unordered_map<uint32_t, std::vector<MessageHandler>> handlers_;
};
