#pragma once

#include <cstdint>
#include <string>
#include <memory>
#include <sstream>
#include <iomanip>
#include "../core/network/AppMsg.h"


namespace google
{
    namespace protobuf
    {
        class Message;
    }
}

class AppMsgWrapper;
class AppMsg;

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
    static uint64_t timeGetTimeS();
    static uint64_t timeGetTimeMS();
    static uint64_t timeGetTimeUS();
    static int64_t GenUID();
    static std::string GetShortTypeName(const google::protobuf::Message& msg);
    static std::string toHex(const void* data, size_t len);
    static std::shared_ptr<AppMsgWrapper> CreateSSPack(const google::protobuf::Message &message, Type type = Type::S2SReq, const uint64_t& co_id = 0);
    // DeleteSSPack 已废弃：slab 内存现在由 CreateSSPack 返回的 shared_ptr 的 custom deleter 自动管理
    // 调用此函数会造成 double-free，请勿使用
    [[deprecated("slab memory is now managed by the shared_ptr deleter from CreateSSPack; do not call")]]
    static void DeleteSSPack(const AppMsgWrapper& pack);
    // Base64url decode（不含 padding）
    static std::string base64Decode(const std::string& input);
    // HMAC-SHA256
    static std::string hmacSha256(const std::string& key, const std::string& data);
};
