#include "IBus.h"
#include "google/protobuf/message.h"
#include <arpa/inet.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include "../../common/Helper.h"
#include "../../common/GlobalSpace.h"
#include "../network/AppMsg.h"
#include "../network/MsgWrapper.h"
#include <condition_variable>

namespace IBus
{

uint64_t BusClient::now_seq_ = 0;

class BusClient::Impl
{
public:
    explicit Impl(const Options &opts) : opts_(opts), running_(false), ready_(false)
    {
        ILOG << "BusClient::Impl created with client_id: " << opts.client_id;
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

        // 启动工作线程
        io_thread_ = std::thread(&Impl::IoLoop, this);
        retry_thread_ = std::thread(&Impl::RetryLoop, this);

        ready_ = true;
        ready_cv_.notify_all();

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
        if (retry_thread_.joinable())
            retry_thread_.join();

        Cleanup();

        ILOG << "BusClient stopped";
    }

    bool WaitReady(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(ready_mutex_);
        return ready_cv_.wait_for(lock, timeout, [this] { return ready_.load(); });
    }

    bool SendToNode(const google::protobuf::Message &message)
    {
        if (!ready_)
        {
            ELOG << "Cannot send, client not ready";
            return false;
        }

        auto pack = Helper::CreateSSPack(message);
        bool result = local_ring_->Push(*pack);
        if (!result)
        {
            ELOG << "Failed to push message to local ring buffer";
        }

        return result;
    }

    bool RegistEvent(uint32_t msg_id, const MessageHandler &handler)
    {
        msg_dispatcher_.RegistEvent(msg_id, handler);

        ILOG << "RegistEvent to msg_id: " << msg_id;
        return true;
    }

    bool UnregistEvent(uint32_t msg_id)
    {
        msg_dispatcher_.UnregistEvent(msg_id);

        ILOG << "UnregistEvent from msg_id: " << msg_id;
        return true;
    }

    uint64_t Request(const google::protobuf::Message& message, std::chrono::milliseconds timeout = std::chrono::seconds(3))
    {
        if (!ready_)
        {
            ELOG << "Cannot reply, client not ready";
            return false;
        }

        auto pack = Helper::CreateSSPack(message);
        
        bool result = local_ring_->Push(*pack);
        if (!result)
        {
            ELOG << "Failed to push message to local ring buffer";
        }

        auto pending_request = std::make_shared<PendingRequest>();
        auto seq = reinterpret_cast<AppMsg*>(GlobalSpace()->shm_slab_.off2ptr(pack->offset_))->header_.seq_;
        pending_requests_[seq] = pending_request;
        std::unique_lock<std::mutex> lock(pending_request->mutex);
        pending_request->has_response.wait_for(lock, timeout);

        return 0;
    }

    // 此处在Recv的位置进行调用，如果是点对点通信，直接将包发回给对端
    // Demo
    bool Reply(uint32_t req_id, const google::protobuf::Message& message)
    {
        if (!ready_)
        {
            ELOG << "Cannot reply, client not ready";
            return false;
        }

        auto pack = Helper::CreateSSPack(message, req_id);
        
        bool result = local_ring_->Push(*pack);
        if (!result)
        {
            ELOG << "Failed to push message to local ring buffer";
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
        std::string shm_name = "/ibus_" + opts_.client_id;
        local_ring_ = std::make_unique<ShmRingBuffer<AppMsgWrapper>>(shm_name, opts_.local_ring_size);

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
                auto pack = reinterpret_cast<AppMsg *>(GlobalSpace()->shm_slab_.off2ptr(msg.offset_));
                if(pack->header_.type_ == Type::S2SRsp)
                {
                    HandleResponse(*pack);
                }
                else 
                {
                    msg_dispatcher_.onMsg(*pack);
                }
            }
        }

        ILOG << "Exiting IO loop";
    }

    void RetryLoop()
    {
        ILOG << "Starting retry loop";

        while (running_)
        {
            CleanupExpiredRequests();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        ILOG << "Exiting retry loop";
    }

    void HandleResponse(const AppMsg &msg)
    {
        if (msg.header_.type_ != Type::S2SRsp) return;

        auto it = pending_requests_.find(msg.header_.seq_);
        if (it != pending_requests_.end()) {
            it->second->has_response.notify_all();
        }
    }

    void CleanupExpiredRequests()
    {
    }

    void Cleanup()
    {
        local_ring_.reset();
        ILOG << "Cleanup completed";
    }

    uint64_t NextRequestId()
    {
        static std::atomic<uint64_t> counter{1};
        return counter.fetch_add(1);
    }

    struct PendingRequest
    {
        std::condition_variable has_response;
        std::mutex mutex;
    };

    Options opts_;
    std::atomic<bool> running_;
    std::atomic<bool> ready_;
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;

    std::unique_ptr<ShmRingBuffer<AppMsgWrapper>> local_ring_;
    std::thread io_thread_;
    std::thread retry_thread_;

    MsgDispatcher msg_dispatcher_;

    std::unordered_map<uint64_t, std::shared_ptr<PendingRequest>> pending_requests_;
    std::mutex pending_requests_mutex_;

    struct
    {
        std::atomic<uint64_t> local_messages{0};
        std::atomic<uint64_t> remote_messages{0};
        std::atomic<uint64_t> errors{0};
    } stats_;
};

// BusClient 包装实现
BusClient::BusClient(const Options &opts) : impl_(std::make_unique<Impl>(opts))
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
bool BusClient::SendToNode(const google::protobuf::Message &message)
{
    return impl_->SendToNode(message);
}

bool BusClient::RegistEvent(uint32_t msg_id, const MessageHandler &handler)
{
    return impl_->RegistEvent(msg_id, handler);
}

bool BusClient::UnregistEvent(uint32_t msg_id)
{
    return impl_->UnregistEvent(msg_id);
}

uint64_t BusClient::Request(const google::protobuf::Message &message, std::chrono::milliseconds timeout)
{
    return impl_->Request(message, timeout);
}

bool BusClient::Reply(uint64_t req_id, const google::protobuf::Message &msg)
{
    return impl_->Reply(req_id, msg);
}

void BusClient::GetStats() const
{
    impl_->GetStats();
}
} // namespace IBus