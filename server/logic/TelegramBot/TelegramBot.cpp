#include "TelegramBot.h"
#include <chrono>
#include <curl/curl.h>
#include <httplib.h>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>
#include <unordered_map>
#include "Log.h"
#include "notify.pb.h"

TelegramBot::TelegramBot(Busd *busd) : ILogic(busd)
{
    registerEvent();
    startWebhook("/webhook", "/etc/letsencrypt/live/vps.ov1.top/fullchain.pem",
                               "/etc/letsencrypt/live/vps.ov1.top/privkey.pem");
    ILOG << "bot init done";
}

void TelegramBot::registerEvent()
{
    busd_->registerEventHandle(&TelegramBot::NotifyAllPlayer, this);
}

void TelegramBot::startWebhook(const std::string &webhook_url, const std::string &cert_file,
                               const std::string &private_key_file)
{

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
    httplib::SSLServer svr(cert_file.c_str(), private_key_file.c_str());
#else
    httplib::Server svr;
#endif

    if (!svr.is_valid())
    {
        printf("server has an error...\n");
        return;
    }

    svr.Post(webhook_url.c_str(), [this](const httplib::Request &req, httplib::Response &res) {
        auto json_response = nlohmann::json::parse(req.body);
        std::cout << "recved: " << json_response << ", size: " << json_response.size() << std::endl;
        auto ev = std::make_shared<cs::ChatMessage>();
        if (!json_response.contains("message"))
        {
            res.status = 200;
            res.set_content("NOT GOOD", "text/plain");
            return;
        }

        if (json_response["message"].contains("text"))
        {
            auto message = json_response["message"];
            auto chatId = message["chat"]["id"].get<int64_t>();
            std::string chat_id = std::to_string(chatId); // 转为字符串
            std::string text = message["text"];

            ev->set_msg(text);
            ev->set_chat_id(chatId);
            ev->set_timestamp(std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
            ev->set_msg_id(message["message_id"].get<int64_t>());
            ev->mutable_player_info()->set_name(message["chat"]["first_name"].get<std::string>());

            // std::cout << "recved: " << text << ", size: " << text.size() << std::endl;
        }
        else
        {
            res.status = 200;
            res.set_content("NOT GOOD", "text/plain");
            return;
        }

        res.status = 200;
        res.set_content("OK", "text/plain");
        NotifyAllPlayer(0, ev);
    });

    // 处理 GET 请求（用于验证 webhook 是否正常）
    svr.Get("/", [](const httplib::Request &req, httplib::Response &res) {
        res.status = 200;
        std::cout << "get method!!" << std::endl;
        res.set_redirect("/hi");
    });

    std::cout << "bind port 8443..." << std::endl;
    svr.listen("0.0.0.0", 8443); // 在 8443 端口监听
}

MessagePtr TelegramBot::NotifyAllPlayer(int64_t playerid, const std::shared_ptr<cs::ChatMessage> &ev)
{
    cs::Notify notify;
    notify.mutable_chat_message()->CopyFrom(*ev);
    busd_->broadcastTotalPlayer(MSGID::SC_NOTIFY, notify);

    return nullptr;
}
