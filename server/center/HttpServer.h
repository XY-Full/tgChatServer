#pragma once

#include "HealthChecker.h"
#include "LoadBalancer.h"
#include "ServiceRegistry.h"
#include <memory>

class HttpServer
{
public:
    HttpServer(ServiceRegistry &reg, uint16_t port = 8080);
    ~HttpServer();
    void start();
    void stop();

private:
    void handle_register(const std::string &body, std::string &out, int &status);
    ServiceRegistry &registry_;
    std::unique_ptr<LBFactory> lb_factory_;
    std::unique_ptr<HealthChecker> health_checker_;
    uint16_t port_;
};