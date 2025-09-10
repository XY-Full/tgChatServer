#include "IBus.h"
#include "BusNet.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "Log.h"
#include "coroutine/CoroutineScheduler.h"
#include "google/protobuf/message.h"
#include "network/AppMsg.h"
#include "network/MsgWrapper.h"
#include "shm/shm_ringbuffer.h"
#include "shm/shm_slab.h"
#include "ss_msg_id.pb.h"
#include <arpa/inet.h>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace IBus
{

uint64_t BusClient::now_seq_ = 0;

class BusClient::Impl
{
public:
    explicit Impl(const ConfigManager &config_manager) : opts_(std::make_shared<Options>(config_manager)), running_(false), ready_(false)
    {
        message_handlers_map_[Type::C2S] = std::bind(&Impl::HandleCSMsg, this, std::placeholders::_1);
        message_handlers_map_[Type::S2C] = std::bind(&Impl::HandleCSMsg, this, std::placeholders::_1);
        message_handlers_map_[Type::S2SReq] = std::bind(&Impl::HandleRequest, this, std::placeholders::_1);
        message_handlers_map_[Type::S2SRsp] = std::bind(&Impl::HandleResponse, this, std::placeholders::_1);
        ILOG << "BusClient::Impl created with client_id: " << opts_->client_id;
    }

    ~Impl()
    {
        Stop();
        ILOG << "BusClient::Impl destroyed";
    }

    bool Start()
    {
        if (running_.exchange(true))
        {
            WLOG << "BusClient is already running";
            return false;
        }

        // 初始化共享内存
        if (!InitSharedMemory())
        {
            ELOG << "Failed to initialize shared memory";
            running_ = false;
            return false;
        }

        bus_net_ = std::make_unique<BusNet>();
        msg_dispatcher_ = std::make_unique<MsgDispatcher>();

        // 启动工作线程
        io_thread_ = std::thread(&Impl::IoLoop, this);

        ready_ = true;
        ready_cv_.notify_all();

        bus_net_->init(opts_);
        RegistEvent(SSMsgID::SS_TRACE_ROUTE, std::bind(&Impl::onTraceRouteResp, this, std::placeholders::_1));

        ILOG << "BusClient started successfully";
        return true;
    }

    void Stop()
    {
        if (!running_.exchange(false))
            return;

        ready_ = false;
        ready_cv_.notify_all();

        if (io_thread_.joinable())
            io_thread_.join();

        Cleanup();

        ILOG << "BusClient stopped";
    }

    bool WaitReady(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(ready_mutex_);
        return ready_cv_.wait_for(lock, timeout, [this] { return ready_.load(); });
    }

    bool SendToNode(const std::string &service_name, const google::protobuf::Message &message)
    {
        if (!ready_)
        {
            ELOG << "Cannot send, client not ready";
            return false;
        }

        auto pack = Helper::CreateSSPack(message);
        bool result = bus_net_->sendMsgTo(service_name, *pack);
        if (!result)
        {
            ELOG << "Failed to push message to local ring buffer";
        }

        return result;
    }

    bool SendToNode(const std::string_view &service_name, const AppMsgWrapper &pack)
    {
        if (!ready_)
        {
            ELOG << "Cannot send, client not ready";
            return false;
        }

        bool result = bus_net_->sendMsgTo(service_name, pack);
        if (!result)
        {
            ELOG << "Failed to push message to local ring buffer";
        }

        return result;
    }

    bool RegistEvent(uint32_t msg_id, const MessageHandler &handler)
    {
        msg_dispatcher_->RegistEvent(msg_id, handler);

        ILOG << "RegistEvent to msg_id: " << msg_id;
        return true;
    }

    bool UnregistEvent(uint32_t msg_id)
    {
        msg_dispatcher_->UnregistEvent(msg_id);

        ILOG << "UnregistEvent from msg_id: " << msg_id;
        return true;
    }

    bool SSRegistEvent(uint32_t msg_id, const MessageHandler &handler)
    {
        ss_msg_dispatcher_->RegistEvent(msg_id, handler);

        ILOG << "SSRegistEvent to msg_id: " << msg_id;
        return true;
    }

    bool SSUnregistEvent(uint32_t msg_id)
    {
        ss_msg_dispatcher_->UnregistEvent(msg_id);

        ILOG << "SSUnregistEvent from msg_id: " << msg_id;
        return true;
    }

    AppMsgPtr Request(const std::string &service_name, const google::protobuf::Message &message)
    {
        if (!ready_)
        {
            ELOG << "Cannot reply, client not ready";
            return nullptr;
        }

        auto pack = Helper::CreateSSPack(message);
        reinterpret_cast<AppMsg *>(GlobalSpace()->shm_slab_.off2ptr(pack->offset_))->co_id_ = CUR_CO_ID;

        bool result = SendToNode(service_name, *pack);
        if (!result)
        {
            ELOG << "Failed to push message to local ring buffer";
        }

        auto rsp_msg = CoroutineScheduler::yield();
        if (!rsp_msg)
        {
            ELOG << "Failed to request " << service_name;
            throw std::runtime_error("Failed to request " + service_name);
        }

        return rsp_msg;
    }

    // 此处在Recv的位置进行调用，如果是点对点通信，直接将包发回给对端
    bool Reply(const AppMsg &req_msg, const google::protobuf::Message &rsp_msg)
    {
        if (!ready_)
        {
            ELOG << "Cannot reply, client not ready";
            return false;
        }

        auto co_id = req_msg.co_id_;
        auto pack = Helper::CreateSSPack(rsp_msg, Type::S2SRsp, co_id);

        bool result = SendToNode(req_msg.src_name_, *pack);
        if (!result)
        {
            ELOG << "Failed to send message when reply to " << co_id;
            return result;
        }

        return result;
    }

    void GetStats() const
    {
        // Stats s;
        // s.local_messages = stats_.local_messages.load();
        // s.remote_messages = stats_.remote_messages.load();
        // s.errors = stats_.errors.load();
        // return s;
    }

private:
    bool InitSharedMemory()
    {
        // 创建或连接到共享内存
        std::string shm_name = "/ibus_" + opts_->client_id;
        local_ring_ = std::make_unique<ShmRingBuffer<AppMsgWrapper>>(shm_name, opts_->local_ring_size);

        ILOG << "Shared memory initialized: " << shm_name;
        return true;
    }

    void IoLoop()
    {
        ILOG << "Starting IO loop";

        while (running_)
        {
            AppMsgWrapper msg;
            if (local_ring_->Pop(msg))
            {
                auto offset = msg.offset_;
                auto pack = reinterpret_cast<AppMsg *>(GlobalSpace()->shm_slab_.off2ptr(offset));

                auto packPtr = std::shared_ptr<AppMsg>(
                    pack, [offset](AppMsg *p) { GlobalSpace()->shm_slab_.Free(offset, p->header_.pack_len_); });

                // 将处理response的函数放在协程中处理
                co_scheduler_->schedule([&]() { message_handlers_map_[packPtr->header_.type_](packPtr); });
            }
        }

        ILOG << "Exiting IO loop";
    }

    void HandleRequest(AppMsgPtr msg)
    {
        if (msg->header_.type_ != Type::S2SReq)
            return;

        ss_msg_dispatcher_->onMsg(msg);
    }

    void HandleResponse(AppMsgPtr msg)
    {
        if (msg->header_.type_ != Type::S2SRsp)
            return;

        auto co_id = msg->co_id_;
        if (co_id == 0)
        {
            ELOG << "Invalid co_id: " << co_id;
            return;
        }

        co_scheduler_->resume(co_id, msg);
    }

    void HandleCSMsg(AppMsgPtr msg)
    {
        if (msg->header_.type_ != Type::C2S && msg->header_.type_ != Type::S2C)
            return;

        msg_dispatcher_->onMsg(msg);
    }

    void onTraceRouteResp(AppMsgPtr msg)
    {
        bus_net_->onRecvRouteCache(msg);
    }

    void Cleanup()
    {
        local_ring_.reset();
        ILOG << "Cleanup completed";
    }

    std::shared_ptr<Options> opts_;
    std::atomic<bool> running_;
    std::atomic<bool> ready_;
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;

    // 本地收消息缓存队列
    std::unique_ptr<ShmRingBuffer<AppMsgWrapper>> local_ring_;
    // 接受消息线程
    std::thread io_thread_;

    // 与客户端通信时注册的分发器
    std::unique_ptr<MsgDispatcher> msg_dispatcher_;
    // 与服务端通信时注册的分发器
    std::unique_ptr<MsgDispatcher> ss_msg_dispatcher_;

    // 不同bus之间通信的网络组件
    std::unique_ptr<BusNet> bus_net_;

    // 各种请求对应的处理函数表
    std::unordered_map<Type, std::function<void(AppMsgPtr msg)>> message_handlers_map_;

    // 协程调度器
    std::unique_ptr<CoroutineScheduler> co_scheduler_;
};

// BusClient 包装实现
BusClient::BusClient(const ConfigManager &config_manager) : impl_(std::make_unique<Impl>(config_manager))
{
}
BusClient::~BusClient() = default;
BusClient::BusClient(BusClient &&) noexcept = default;
BusClient &BusClient::operator=(BusClient &&) noexcept = default;

bool BusClient::Start()
{
    return impl_->Start();
}
void BusClient::Stop()
{
    impl_->Stop();
}
bool BusClient::WaitReady(std::chrono::milliseconds timeout)
{
    return impl_->WaitReady(timeout);
}

// BusClient公共接口实现
bool BusClient::SendToNode(const std::string &service_name, const google::protobuf::Message &message)
{
    return impl_->SendToNode(service_name, message);
}

bool BusClient::RegistEvent(uint32_t msg_id, const MessageHandler &handler)
{
    return impl_->RegistEvent(msg_id, handler);
}

bool BusClient::UnregistEvent(uint32_t msg_id)
{
    return impl_->UnregistEvent(msg_id);
}

AppMsgPtr BusClient::Request(const std::string &service_name, const google::protobuf::Message &message)
{
    return impl_->Request(service_name, message);
}

bool BusClient::Reply(const AppMsg &req_msg, const google::protobuf::Message &msg)
{
    return impl_->Reply(req_msg, msg);
}

void BusClient::GetStats() const
{
    impl_->GetStats();
}
} // namespace IBus