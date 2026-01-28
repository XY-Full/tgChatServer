#include "HealthChecker.h"
#include "GlobalSpace.h"
#include "bus/IBus.h"
#include "core.pb.h"
#include "httplib.h"
#include "network/AppMsg.h"
#include "ss_msg_id.pb.h"
#include <iostream>
#include <memory>

HealthChecker::HealthChecker(ServiceRegistry &reg) : reg_(reg)
{
    GlobalSpace()->bus_->RegistMessage(SS_HEART_BEAT, std::bind(&HealthChecker::handle_heartbeat, this, std::placeholders::_1));
}
HealthChecker::~HealthChecker()
{
    stop();
}
void HealthChecker::start(std::chrono::seconds interval, std::chrono::seconds timeout)
{
    interval_ = interval;
    timeout_ = timeout;
    stop_ = false;
    thr_ = std::thread([this] { this->loop(); });
}
void HealthChecker::stop()
{
    stop_ = true;
    cv_.notify_all();  // Wake up the thread immediately
    if (thr_.joinable())
        thr_.join();
}
void HealthChecker::loop()
{
    while (!stop_)
    {
        try
        {
            auto snap = reg_.snapshot();
            for (auto &kv : snap)
            {
                for (auto &inst_pair : kv.second)
                {
                    probe_instance(inst_pair.second);
                }
            }
            reg_.cleanup_expired();
        }
        catch (const std::exception &e)
        {
            std::cerr << "HealthChecker 异常: " << e.what() << " ";
        }
        
        // Use condition variable with timeout instead of sleep_for
        // This allows immediate wakeup when stop() is called
        std::unique_lock<std::mutex> lock(cv_mutex_);
        cv_.wait_for(lock, interval_, [this]() { return stop_.load(); });
    }
}

bool HealthChecker::probe_instance(const ServiceInstancePtr &inst)
{
    httplib::Client cli(inst->address.c_str(), inst->port);
    cli.set_connection_timeout(timeout_);
    cli.set_read_timeout(timeout_);
    if (auto res = cli.Get("/health"))
    {
        if (res->status >= 200 && res->status < 300)
            return true;
    }
    return false;
}

void HealthChecker::handle_heartbeat(const AppMsg &msg)
{
    auto response_pb = std::make_shared<ss::HeartBeat>();
    response_pb->ParseFromArray(msg.data_, msg.data_len_);
    auto response = response_pb->mutable_response();
    auto request = std::make_shared<ss::HeartBeat>()->mutable_request();
    auto& service_id = response->service_info_().id_();
    try
    {
        auto snap = reg_.snapshot();
        for (auto &kv : snap)
        {
            auto &all_inst = kv.second;
            if(all_inst.find(service_id) == all_inst.end())
            {
                ILOG << "node: " << service_id << " has offline";
                break;
            }
            auto &inst = all_inst[service_id]->healthy = true;
        }
        reg_.cleanup_expired();
    }
    catch (const std::exception &e)
    {
        ELOG << "HealthChecker 异常: " << e.what();
    }
}