#pragma once

#include "string"

#define BOT_TOKEN "7860009277:AAEvFHoZqJIeYVReYOoS62m5GjYWVIDfXNo"

class Helper
{
public:
    static void printStringData(std::string data);
    static std::string utf8_to_local(const std::string &utf8Str);
    static std::string local_to_utf8(const std::string &localStr);
    static std::string gbk_to_local(const std::string &gbkStr);
    static std::string local_to_gbk(const std::string &localStr);
    static bool IsTextUTF8(const std::string &str);
    static void sendTgMessage(const std::string &chat_id, const std::string &text);
    static int64_t timeGetTimeS();
    static int64_t timeGetTimeMS();
    static int64_t GenUID();
};