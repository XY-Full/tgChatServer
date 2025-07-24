#ifndef TELEGRAM_BOT_H
#define TELEGRAM_BOT_H

#include "ILogic.h"
#include "chat.pb.h"
#define CPPHTTPLIB_OPENSSL_SUPPORT

#include <string>

class TelegramBot : ILogic
{
public:
    TelegramBot(Busd *);

    void registerEvent();

private:
    MessagePtr NotifyAllPlayer(int64_t, const std::shared_ptr<cs::ChatMessage> &msg);
    void startWebhook(const std::string& webhook_url, const std::string& cert_file, const std::string& private_key_file);
};

#endif // TELEGRAM_BOT_H

