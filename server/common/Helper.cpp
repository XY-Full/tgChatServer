#include "Helper.h"
#include "GlobalSpace.h"
#include "JsonConfigNode.h"
#include "Log.h"
#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <iomanip>
#include <stdexcept>
#include <string>
#include "google/protobuf/message.h"
#include "../core/network/AppMsg.h"
#include "../../public/proto_files/msg_mapping_ss.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <cstring>
#include <iconv.h>
#endif

#if defined(_WIN32)

// Helper for Windows: Convert encoding via wide char
std::string convert_encoding_win(const std::string &input, UINT fromCP, UINT toCP)
{
    if (input.empty())
        return "";

    int wlen = MultiByteToWideChar(fromCP, 0, input.c_str(), -1, nullptr, 0);
    if (wlen <= 0)
        throw std::runtime_error("MultiByteToWideChar failed");

    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(fromCP, 0, input.c_str(), -1, &wstr[0], wlen);

    int olen = WideCharToMultiByte(toCP, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (olen <= 0)
        throw std::runtime_error("WideCharToMultiByte failed");

    std::string result(olen, 0);
    WideCharToMultiByte(toCP, 0, wstr.c_str(), -1, &result[0], olen, nullptr, nullptr);

    result.pop_back(); // Remove null terminator
    return result;
}

std::string Helper::utf8_to_local(const std::string &utf8Str)
{
    return convert_encoding_win(utf8Str, CP_UTF8, CP_ACP);
}

std::string Helper::local_to_utf8(const std::string &localStr)
{
    return convert_encoding_win(localStr, CP_ACP, CP_UTF8);
}

std::string Helper::gbk_to_local(const std::string &gbkStr)
{
    return convert_encoding_win(gbkStr, 936, CP_ACP); // 936 = GBK
}

std::string Helper::local_to_gbk(const std::string &localStr)
{
    return convert_encoding_win(localStr, CP_ACP, 936);
}

#else // POSIX (macOS/Linux)

std::string convert_iconv(const std::string &input, const char *from, const char *to)
{
    iconv_t cd = iconv_open(to, from);
    if (cd == (iconv_t)(-1))
    {
        throw std::runtime_error("iconv_open failed");
    }

    size_t inSize = input.size();
    size_t outSize = inSize * 4;
    std::string output(outSize, 0);

    char *inBuf = const_cast<char *>(input.data());
    char *outBuf = &output[0];
    size_t inBytes = inSize;
    size_t outBytes = outSize;

    size_t res = iconv(cd, &inBuf, &inBytes, &outBuf, &outBytes);
    iconv_close(cd);

    if (res == (size_t)-1)
    {
        throw std::runtime_error("iconv conversion failed");
    }

    output.resize(outSize - outBytes);
    return output;
}

std::string utf8_to_local(const std::string &utf8Str)
{
#if defined(__APPLE__)
    return convert_iconv(utf8Str, "UTF-8", "MACROMAN"); // macOS locale encoding fallback
#else
    return convert_iconv(utf8Str, "UTF-8", "GBK"); // Assume GBK as default locale
#endif
}

std::string local_to_utf8(const std::string &localStr)
{
#if defined(__APPLE__)
    return convert_iconv(localStr, "MACROMAN", "UTF-8");
#else
    return convert_iconv(localStr, "GBK", "UTF-8");
#endif
}

std::string gbk_to_local(const std::string &gbkStr)
{
    // On most Linux systems local encoding is GBK already; pass through
    return gbkStr;
}

std::string local_to_gbk(const std::string &localStr)
{
    // Same as above
    return localStr;
}

#endif // _WIN32

bool Helper::IsTextUTF8(const std::string &str)
{
    char nBytes = 0; // UFT8可用1-6个字节编码,ASCII用一个字节
    unsigned char chr;
    bool bAllAscii = true; // 如果全部都是ASCII, 说明不是UTF-8

    for (int i = 0; i < str.length(); i++)
    {
        chr = str[i];

        // 判断是否ASCII编码,如果不是,说明有可能是UTF-8,ASCII用7位编码,
        // 但用一个字节存,最高位标记为0,o0xxxxxxx
        if ((chr & 0x80) != 0)
            bAllAscii = false;

        if (nBytes == 0) // 如果不是ASCII码,应该是多字节符,计算字节数
        {
            if (chr >= 0x80)
            {
                if (chr >= 0xFC && chr <= 0xFD)
                    nBytes = 6;
                else if (chr >= 0xF8)
                    nBytes = 5;
                else if (chr >= 0xF0)
                    nBytes = 4;
                else if (chr >= 0xE0)
                    nBytes = 3;
                else if (chr >= 0xC0)
                    nBytes = 2;
                else
                {
                    return false;
                }
                nBytes--;
            }
        }
        else // 多字节符的非首字节,应为 10xxxxxx
        {
            if ((chr & 0xC0) != 0x80)
            {
                return false;
            }
            nBytes--;
        }
    }

    if (nBytes > 0) // 违返规则
        return false;

    if (bAllAscii) // 如果全部都是ASCII, 说明不是UTF-8
        return false;

    return true;
}

void Helper::printStringData(std::string data)
{
    std::cout << "String content as bytes: ";
    for (size_t i = 0; i < data.size(); ++i)
    {
        std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)(unsigned char)data[i] << " ";
    }
    std::cout << std::dec << std::endl; // 恢复十进制输出
}

void Helper::sendTgMessage(const std::string &chat_id, const std::string &text)
{
    CURL *curl = curl_easy_init();
    if (curl)
    {
        std::string escaped_text = curl_easy_escape(curl, text.c_str(), text.length());
        std::string base_url = "https://api.telegram.org/bot";
        std::string token = (*(GlobalSpace()->configMgr_))["telegram"]["bot_token"].value<std::string>();
        std::string url = base_url + token + "/sendMessage?chat_id=" + chat_id + "&text=" + escaped_text;

        // std::cout << "URL: " << url << std::endl;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L); // 允许重定向
        // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L); // 启用调试信息
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L); // 设置超时

        // std::string response_string;
        // curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, NULL);
        // curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_string);

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK)
        {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }
        else
        {
            // std::cout << "Response: " << response_string << std::endl; // 打印响应
        }

        curl_easy_cleanup(curl); // 清理curl句柄
    }
    else
    {
        std::cerr << "Failed to initialize CURL" << std::endl;
        curl_easy_cleanup(curl); // 清理curl句柄
    }
}

int64_t Helper::timeGetTimeS()
{
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<double>(steady_clock::now() - start).count();
}

int64_t Helper::timeGetTimeMS()
{
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration_cast<milliseconds>(steady_clock::now() - start).count();
}

int64_t Helper::GenUID()
{
    using namespace std::chrono;
    static std::atomic<int64_t> last_ms{0};
    static std::atomic<int64_t> counter{0};

    int64_t now_ms = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();

    int64_t base;
    do
    {
        base = last_ms.load();
        if (now_ms > base)
        {
            counter.store(0); // 新时间戳，重置 counter
        }
    }
    while (!last_ms.compare_exchange_weak(base, now_ms));

    // 3位 counter，可支撑同一毫秒内生成 1000 个 UID
    int64_t uid = now_ms * 1000 + counter.fetch_add(1);
    return uid;
}

uint32_t Helper::CreateSSPack(const google::protobuf::Message &message, const uint32_t& seq)
{
    static std::atomic<uint32_t> now_seq_{0};

    // 获取message序列化后的长度，直接申请一块AppMsg+message_strlen大小的内存
    auto message_strlen = message.ByteSizeLong();
    int32_t msg_id = kMsgNameToId.at(message.GetTypeName());

    auto shm_slab_ = GlobalSpace()->shm_slab_;

    uint32_t data_len = message_strlen;
    uint32_t pack_len = sizeof(AppMsg) + data_len;

    // 从slab中分配内存块
    auto pack_shm_offset = shm_slab_.Alloc(pack_len); // 申请AppMsg结构体+body数据大小+类型名长度的内存块
    auto pack_shm_addr = shm_slab_.off2ptr(pack_shm_offset);
    
    auto msg = reinterpret_cast<AppMsg *>(pack_shm_addr);
    // 填充AppMsg Header
    msg->header_.version_ = MAGIC_VERSION;
    msg->header_.type_ = Type::S2S;
    msg->header_.pack_len_ = pack_len;
    if(seq == 0) msg->header_.seq_ = now_seq_.fetch_add(1);
    else msg->header_.seq_ = seq;

    msg->data_ = reinterpret_cast<char *>(pack_shm_addr) + sizeof(AppMsg);  // body紧跟在AppMsg结构体后面
    msg->data_len_ = data_len;                                              // body数据长度
    msg->msg_id_ = msg_id;                                                  // 消息ID

    // pb数据序列化到data_中，需要预留前缀长度，填充类型名字
    if (!message.SerializeToArray(msg->data_, message_strlen))
    {
        ELOG << "Error to serialize message";
        return pack_shm_offset;
    }

    return pack_shm_offset;
}

void Helper::DeleteSSPack(uint32_t pack_shm_offset)
{
    auto pack_size = reinterpret_cast<AppMsg*>(GlobalSpace()->shm_slab_.off2ptr(pack_shm_offset))->header_.pack_len_;
    GlobalSpace()->shm_slab_.Free(pack_shm_offset, pack_size);
}