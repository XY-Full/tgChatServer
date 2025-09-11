#include "HealthChecker.h"
#include "httplib.h"
#include <iostream>

HealthChecker::HealthChecker(ServiceRegistry &reg) : reg_(reg)
{
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
                for (auto &inst : kv.second)
                {
                    bool ok = probe_instance(inst);
                    inst->healthy.store(ok);
                }
            }
            reg_.cleanup_expired();
        }
        catch (const std::exception &e)
        {
            std::cerr << "HealthChecker 异常: " << e.what() << " ";
        }
        std::this_thread::sleep_for(interval_);
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