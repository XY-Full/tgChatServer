#include "HttpServer.h"
#include "json.hpp"
#include "httplib.h"
#include <iostream>
#include <chrono>

using json = nlohmann::json;

HttpServer::HttpServer(ServiceRegistry& reg, uint16_t port): registry_(reg), port_(port) {
    lb_factory_ = std::make_unique<LBFactory>();
    health_checker_ = std::make_unique<HealthChecker>(registry_);
}
HttpServer::~HttpServer(){ stop(); }

void HttpServer::start() {
    health_checker_->start(std::chrono::seconds(5), std::chrono::seconds(2));
    httplib::Server svr;

    svr.Post("/register", [&](const httplib::Request& req, httplib::Response& res){
        std::string out; int status=200;
        handle_register(req.body, out, status);
        res.status = status; res.set_content(out, "application/json");
    });

    svr.Post("/deregister", [&](const httplib::Request& req, httplib::Response& res){
        try {
            auto j = json::parse(req.body);
            auto svc = j.at("service").get<std::string>();
            auto id = j.at("id").get<std::string>();
            registry_.deregister_instance(svc, id);
            res.status = 200; res.set_content("{\"result\":\"ok\"}", "application/json");
        } catch (...) { res.status = 400; res.set_content("{\"error\":\"invalid\"}", "application/json"); }
    });

    svr.Get(R"(/list)", [&](const httplib::Request& req, httplib::Response& res){
        auto svc = req.get_param_value("service");
        if (svc.empty()) { res.status=400; res.set_content("{\"error\":\"service required\"}", "application/json"); return; }
        auto include_unhealthy = req.has_param("include_unhealthy") && req.get_param_value("include_unhealthy")=="1";
        auto instances = registry_.get_instances(svc, !include_unhealthy);
        json arr = json::array();
        for (auto &p : instances) {
            json j; j["id"]=p->id; j["address"]=p->address; j["port"]=p->port; j["weight"]=p->weight; j["healthy"]=p->healthy.load();
            j["connections"]=p->connections.load(); j["metadata"]=p->metadata; j["avg_latency_us"]=p->avg_latency_us.load(); arr.push_back(j);
        }
        res.status = 200; res.set_content(arr.dump(), "application/json");
    });

    svr.Get(R"(/discover)", [&](const httplib::Request& req, httplib::Response& res){
        auto svc = req.get_param_value("service");
        if (svc.empty()) { res.status = 400; res.set_content("{\"error\":\"service required\"}", "application/json"); return; }
        auto strategy = req.has_param("strategy") ? req.get_param_value("strategy") : std::string("round_robin");
        auto onlyHealthy = !(req.has_param("include_unhealthy") && req.get_param_value("include_unhealthy")=="1");
        auto key = req.has_param("key") ? req.get_param_value("key") : std::string("");
        auto instances = registry_.get_instances(svc, onlyHealthy);
        if (instances.empty()) { res.status = 404; res.set_content("{\"error\":\"no instances\"}", "application/json"); return; }
        auto lb = lb_factory_->create(strategy);
        auto chosen = lb->select(instances, key);
        if (!chosen) { res.status = 500; res.set_content("{\"error\":\"no candidate\"}", "application/json"); return; }
        json out; out["id"]=chosen->id; out["address"]=chosen->address; out["port"]=chosen->port; out["weight"]=chosen->weight; out["metadata"]=chosen->metadata; out["avg_latency_us"]=chosen->avg_latency_us.load();
        res.status=200; res.set_content(out.dump(), "application/json");
    });

    // /routing_table
    svr.Get(R"(/routing_table)", [&](const httplib::Request& req, httplib::Response& res){
        res.status = 200; res.set_content(registry_.routing_table_string(), "text/plain");
    });

    // /proxy/<service>/<path...> 支持 server-side LB 代理，并在代理完成后记录响应延迟
    svr.Get(R"(^/proxy/([A-Za-z0-9_.-]+)/(.+)$)", [&](const httplib::Request& req, httplib::Response& res){
        if (req.matches.size() < 3) { res.status = 400; res.set_content("invalid proxy path", "text/plain"); return; }
        auto svc = req.matches[1];
        auto path = std::string("/") + req.matches[2];
        auto strategy = req.has_param("strategy") ? req.get_param_value("strategy") : "round_robin";
        auto key = req.has_param("key") ? req.get_param_value("key") : std::string("");

        auto instances = registry_.get_instances(svc);
        if (instances.empty()) { res.status = 404; res.set_content("no instances", "text/plain"); return; }
        auto lb = lb_factory_->create(strategy);
        auto chosen = lb->select(instances, key);
        if (!chosen) { res.status = 500; res.set_content("no candidate", "text/plain"); return; }

        chosen->connections.fetch_add(1);
        httplib::Client proxy_cli(chosen->address.c_str(), chosen->port);
        proxy_cli.set_connection_timeout(std::chrono::seconds(2));
        proxy_cli.set_read_timeout(std::chrono::seconds(10));

        auto start = std::chrono::steady_clock::now();
        if (auto r = proxy_cli.Get(path.c_str())) {
            auto end = std::chrono::steady_clock::now();
            uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            // 更新实例延迟测量
            chosen->update_latency_us(us);
            res.status = r->status;
            res.set_content(r->body, r->get_header_value("Content-Type"));
        } else {
            res.status = 502; res.set_content("bad gateway", "text/plain");
        }
        chosen->connections.fetch_sub(1);
    });

    svr.Post(R"(^/proxy/([A-Za-z0-9_.-]+)/(.+)$)", [&](const httplib::Request& req, httplib::Response& res){
        if (req.matches.size() < 3) { res.status = 400; res.set_content("invalid proxy path", "text/plain"); return; }
        auto svc = req.matches[1];
        auto path = std::string("/") + req.matches[2];
        auto strategy = req.has_param("strategy") ? req.get_param_value("strategy") : "round_robin";
        auto key = req.has_param("key") ? req.get_param_value("key") : std::string("");

        auto instances = registry_.get_instances(svc);
        if (instances.empty()) { res.status = 404; res.set_content("no instances", "text/plain"); return; }
        auto lb = lb_factory_->create(strategy);
        auto chosen = lb->select(instances, key);
        if (!chosen) { res.status = 500; res.set_content("no candidate", "text/plain"); return; }

        chosen->connections.fetch_add(1);
        httplib::Client proxy_cli(chosen->address.c_str(), chosen->port);
        proxy_cli.set_connection_timeout(std::chrono::seconds(2));
        proxy_cli.set_read_timeout(std::chrono::seconds(10));

        auto start = std::chrono::steady_clock::now();
        if (auto r = proxy_cli.Post(path.c_str(), req.body, req.get_header_value("Content-Type"))) {
            auto end = std::chrono::steady_clock::now();
            uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
            chosen->update_latency_us(us);
            res.status = r->status;
            res.set_content(r->body, r->get_header_value("Content-Type"));
        } else {
            res.status = 502; res.set_content("bad gateway", "text/plain");
        }
        chosen->connections.fetch_sub(1);
    });

    std::cout << "HttpServer listening on " << port_ << " ";
    svr.listen("0.0.0.0", port_);
}

void HttpServer::stop() {
    if (health_checker_) health_checker_->stop();
}

void HttpServer::handle_register(const std::string& body, std::string& out, int& status) {
    try {
        auto j = json::parse(body);
        auto inst = std::make_shared<ServiceInstance>();
        inst->svc_name = j.at("service").get<std::string>();
        inst->address = j.at("address").get<std::string>();
        inst->port = j.at("port").get<uint16_t>();
        if (j.contains("id")) inst->id = j.at("id").get<std::string>();
        else inst->id = inst->address + ":" + std::to_string(inst->port);
        if (j.contains("weight")) inst->weight = j.at("weight").get<uint32_t>();
        std::chrono::seconds ttl = std::chrono::seconds(30);
        if (j.contains("ttl")) ttl = std::chrono::seconds(j.at("ttl").get<int>());
        registry_.register_instance(inst, ttl);
        json r; r["result"] = "ok"; r["id"]=inst->id;
        out = r.dump(); status = 200;
        std::cout << "HTTP Registered: " << inst->to_string() << " ttl=" << ttl.count() << "s ";
    } catch (const std::exception &e) {
        status = 400; json r; r["error"] = e.what(); out = r.dump();
    }
}