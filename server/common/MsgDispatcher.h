#include "../../public/proto_files//msg_mapping.h"
#include "../../public/proto_files//msg_mapping_ss.h"
#include "Log.h"
#include "google/protobuf/message.h"
#include <vector>
#include <functional>
#include "../core/network/AppMsg.h"

namespace google { namespace protobuf { class Message; } }

using MessageHandler = std::function<void(const AppMsg&)>;

class MsgDispatcher
{
public:
    bool RegistEvent(uint32_t msg_id, const MessageHandler &handler)
    {
        handlers_[msg_id].push_back(handler);

        ILOG << "Subscribed to msg_id: " << msg_id;
        return true;
    }

    bool UnregistEvent(uint32_t msg_id)
    {
        handlers_.erase(msg_id);

        ILOG << "Unsubscribed from msg_id: " << msg_id;
        return true;
    }

    bool onMsg(const AppMsg& msg)
    {
        auto it = handlers_.find(msg.msg_id_);
        if (it != handlers_.end())
        {
            for (const auto &handler : it->second)
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
        }
        return true;
    }

    std::unordered_map<uint32_t, std::vector<MessageHandler>> handlers_;
};