/**
 * connd 测试客户端
 *
 * 用法：
 *   ./client                   # 自动化测试：connect → login → heart×3 → quit
 *   ./client -i                # 交互式 REPL
 *   ./client -i -h 127.0.0.1 -p 7000
 *
 * 包格式（与服务端 TcpConnection::processRecvBuffer 完全对齐）：
 *
 *   服务端内部格式（SS格式）：[完整 AppMsg 结构体][protobuf data]
 *
 *   AppMsg 结构体布局（#pragma pack(1)，共 70 bytes）：
 *     Header (18 bytes):
 *       uint8_t  version_   = 0x01
 *       uint8_t  type_      = Type::C2S (0x01)
 *       uint32_t pack_len_  = sizeof(AppMsg) + protobuf_bytes
 *       uint32_t seq_
 *       uint64_t conn_id_   = 0
 *     uint16_t msg_id_
 *     uint16_t data_len_
 *     char*    data_        = 8 bytes (placeholder, server rewrites this)
 *     uint64_t co_id_       = 0
 *     char     src_name_[16] = ""
 *     char     dst_name_[16] = ""
 *   [protobuf bytes紧随其后]
 *
 * msg_id 约定：
 *   CS_HEART_BEAT = 0
 *   CS_LOGIN      = 100  (connd 自定义)
 *   SC_LOGIN_RSP  = 200  (connd 自定义)
 */

#include "gate.pb.h"
#include "login.pb.h"
#include "err_code.pb.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

// ─────────────────────────────────────────────────────────────────────
// 包格式（与服务端 PackBase/AppMsg 完全对齐，#pragma pack(1)）
// ─────────────────────────────────────────────────────────────────────

#pragma pack(push, 1)

struct ClientHeader
{
    uint8_t  version_;   // = 0x01
    uint8_t  type_;      // 1=C2S, 2=S2C
    uint32_t pack_len_;  // 整包字节数（sizeof(AppMsgFrame) + data_bytes）
    uint32_t seq_;
    uint64_t conn_id_;   // = 0
};

// 完整 AppMsg 帧头（与服务端 AppMsg 大小相同，70 bytes）
struct AppMsgFrame
{
    // PackBase 部分
    ClientHeader header_;    // 18 bytes
    uint16_t     msg_id_;    // 2 bytes
    uint16_t     data_len_;  // 2 bytes
    uint64_t     data_ptr_;  // 8 bytes  (char* placeholder, server overwrites)
    // AppMsg 额外部分
    uint64_t     co_id_;     // 8 bytes
    char         src_name_[16]; // 16 bytes
    char         dst_name_[16]; // 16 bytes
    // Total = 70 bytes
};

#pragma pack(pop)

static_assert(sizeof(ClientHeader) == 18, "ClientHeader must be 18 bytes");
static_assert(sizeof(AppMsgFrame)  == 70, "AppMsgFrame must be 70 bytes");

// 整包 overhead = sizeof(AppMsgFrame)
static constexpr size_t  kFrameOverhead = sizeof(AppMsgFrame); // 70
static constexpr uint8_t kMagicVersion  = 0x01;
static constexpr uint8_t kTypeC2S       = 0x01;

static constexpr uint16_t kMsgHeartBeat = 0;
static constexpr uint16_t kMsgLogin     = 100;
static constexpr uint16_t kMsgLoginRsp  = 200;
static constexpr uint16_t kMsgHeartRsp  = 0;   // SC_HEART_BEAT 同 msg_id

// 预生成测试 JWT（secret="change-me-in-production", sub="test_user_1", exp=9999999999）
static const char* kTestJwtToken =
    "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9"
    ".eyJzdWIiOiJ0ZXN0X3VzZXJfMSIsImlhdCI6MTcwMDAwMDAwMCwiZXhwIjo5OTk5OTk5OTk5fQ"
    ".CPaVveRjQSfBtESvZWOxG4SaqP0e_AqE6za6FVJrh_c";

// ─────────────────────────────────────────────────────────────────────
// 辅助：十六进制打印
// ─────────────────────────────────────────────────────────────────────

static void hexdump(const void* data, size_t len, const char* label = "")
{
    const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
    std::cout << "[hex " << label << " (" << len << "B)] ";
    for (size_t i = 0; i < len && i < 80; ++i)
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)p[i] << " ";
    if (len > 80) std::cout << "...";
    std::cout << std::dec << "\n";
}

// ─────────────────────────────────────────────────────────────────────
// ConndClient
// ─────────────────────────────────────────────────────────────────────

class ConndClient
{
public:
    explicit ConndClient(const std::string& host = "127.0.0.1", int port = 7000)
        : host_(host), port_(port)
    {}

    ~ConndClient() { disconnect(); }

    bool connect()
    {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) { perror("socket"); return false; }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(static_cast<uint16_t>(port_));
        if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) <= 0)
        {
            std::cerr << "inet_pton failed for " << host_ << "\n";
            ::close(fd_); fd_ = -1;
            return false;
        }

        if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            perror("connect");
            ::close(fd_); fd_ = -1;
            return false;
        }
        std::cout << "[client] connected to " << host_ << ":" << port_ << "\n";
        return true;
    }

    void disconnect()
    {
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }

    bool connected() const { return fd_ >= 0; }

    // ── 发送一帧（服务端 SS 格式：[AppMsgFrame(70B)][proto bytes]）────
    bool sendPack(uint16_t msg_id, const google::protobuf::Message& msg)
    {
        std::string body = msg.SerializeAsString();
        uint32_t total   = static_cast<uint32_t>(kFrameOverhead + body.size());

        std::string frame(total, '\0');
        AppMsgFrame* hdr = reinterpret_cast<AppMsgFrame*>(frame.data());

        hdr->header_.version_  = kMagicVersion;
        hdr->header_.type_     = kTypeC2S;
        hdr->header_.pack_len_ = total;
        hdr->header_.seq_      = ++seq_;
        hdr->header_.conn_id_  = 0;
        hdr->msg_id_           = msg_id;
        hdr->data_len_         = static_cast<uint16_t>(body.size());
        hdr->data_ptr_         = 0;   // server will rewrite
        hdr->co_id_            = 0;
        memset(hdr->src_name_, 0, sizeof(hdr->src_name_));
        memset(hdr->dst_name_, 0, sizeof(hdr->dst_name_));

        if (!body.empty())
            memcpy(frame.data() + kFrameOverhead, body.data(), body.size());

        hexdump(frame.data(), frame.size(), "send");

        ssize_t sent = ::send(fd_, frame.data(), frame.size(), MSG_NOSIGNAL);
        if (sent < 0) { perror("send"); return false; }
        return true;
    }

    // ── 接收一帧（服务端 SS 格式：[AppMsgFrame(70B)][proto bytes]）────
    struct RecvResult
    {
        bool     ok      = false;
        uint16_t msg_id  = 0;
        std::string data;
    };

    RecvResult recvPack(int timeout_sec = 5)
    {
        RecvResult res;

        struct timeval tv{};
        tv.tv_sec = timeout_sec;
        setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // 读完整的 AppMsgFrame（70B）
        std::string buf(kFrameOverhead, '\0');
        if (!recvExact(buf.data(), kFrameOverhead)) return res;

        hexdump(buf.data(), kFrameOverhead, "recv-hdr");

        const AppMsgFrame* hdr = reinterpret_cast<const AppMsgFrame*>(buf.data());

        if (hdr->header_.version_ != kMagicVersion)
        {
            std::cerr << "[client] bad version=0x"
                      << std::hex << (int)hdr->header_.version_ << std::dec << "\n";
            return res;
        }

        res.msg_id = hdr->msg_id_;
        uint16_t dlen = hdr->data_len_;

        if (dlen > 0)
        {
            res.data.resize(dlen);
            if (!recvExact(res.data.data(), dlen)) return res;
        }

        // pack_len 包含整个帧头；如果 pack_len > kFrameOverhead + dlen，跳过剩余
        uint32_t declared_total = hdr->header_.pack_len_;
        uint32_t expected_total = static_cast<uint32_t>(kFrameOverhead) + dlen;
        if (declared_total > expected_total)
        {
            size_t skip = declared_total - expected_total;
            std::string tmp(skip, '\0');
            recvExact(tmp.data(), skip); // ignore error
        }

        res.ok = true;
        return res;
    }

    // ── 高级操作 ─────────────────────────────────────────────────────

    bool login(const std::string& token = kTestJwtToken)
    {
        std::cout << "[login] sending Login.Request (token=" << token.substr(0, 20) << "...)\n";

        Login req;
        req.mutable_request()->set_account(token);
        req.mutable_request()->set_platform("jwt");

        if (!sendPack(kMsgLogin, req)) return false;

        auto res = recvPack();
        if (!res.ok)
        {
            std::cerr << "[login] no response\n";
            return false;
        }

        if (res.msg_id != kMsgLoginRsp && res.msg_id != kMsgLogin)
        {
            std::cerr << "[login] unexpected msg_id=" << res.msg_id << "\n";
            return false;
        }

        Login rsp;
        rsp.ParseFromString(res.data);
        int32_t err = rsp.response().err();
        std::cout << "[login] response: err=" << err
                  << " uid=" << rsp.response().uid()
                  << " token/msg=" << rsp.response().token() << "\n";

        if (err == 1) // Error_success = 1
        {
            std::cout << "[login] SUCCESS - authenticated as user_id=" << rsp.response().token() << "\n";
            return true;
        }
        std::cerr << "[login] FAILED - server error: " << rsp.response().token() << "\n";
        return false;
    }

    bool heart()
    {
        std::cout << "[heart] sending Heart.Request\n";

        Heart req;
        req.mutable_request(); // 空请求

        if (!sendPack(kMsgHeartBeat, req)) return false;

        auto res = recvPack();
        if (!res.ok)
        {
            std::cerr << "[heart] no response\n";
            return false;
        }

        Heart rsp;
        rsp.ParseFromString(res.data);
        std::cout << "[heart] response: err=" << rsp.response().err()
                  << " timestamp=" << rsp.response().timestamp() << "\n";

        if (rsp.response().err() == 1)
        {
            std::cout << "[heart] SUCCESS\n";
            return true;
        }
        return false;
    }

    // ── 自动测试 ─────────────────────────────────────────────────────

    void autoTest()
    {
        std::cout << "=== autoTest: connect → login → heart×3 → disconnect ===\n";

        if (!connect()) return;

        if (!login())
        {
            std::cout << "[autoTest] login failed, aborting\n";
            disconnect();
            return;
        }

        for (int i = 0; i < 3; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            heart();
        }

        std::cout << "=== autoTest done ===\n";
        disconnect();
    }

    // ── 交互式 REPL ──────────────────────────────────────────────────

    void repl()
    {
        std::cout << "=== interactive mode ===\n";
        std::cout << "commands: connect, disconnect, login [token], heart, quit\n";

        std::string line;
        while (std::cout << "> " && std::getline(std::cin, line))
        {
            if (line.empty()) continue;

            std::istringstream ss(line);
            std::string cmd;
            ss >> cmd;

            if (cmd == "quit" || cmd == "exit" || cmd == "q")
            {
                break;
            }
            else if (cmd == "connect")
            {
                if (connected())
                    std::cout << "[repl] already connected\n";
                else
                    connect();
            }
            else if (cmd == "disconnect")
            {
                disconnect();
                std::cout << "[repl] disconnected\n";
            }
            else if (cmd == "login")
            {
                if (!connected()) { std::cout << "[repl] not connected\n"; continue; }
                std::string tok;
                ss >> tok;
                if (tok.empty()) tok = kTestJwtToken;
                login(tok);
            }
            else if (cmd == "heart")
            {
                if (!connected()) { std::cout << "[repl] not connected\n"; continue; }
                heart();
            }
            else
            {
                std::cout << "[repl] unknown command: " << cmd << "\n";
                std::cout << "commands: connect, disconnect, login [token], heart, quit\n";
            }
        }

        disconnect();
        std::cout << "=== bye ===\n";
    }

private:
    bool recvExact(char* buf, size_t n)
    {
        size_t got = 0;
        while (got < n)
        {
            ssize_t r = ::recv(fd_, buf + got, n - got, 0);
            if (r <= 0)
            {
                if (r == 0)
                    std::cerr << "[client] connection closed by server\n";
                else
                    perror("recv");
                return false;
            }
            got += static_cast<size_t>(r);
        }
        return true;
    }

    std::string host_;
    int         port_;
    int         fd_  = -1;
    uint32_t    seq_ = 0;
};

// ─────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────

static void printUsage(const char* argv0)
{
    std::cout << "Usage: " << argv0 << " [-i] [-h host] [-p port]\n"
              << "  (no flags)  auto test: connect → login → heart×3\n"
              << "  -i          interactive REPL\n"
              << "  -h host     server host  (default: 127.0.0.1)\n"
              << "  -p port     server port  (default: 7000)\n";
}

int main(int argc, char* argv[])
{
    bool        interactive = false;
    std::string host        = "127.0.0.1";
    int         port        = 7000;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];
        if (arg == "-i")
        {
            interactive = true;
        }
        else if (arg == "-h" && i + 1 < argc)
        {
            host = argv[++i];
        }
        else if (arg == "-p" && i + 1 < argc)
        {
            port = std::stoi(argv[++i]);
        }
        else if (arg == "--help" || arg == "-?")
        {
            printUsage(argv[0]);
            return 0;
        }
    }

    ConndClient client(host, port);

    if (interactive)
    {
        client.connect();
        client.repl();
    }
    else
    {
        client.autoTest();
    }

    return 0;
}
