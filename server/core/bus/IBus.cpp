// IBus.cpp
#include "IBus.h"
#include <cstring>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/socket.h>
#include <poll.h>

namespace IBus {

class BusClient::Impl {
public:
    explicit Impl(const Options& opts) : opts_(opts), running_(false), ready_(false) {
        ILOG << "BusClient::Impl created with client_id: " << opts.client_id;
    }

    ~Impl() {
        Stop();
        ILOG << "BusClient::Impl destroyed";
    }

    bool Start() {
        if (running_.exchange(true)) {
            WLOG << "BusClient is already running";
            return false;
        }

        // 初始化共享内存
        if (!InitSharedMemory()) {
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

    void Stop() {
        if (!running_.exchange(false)) return;

        ready_ = false;
        ready_cv_.notify_all();

        if (io_thread_.joinable()) io_thread_.join();
        if (retry_thread_.joinable()) retry_thread_.join();

        Cleanup();
        
        ILOG << "BusClient stopped";
    }

    bool WaitReady(std::chrono::milliseconds timeout) {
        std::unique_lock lock(ready_mutex_);
        return ready_cv_.wait_for(lock, timeout, [this] { return ready_; });
    }

    bool Publish(const std::string& topic, const void* data, size_t len, Message::Flags flags) {
        if (!ready_) {
            ELOG << "Cannot publish, client not ready";
            return false;
        }

        Message msg;
        msg.topic = topic;
        msg.data.assign(static_cast<const char*>(data), len);
        msg.flags = flags;
        msg.timestamp = std::chrono::steady_clock::now();

        bool result = local_ring_->Push(msg);
        if (!result) {
            ELOG << "Failed to push message to local ring buffer";
        }
        
        return result;
    }

    bool Subscribe(const std::string& topic, const MessageHandler& handler) {
        std::lock_guard lock(handlers_mutex_);
        handlers_[topic].push_back(handler);
        
        ILOG << "Subscribed to topic: " << topic;
        return true;
    }

    bool Unsubscribe(const std::string& topic) {
        std::lock_guard lock(handlers_mutex_);
        handlers_.erase(topic);
        
        ILOG << "Unsubscribed from topic: " << topic;
        return true;
    }

    uint64_t Request(const std::string& topic, const void* data, size_t len,
                     const ResponseHandler& handler, std::chrono::milliseconds timeout) {
        if (!ready_) {
            ELOG << "Cannot send request, client not ready";
            return 0;
        }

        uint64_t req_id = NextRequestId();
        Message msg;
        msg.topic = topic;
        msg.data.assign(static_cast<const char*>(data), len);
        msg.flags = Message::Flags::REQUEST;
        msg.request_id = req_id;
        msg.timestamp = std::chrono::steady_clock::now();

        {
            std::lock_guard lock(pending_requests_mutex_);
            pending_requests_[req_id] = {handler, std::chrono::steady_clock::now() + timeout};
        }

        if (!local_ring_->Push(msg)) {
            ELOG << "Failed to push request to local ring buffer";
            return 0;
        }
        
        ILOG << "Sent request with ID: " << req_id << " on topic: " << topic;
        return req_id;
    }

    bool Reply(uint64_t req_id, const void* data, size_t len) {
        if (!ready_) {
            ELOG << "Cannot send reply, client not ready";
            return false;
        }

        Message msg;
        msg.flags = Message::Flags::RESPONSE;
        msg.request_id = req_id;
        msg.data.assign(static_cast<const char*>(data), len);
        msg.timestamp = std::chrono::steady_clock::now();

        bool result = local_ring_->Push(msg);
        if (!result) {
            ELOG << "Failed to push reply to local ring buffer";
        }
        
        return result;
    }

    Stats GetStats() const {
        Stats s;
        s.local_messages = stats_.local_messages.load();
        s.remote_messages = stats_.remote_messages.load();
        s.errors = stats_.errors.load();
        return s;
    }

private:
    bool InitSharedMemory() {
        // 创建或连接到共享内存
        std::string shm_name = "/ibus_" + opts_.client_id;
        local_ring_ = std::make_unique<ShmRingBuffer<Message>>(shm_name, opts_.local_ring_size);
        
        if (!local_ring_->IsValid()) {
            ELOG << "Failed to initialize shared memory ring buffer";
            return false;
        }
        
        ILOG << "Shared memory initialized: " << shm_name;
        return true;
    }

    void IoLoop() {
        ILOG << "Starting IO loop";
        
        while (running_) {
            Message msg;
            if (local_ring_->Pop(msg, std::chrono::milliseconds(100))) {
                ProcessMessage(msg);
            }
        }
        
        ILOG << "Exiting IO loop";
    }

    void RetryLoop() {
        ILOG << "Starting retry loop";
        
        while (running_) {
            CleanupExpiredRequests();
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        ILOG << "Exiting retry loop";
    }

    void ProcessMessage(const Message& msg) {
        if (msg.flags & Message::Flags::RESPONSE) {
            HandleResponse(msg);
        } else {
            HandlePublish(msg);
        }
    }

    void HandlePublish(const Message& msg) {
        std::vector<MessageHandler> handlers;
        {
            std::lock_guard lock(handlers_mutex_);
            auto it = handlers_.find(msg.topic);
            if (it != handlers_.end()) {
                handlers = it->second;
            }
        }

        for (const auto& handler : handlers) {
            try {
                handler(msg);
            } catch (const std::exception& e) {
                ELOG << "Handler exception: " << e.what();
                stats_.errors++;
            }
        }

        stats_.local_messages++;
    }

    void HandleResponse(const Message& msg) {
        ResponseHandler handler;
        {
            std::lock_guard lock(pending_requests_mutex_);
            auto it = pending_requests_.find(msg.request_id);
            if (it != pending_requests_.end()) {
                handler = it->second.handler;
                pending_requests_.erase(it);
            }
        }

        if (handler) {
            try {
                handler(msg, ErrorCode::OK);
            } catch (const std::exception& e) {
                ELOG << "Response handler exception: " << e.what();
            }
        } else {
            WLOG << "No handler found for response with ID: " << msg.request_id;
        }
    }

    void CleanupExpiredRequests() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard lock(pending_requests_mutex_);
        for (auto it = pending_requests_.begin(); it != pending_requests_.end();) {
            if (it->second.expires_at < now) {
                try {
                    it->second.handler(Message{}, ErrorCode::TIMEOUT);
                } catch (...) {}
                it = pending_requests_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void Cleanup() {
        local_ring_.reset();
        ILOG << "Cleanup completed";
    }

    uint64_t NextRequestId() {
        static std::atomic<uint64_t> counter{1};
        return counter.fetch_add(1);
    }

    struct PendingRequest {
        ResponseHandler handler;
        std::chrono::steady_clock::time_point expires_at;
    };

    Options opts_;
    std::atomic<bool> running_;
    std::atomic<bool> ready_;
    std::mutex ready_mutex_;
    std::condition_variable ready_cv_;

    std::unique_ptr<ShmRingBuffer<Message>> local_ring_;
    std::thread io_thread_;
    std::thread retry_thread_;

    std::unordered_map<std::string, std::vector<MessageHandler>> handlers_;
    std::mutex handlers_mutex_;

    std::unordered_map<uint64_t, PendingRequest> pending_requests_;
    std::mutex pending_requests_mutex_;

    struct {
        std::atomic<uint64_t> local_messages{0};
        std::atomic<uint64_t> remote_messages{0};
        std::atomic<uint64_t> errors{0};
    } stats_;
};

// BusClient 包装实现
BusClient::BusClient(const Options& opts) : impl_(std::make_unique<Impl>(opts)) {}
BusClient::~BusClient() = default;
BusClient::BusClient(BusClient&&) noexcept = default;
BusClient& BusClient::operator=(BusClient&&) noexcept = default;

bool BusClient::Start() { return impl_->Start(); }
void BusClient::Stop() { impl_->Stop(); }
bool BusClient::WaitReady(std::chrono::milliseconds timeout) { return impl_->WaitReady(timeout); }

// BusClient公共接口实现
bool BusClient::Publish(const std::string& topic, const void* data, size_t len, Message::Flags flags) {
    return impl_->Publish(topic, data, len, flags);
}

bool BusClient::Subscribe(const std::string& topic, const MessageHandler& handler) {
    return impl_->Subscribe(topic, handler);
}

bool BusClient::Unsubscribe(const std::string& topic) {
    return impl_->Unsubscribe(topic);
}

uint64_t BusClient::Request(const std::string& topic, const void* data, size_t len,
                            const ResponseHandler& handler, std::chrono::milliseconds timeout) {
    return impl_->Request(topic, data, len, handler, timeout);
}

bool BusClient::Reply(uint64_t req_id, const void* data, size_t len) {
    return impl_->Reply(req_id, data, len);
}

Stats BusClient::GetStats() const { 
    return impl_->GetStats(); 
}

void BusClient::SetLogLevel(LogLevel level) {
    // 这里可以添加日志级别设置逻辑
    ILOG << "Log level set to: " << static_cast<int>(level);
}

} // namespace IBus