#pragma once

#include "ServiceRegistry.h"
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>

class AppMsg;

class HealthChecker
{
public:
    HealthChecker(ServiceRegistry &reg);
    ~HealthChecker();
    void start(std::chrono::seconds interval = std::chrono::seconds(5),
               std::chrono::seconds timeout = std::chrono::seconds(2));
    void stop();

private:
    void loop();
    bool probe_instance(const ServiceInstancePtr &inst);
    void handle_heartbeat(const AppMsg &msg);
    ServiceRegistry &reg_;
    std::thread thr_;
    std::atomic<bool> stop_{false};
    std::chrono::seconds interval_{5};
    std::chrono::seconds timeout_{2};
    std::mutex cv_mutex_;
    std::condition_variable cv_;
};