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
    msg_handler_[SSMsgID::SS_HEART_BEAT] = std::bind(&TcpRegistrar::onHeartbeat, this, std::placeholders::_1, std::placeholders::_2);
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
        // 需要得到 service name 才能注销；在 fd_to_inst_ 中保存了实例对象
        auto it2 = fd_to_inst_.find(client_fd);
        if (it2 != fd_to_inst_.end())
        {
            auto inst = it2->second;
            reg_.deregister_instance(inst->svc_name, inst->id);
            ILOG << "TcpRegistrar: connection closed, deregistered " << inst->to_string() << " ";
        }
        fd_to_id_.erase(it);
        fd_to_inst_.erase(client_fd);
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

    reg_.register_instance(inst, std::chrono::seconds(TTL));
    {
        std::lock_guard lg(conn_mu_);
        fd_to_id_[client_fd] = inst->id;
        fd_to_inst_[client_fd] = inst;
    }

    auto response_pack = Helper::CreateSSPack(*replyMsg);
    server_.send(client_fd, response_pack);
}

void TcpRegistrar::onHeartbeat(uint64_t client_fd, std::shared_ptr<AppMsg> msg)
{
    auto recvMsg = std::make_shared<ss::RegistToCenter>();
    recvMsg->ParsePartialFromArray(msg->data_, msg->data_len_);
    ILOG << recvMsg->Utf8DebugString();
    auto replyMsg = std::make_shared<ss::RegistToCenter>();
    auto request = recvMsg->mutable_request();
    auto response = replyMsg->mutable_response();
    response->set_err(Error_success);

    auto& svr_info = request->local_info_();
    
    // 找到 fd 对应保存的 instance 一并续约
    ServiceInstancePtr inst;
    {
        std::lock_guard lg(conn_mu_);
        auto it = fd_to_inst_.find(client_fd);
        if (it != fd_to_inst_.end())
            inst = it->second;
    }
    if (inst && inst->id == svr_info.id_())
    {
        reg_.register_instance(inst, std::chrono::seconds(TTL));
    }
    else
    {
        // 尝试按 id 在 registry 中查找实例并续约
        // 注：若当前连接上并未保存实例，可能客户端只是发送心跳来续约已有实例
        // 通过遍历 snapshot 可找到实例（代价较高，可优化）
        auto snap = reg_.snapshot();
        bool renewed = false;
        for (auto &kv : snap)
        {
            for (auto &p : kv.second)
            {
                if (p.second->id == svr_info.id_())
                {
                    reg_.register_instance(p.second, std::chrono::seconds(TTL));
                    renewed = true;
                    break;
                }
            }
            if (renewed)
                break;
        }
        if (!renewed)
            response->set_err(Error_instance_not_found);
    }

    auto response_pack = Helper::CreateSSPack(*response);
    server_.send(client_fd, response_pack);
}