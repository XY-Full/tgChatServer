#include "TcpRegistrar.h"
#include "../../third/nlohmann/json.hpp"
#include "Helper.h"
#include "Log.h"
#include "CommonDef.h"
#include "core.pb.h"
#include "ss_msg_id.pb.h"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#include "CenterService.h"

using json = nlohmann::json;

TcpRegistrar::TcpRegistrar(ServiceRegistry &reg, uint16_t port)
    : reg_(reg), port_(port),
      server_(port, std::bind(&TcpRegistrar::handleClient, this, std::placeholders::_1, std::placeholders::_2),
              CenterApp::getInstance().getName() + "_registrar", std::bind(&TcpRegistrar::handleDisconnect, this, std::placeholders::_1))
{
    msg_handler_[SSMsgID::SS_REGIST_TO_CENTER] = std::bind(&TcpRegistrar::onRegist, this, std::placeholders::_1, std::placeholders::_2);
    // msg_handler_[SSMsgID::SS_HEART_BEAT] = std::bind(&TcpRegistrar::onHeartbeat, this, std::placeholders::_1, std::placeholders::_2);
    msg_handler_[SSMsgID::SS_UPDATE_SERVICE_STATUS] = std::bind(&TcpRegistrar::onUpdateServiceStatus, this, std::placeholders::_1, std::placeholders::_2);
}
TcpRegistrar::~TcpRegistrar()
{
    stop();
}

void TcpRegistrar::start()
{
    server_.start();
}

void TcpRegistrar::stop()
{
    server_.stop();
}

void TcpRegistrar::handleClient(uint64_t client_fd, std::shared_ptr<AppMsg> msg)
{
    auto msg_id = msg->msg_id_;
    ILOG << "TcpRegistrar::handleClient, msg_id: " << msg_id << ", from: " << msg->src_name_;
    if(msg_handler_.find(msg_id) != msg_handler_.end())
        msg_handler_[msg_id](client_fd, msg);
}

void TcpRegistrar::handleDisconnect(uint64_t client_fd)
{
    std::lock_guard lg(conn_mu_);
    auto it = fd_to_id_.find(client_fd);
    if (it != fd_to_id_.end())
    {
        auto it2 = fd_to_inst_.find(client_fd);
        if (it2 != fd_to_inst_.end())
        {
            auto inst = it2->second;
            // 从 registry 注销（会触发差量推送给其他所有订阅者）
            reg_.deregister_instance(inst->svc_name, inst->id);
            // 释放该实例的 delta 订阅槽
            reg_.unsubscribe(inst->id);
            ILOG << "TcpRegistrar: connection closed, deregistered " << inst->to_string();
        }
        fd_to_id_.erase(it);
        fd_to_inst_.erase(client_fd);
        fd_to_got_full_update_.erase(client_fd);
    }
}

void TcpRegistrar::onRegist(uint64_t client_fd, std::shared_ptr<AppMsg> msg)
{
    ILOG << "TcpRegistrar::onRegist, from: " << msg->src_name_;

    auto recvMsg = std::make_shared<ss::RegistToCenter>();
    recvMsg->ParsePartialFromArray(msg->data_, msg->data_len_);
    ILOG << recvMsg->Utf8DebugString();
    auto replyMsg = std::make_shared<ss::RegistToCenter>();
    auto request = recvMsg->mutable_request();
    auto response = replyMsg->mutable_response();
    response->set_err(Error_success);

    auto inst = std::make_shared<ServiceInstance>();
    auto& svr_info = request->local_info_();
    inst->svc_name = svr_info.svr_name_();
    inst->address = svr_info.ip_();
    inst->port = svr_info.port_();
    inst->id = svr_info.id_();

    inst->update_latency_us(Helper::timeGetTimeUS() - request->send_time_());

    // 先订阅 delta 槽，再注册实例（保证不会错过自己注册触发的其他人的 delta）
    reg_.subscribe(inst->id);
    reg_.register_instance(inst, std::chrono::seconds(TTL), /*is_new_instance=*/true);

    {
        std::lock_guard lg(conn_mu_);
        fd_to_id_[client_fd] = inst->id;
        fd_to_inst_[client_fd] = inst;
    }

    auto response_pack = Helper::CreateSSPack(*replyMsg);
    server_.send(client_fd, response_pack);
    // 注意：全量服务列表通过第一次 onUpdateServiceStatus 下发，onRegist 只确认注册成功
}

void TcpRegistrar::onUpdateServiceStatus(uint64_t client_fd, std::shared_ptr<AppMsg> msg)
{
    auto recvMsg = std::make_shared<ss::UpdateServiceStatus>();
    recvMsg->ParsePartialFromArray(msg->data_, msg->data_len_);

    auto replyMsg = std::make_shared<ss::UpdateServiceStatus>();
    auto request = recvMsg->mutable_request();
    auto response = replyMsg->mutable_response();
    response->set_err(Error_success);

    auto& svr_info = request->local_info_();

    // ---- 快速路径：缓存续约 ----
    ServiceInstancePtr inst;
    {
        std::lock_guard lg(conn_mu_);
        auto it = fd_to_inst_.find(client_fd);
        if (it != fd_to_inst_.end())
            inst = it->second;
    }

    bool renewed = false;

    if (inst && inst->id == svr_info.id_())
    {
        ILOG << "onUpdateServiceStatus: renewing instance from cache for fd=" << client_fd
             << " id=" << svr_info.id_();
        // 续约：is_new_instance=false，不触发差量推送
        reg_.register_instance(inst, std::chrono::seconds(TTL), /*is_new_instance=*/false);
        renewed = true;
    }
    else
    {
        // 缓存未命中（重连等异常情况），全表查找
        auto snap = reg_.snapshot();
        for (auto &kv : snap)
        {
            for (auto &p : kv.second)
            {
                if (p.second->id == svr_info.id_())
                {
                    reg_.register_instance(p.second, std::chrono::seconds(TTL), /*is_new_instance=*/false);
                    {
                        std::lock_guard lg(conn_mu_);
                        fd_to_inst_[client_fd] = p.second;
                    }
                    inst = p.second;
                    renewed = true;
                    break;
                }
            }
            if (renewed) break;
        }
    }

    if (!renewed)
    {
        // 实例不在 registry 中（center 重启、TTL 过期后重连等场景）
        // 将其视为重新注册：补充 subscribe + register，然后走全量下发路径
        ILOG << "onUpdateServiceStatus: instance not found, re-registering fd=" << client_fd
             << " id=" << svr_info.id_();
        auto new_inst = std::make_shared<ServiceInstance>();
        new_inst->svc_name = svr_info.svr_name_();
        new_inst->address  = svr_info.ip_();
        new_inst->port     = svr_info.port_();
        new_inst->id       = svr_info.id_();
        new_inst->shm_recv_buffer_name = svr_info.shm_recv_buffer_name_();
        new_inst->last_seen = std::chrono::steady_clock::now();

        // subscribe 幂等（已存在则不覆盖）
        reg_.subscribe(new_inst->id);
        reg_.register_instance(new_inst, std::chrono::seconds(TTL), /*is_new_instance=*/true);

        {
            std::lock_guard lg(conn_mu_);
            fd_to_id_[client_fd]   = new_inst->id;
            fd_to_inst_[client_fd] = new_inst;
            // 清除旧的全量标记，确保下面走全量下发
            fd_to_got_full_update_.erase(client_fd);
        }
        inst = new_inst;
        // renewed 保持 false，跳过差量逻辑，直接走全量下发
    }

    if (!inst)
    {
        // 理论上不会到这里，保险起见
        response->set_err(Error_instance_not_found);
        auto response_pack = Helper::CreateSSPack(*replyMsg);
        server_.send(client_fd, response_pack);
        return;
    }

    // ---- 取差量 ----
    auto deltas = reg_.pop_deltas(inst->id);

    if (deltas.empty())
    {
        // 无变化：首次请求时 deltas 也可能为空（刚注册，snapshot 为最新）
        // 此时若没有收到过全量，需要做一次全量（通过 is_full_update_=true 标记）
        bool is_first_request;
        {
            std::lock_guard lg(conn_mu_);
            is_first_request = !fd_to_got_full_update_.count(client_fd);
        }

        if (is_first_request)
        {
            response->set_is_full_update_(true);
            auto snap = reg_.snapshot();
            for (auto &kv : snap)
            {
                for (auto &p : kv.second)
                {
                    auto new_info = response->mutable_service_info_map_()->Add();
                    new_info->set_svr_name_(p.second->svc_name);
                    new_info->set_ip_(p.second->address);
                    new_info->set_port_(p.second->port);
                    new_info->set_id_(p.second->id);
                    new_info->set_is_offline_(false);
                }
            }
            {
                std::lock_guard lg(conn_mu_);
                fd_to_got_full_update_.insert(client_fd);
            }
            ILOG << "onUpdateServiceStatus: full snapshot sent to fd=" << client_fd;
        }
        // 否则无变化，返回空列表（客户端无需处理）
    }
    else
    {
        // 差量更新：按 delta 列表下发
        response->set_is_full_update_(false);
        for (auto &entry : deltas)
        {
            auto new_info = response->mutable_service_info_map_()->Add();
            new_info->set_svr_name_(entry.inst->svc_name);
            new_info->set_ip_(entry.inst->address);
            new_info->set_port_(entry.inst->port);
            new_info->set_id_(entry.inst->id);
            new_info->set_is_offline_(entry.op == DeltaEntry::Op::OFFLINE);
        }
        ILOG << "onUpdateServiceStatus: " << deltas.size()
             << " delta(s) sent to fd=" << client_fd;
    }

    auto response_pack = Helper::CreateSSPack(*replyMsg);
    server_.send(client_fd, response_pack);

    // 每次心跳顺带清理过期实例，避免依赖独立的 HealthChecker 线程
    reg_.cleanup_expired();
}
