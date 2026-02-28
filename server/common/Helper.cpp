#include "Helper.h"
#include "GlobalSpace.h"
#include "Log.h"
#include <atomic>
#include <chrono>
#include <curl/curl.h>
#include <iomanip>
#include <stdexcept>
#include <string>
#include "google/protobuf/message.h"
#include "network/AppMsg.h"
#include "network/MsgWrapper.h"
#include "msg_mapping_ss.h"
#include "app/ConfigManager.h"

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
        std::string token = GlobalSpace()->configMgr_->getValue<std::string>("telegram.bot_token", "");
        
        if (token.empty()) {
            std::cerr << "[ERROR] telegram.bot_token not configured" << std::endl;
            curl_easy_cleanup(curl);
            return;
        }
        
        std::string url = base_url + token + "/sendMessage?chat_id=" + chat_id + "&text=" + escaped_text;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

        CURLcode res = curl_easy_perform(curl);

        if (res != CURLE_OK)
        {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }

        curl_easy_cleanup(curl);
    }
    else
    {
        std::cerr << "Failed to initialize CURL" << std::endl;
    }
}

uint64_t Helper::timeGetTimeS()
{
    return timeGetTimeUS() / 1000000ULL;  // Fixed: was calling timeGetTimeMS() recursively
}

uint64_t Helper::timeGetTimeMS()
{
    return timeGetTimeUS() / 1000ULL;  // Fixed: was calling itself recursively
}

uint64_t Helper::timeGetTimeUS()
{
#if defined(__linux__) || defined(__APPLE__)
    // Linux / macOS 使用高精度时钟
    struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return static_cast<uint64_t>(ts.tv_sec) * 1000000ULL + ts.tv_nsec / 1000ULL;

#elif defined(_WIN32)
    // Windows 使用高精度计时器
    static double freq_inv = 0.0;
    static LARGE_INTEGER freq;
    static bool initialized = false;

    if (!initialized)
    {
        QueryPerformanceFrequency(&freq);
        freq_inv = 1000000.0 / static_cast<double>(freq.QuadPart);
        initialized = true;
    }

    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart * freq_inv);

#else
    // 其他平台回退到 chrono
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch())
        .count();
#endif
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
        base = last_ms.load(std::memory_order_relaxed);
        if (now_ms > base)
        {
            // Will reset counter after CAS succeeds
        }
    }
    while (!last_ms.compare_exchange_weak(base, now_ms, std::memory_order_acq_rel));

    // Only reset counter if we successfully updated last_ms with a newer timestamp
    if (now_ms > base) {
        counter.store(0, std::memory_order_release);
    }

    // 3位 counter，可支撑同一毫秒内生成 1000 个 UID
    int64_t uid = now_ms * 1000 + counter.fetch_add(1, std::memory_order_relaxed);
    return uid;
}

std::string Helper::toHex(const void* data, size_t len)
{
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) oss << ' ';
        oss << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return oss.str();
}

std::string Helper::GetShortTypeName(const google::protobuf::Message& msg)
{
    const std::string& full = msg.GetTypeName();
    auto pos = full.rfind('.');
    if (pos == std::string::npos)
        return full;
    return full.substr(pos + 1);
}

std::shared_ptr<AppMsgWrapper> Helper::CreateSSPack(const google::protobuf::Message &message, Type type, const uint64_t& co_id)
{
    static std::atomic<uint32_t> now_seq_{0};

    // 获取message序列化后的长度，直接申请一块AppMsg+message_strlen大小的内存
    auto message_strlen = message.ByteSizeLong();
    DLOG << "CreateSSPack: message type=" << GetShortTypeName(message) << ", strlen=" << message_strlen;
    if (kssMsgNameToId.find(GetShortTypeName(message)) == kssMsgNameToId.end())
    {
        ELOG << "Message type not registered in kssMsgNameToId: " << GetShortTypeName(message);
        return nullptr;
    }
    int32_t msg_id = kssMsgNameToId.at(GetShortTypeName(message));

    auto& shm_slab_ = GlobalSpace()->shm_slab_;

    uint32_t data_len = message_strlen;
    uint32_t pack_len = sizeof(AppMsg) + data_len;

    // 从slab中分配内存块
    auto pack_shm_offset = shm_slab_.Alloc(pack_len);
    auto pack_shm_addr = shm_slab_.off2ptr(pack_shm_offset);

    auto msg = reinterpret_cast<AppMsg *>(pack_shm_addr);
    // 填充AppMsg Header
    msg->header_.version_ = MAGIC_VERSION;
    msg->header_.pack_len_ = pack_len;
    msg->header_.seq_ = now_seq_.fetch_add(1);
    // co_id传入为0，代表是req，否则为rsp
    if(co_id != 0)
    {
        msg->co_id_ = co_id;
    }
    msg->header_.type_ = type;

    msg->data_ = reinterpret_cast<char *>(pack_shm_addr) + sizeof(AppMsg);  // body紧跟在AppMsg结构体后面
    msg->data_len_ = data_len;                                              // body数据长度
    msg->msg_id_ = msg_id;                                                  // 消息ID

    // pb数据序列化到data_中
    if (!message.SerializeToArray(msg->data_, message_strlen))
    {
        ELOG << "Error to serialize message";
        shm_slab_.Free(pack_shm_offset, pack_len);
        return nullptr;
    }

    // custom deleter：shared_ptr 析构时自动归还 slab 内存
    auto pack = std::shared_ptr<AppMsgWrapper>(
        new AppMsgWrapper(),
        [pack_shm_offset, pack_len](AppMsgWrapper* p) {
            GlobalSpace()->shm_slab_.Free(pack_shm_offset, pack_len);
            delete p;
        });
    pack->offset_ = pack_shm_offset;

    return pack;
}

void Helper::DeleteSSPack(const AppMsgWrapper& pack)
{
    auto pack_shm_offset = pack.offset_;
    auto pack_size = reinterpret_cast<AppMsg*>(GlobalSpace()->shm_slab_.off2ptr(pack_shm_offset))->header_.pack_len_;
    GlobalSpace()->shm_slab_.Free(pack_shm_offset, pack_size);
}