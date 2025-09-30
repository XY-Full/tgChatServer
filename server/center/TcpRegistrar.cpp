#include "TcpRegistrar.h"
#include "../../third/nlohmann/json.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using json = nlohmann::json;

TcpRegistrar::TcpRegistrar(ServiceRegistry &reg, uint16_t port) : reg_(reg), port_(port)
{
}
TcpRegistrar::~TcpRegistrar()
{
    stop();
}

void TcpRegistrar::start()
{
    stop_ = false;
    thr_ = std::thread([this] { this->accept_loop(); });
}

void TcpRegistrar::stop()
{
    stop_ = true;
    if (thr_.joinable())
        thr_.join();
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void TcpRegistrar::accept_loop()
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0)
    {
        perror("socket");
        return;
    }
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port_);
    if (bind(listen_fd, (sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("bind");
        close(listen_fd);
        return;
    }
    if (listen(listen_fd, 128) < 0)
    {
        perror("listen");
        close(listen_fd);
        return;
    }

    std::cout << "TcpRegistrar listening on " << port_ << " ";

    while (!stop_)
    {
        sockaddr_in cli_addr{};
        socklen_t cli_len = sizeof(cli_addr);
        int client_fd = accept(listen_fd, (sockaddr *)&cli_addr, &cli_len);
        if (client_fd < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            if (errno == EINTR)
                continue;
            perror("accept");
            break;
        }
        // 每个连接使用独立线程（生产环境推荐线程池/协程）
        std::thread(&TcpRegistrar::handle_client, this, client_fd).detach();
    }

    close(listen_fd);
}

void TcpRegistrar::handle_client(int client_fd)
{
    constexpr size_t BUF_SIZE = 8192;
    std::string buffer;
    buffer.reserve(1024);
    char tmp[BUF_SIZE];
    ssize_t n;
    // 读取循环
    while ((n = read(client_fd, tmp, sizeof(tmp))) > 0)
    {
        buffer.append(tmp, tmp + n);
        size_t pos;
        while ((pos = buffer.find(' ')) != std::string::npos)
        {
            std::string line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            if (line.empty())
                continue;
            try
            {
                auto j = json::parse(line);
                // 判断消息类型，支持 register 和 heartbeat
                if (j.contains("type") && j.at("type").get<std::string>() == "heartbeat")
                {
                    // 心跳续约
                    if (!j.contains("id"))
                    {
                        std::string err = "ERR missing id in heartbeat ";
                        write(client_fd, err.data(), err.size());
                        continue;
                    }
                    std::string id = j.at("id").get<std::string>();
                    int ttl = 300;
                    if (j.contains("ttl"))
                        ttl = j.at("ttl").get<int>();
                    // 找到 fd 对应保存的 instance 一并续约
                    ServiceInstancePtr inst;
                    {
                        std::lock_guard lg(conn_mu_);
                        auto it = fd_to_inst_.find(client_fd);
                        if (it != fd_to_inst_.end())
                            inst = it->second;
                    }
                    if (inst && inst->id == id)
                    {
                        reg_.register_instance(inst, std::chrono::seconds(ttl));
                        std::string ok = "OK ";
                        write(client_fd, ok.data(), ok.size());
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
                                if (p.second->id == id)
                                {
                                    reg_.register_instance(p.second, std::chrono::seconds(ttl));
                                    renewed = true;
                                    break;
                                }
                            }
                            if (renewed)
                                break;
                        }
                        if (renewed)
                            write(client_fd, std::string("OK ").data(), 3);
                        else
                            write(client_fd, std::string("ERR no such id ").data(), 12);
                    }
                }
                else
                {
                    // 视为注册消息（兼容历史实现）
                    auto inst = std::make_shared<ServiceInstance>();
                    inst->svc_name = j.at("service").get<std::string>();
                    inst->address = j.at("address").get<std::string>();
                    inst->port = j.at("port").get<uint16_t>();
                    if (j.contains("id"))
                        inst->id = j.at("id").get<std::string>();
                    else
                        inst->id = inst->address + ":" + std::to_string(inst->port);
                    if (j.contains("weight"))
                        inst->weight = j.at("weight").get<uint32_t>();
                    int ttl = 300;
                    if (j.contains("ttl"))
                        ttl = j.at("ttl").get<int>();
                    reg_.register_instance(inst, std::chrono::seconds(ttl));
                    {
                        std::lock_guard lg(conn_mu_);
                        fd_to_id_[client_fd] = inst->id;
                        fd_to_inst_[client_fd] = inst;
                    }
                    std::string ack = "OK ";
                    write(client_fd, ack.data(), ack.size());
                }
            }
            catch (const std::exception &e)
            {
                std::string err = std::string("ERR ") + e.what() + " ";
                write(client_fd, err.data(), err.size());
            }
        }
    }
    // 连接关闭：尝试立即注销该 fd 对应的实例
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
                std::cout << "TcpRegistrar: connection closed, deregistered " << inst->to_string() << " ";
            }
            fd_to_id_.erase(it);
            fd_to_inst_.erase(client_fd);
        }
    }
    close(client_fd);
}