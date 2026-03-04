#include "WsListener.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "MsgWrapper.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sstream>

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

std::string WsListener::base64Encode(const uint8_t* data, size_t len)
{
    BIO* b64  = BIO_new(BIO_f_base64());
    BIO* bmem = BIO_new(BIO_s_mem());
    b64 = BIO_push(b64, bmem);
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO_write(b64, data, static_cast<int>(len));
    BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);
    std::string result(bptr->data, bptr->length);
    BIO_free_all(b64);
    return result;
}

std::vector<uint8_t> WsListener::buildFrame(const uint8_t* data, size_t len)
{
    std::vector<uint8_t> frame;
    frame.push_back(0x82); // FIN=1, opcode=2 (binary)
    if (len <= 125)
    {
        frame.push_back(static_cast<uint8_t>(len));
    }
    else if (len <= 65535)
    {
        frame.push_back(126);
        frame.push_back(static_cast<uint8_t>((len >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(len & 0xFF));
    }
    else
    {
        frame.push_back(127);
        for (int i = 7; i >= 0; --i)
            frame.push_back(static_cast<uint8_t>((len >> (8 * i)) & 0xFF));
    }
    frame.insert(frame.end(), data, data + len);
    return frame;
}

// ─────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────

bool WsListener::start()
{
    server_fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (server_fd_ < 0)
    {
        ELOG << "WsListener: socket() failed: " << strerror(errno);
        return false;
    }
    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    setsockopt(server_fd_, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));
    if (::bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ELOG << "WsListener: bind() failed on port " << port_ << ": " << strerror(errno);
        ::close(server_fd_);
        return false;
    }
    if (::listen(server_fd_, 128) < 0)
    {
        ELOG << "WsListener: listen() failed: " << strerror(errno);
        ::close(server_fd_);
        return false;
    }

    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0)
    {
        ELOG << "WsListener: epoll_create1() failed";
        ::close(server_fd_);
        return false;
    }

    epoll_event ev{};
    ev.events  = EPOLLIN | EPOLLET;
    ev.data.fd = server_fd_;
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, server_fd_, &ev);

    running_ = true;
    io_thread_ = std::thread(&WsListener::acceptLoop, this);
    ILOG << "WsListener: started on port " << port_;
    return true;
}

void WsListener::stop()
{
    if (!running_.exchange(false))
        return;
    if (server_fd_ >= 0) { ::close(server_fd_); server_fd_ = -1; }
    if (epoll_fd_ >= 0)  { ::close(epoll_fd_);  epoll_fd_  = -1; }
    if (io_thread_.joinable()) io_thread_.join();
    ILOG << "WsListener: stopped";
}

// ─────────────────────────────────────────────
// IO loop
// ─────────────────────────────────────────────

void WsListener::acceptLoop()
{
    epoll_event events[MAX_EVENTS];
    while (running_)
    {
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 1000);
        if (n < 0)
        {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < n; ++i)
        {
            int fd = events[i].data.fd;
            if (fd == server_fd_)
            {
                // accept new connections
                while (true)
                {
                    sockaddr_in cli{};
                    socklen_t   clen = sizeof(cli);
                    int cfd = ::accept4(server_fd_, reinterpret_cast<sockaddr*>(&cli), &clen, SOCK_NONBLOCK);
                    if (cfd < 0) break;

                    uint64_t cid = next_conn_id_++;
                    auto conn = std::make_shared<WsConn>();
                    conn->fd       = cfd;
                    conn->conn_id  = cid;
                    conn->last_active = std::chrono::steady_clock::now();

                    {
                        std::lock_guard lk(conn_mutex_);
                        fd_to_conn_[cfd] = conn;
                        id_to_conn_[cid] = conn;
                    }

                    epoll_event cev{};
                    cev.events  = EPOLLIN | EPOLLET | EPOLLRDHUP;
                    cev.data.fd = cfd;
                    ::epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &cev);

                    ILOG << "WsListener: new connection cfd=" << cfd << " conn_id=" << cid;
                }
            }
            else
            {
                handleEvent(fd, events[i].events);
            }
        }
    }
}

void WsListener::handleEvent(int fd, uint32_t events)
{
    std::shared_ptr<WsConn> conn;
    {
        std::lock_guard lk(conn_mutex_);
        auto it = fd_to_conn_.find(fd);
        if (it == fd_to_conn_.end()) return;
        conn = it->second;
    }

    if (events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR))
    {
        removeConn(fd);
        return;
    }

    if (events & EPOLLIN)
    {
        conn->last_active = std::chrono::steady_clock::now();
        readData(*conn);
    }
}

void WsListener::readData(WsConn& conn)
{
    uint8_t buf[4096];
    while (true)
    {
        ssize_t n = ::read(conn.fd, buf, sizeof(buf));
        if (n > 0)
        {
            conn.recv_buf.insert(conn.recv_buf.end(), buf, buf + n);
        }
        else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK))
        {
            removeConn(conn.fd);
            return;
        }
        else
        {
            break; // EAGAIN
        }
    }

    if (!conn.handshaked)
    {
        doHandshake(conn);
    }
    else
    {
        parseFrames(conn);
    }
}

// ─────────────────────────────────────────────
// WS Handshake
// ─────────────────────────────────────────────

bool WsListener::doHandshake(WsConn& conn)
{
    std::string raw(conn.recv_buf.begin(), conn.recv_buf.end());
    auto hdr_end = raw.find("\r\n\r\n");
    if (hdr_end == std::string::npos) return false; // 头部未接收完

    // 提取 Sec-WebSocket-Key
    std::string key;
    auto pos = raw.find("Sec-WebSocket-Key:");
    if (pos == std::string::npos)
    {
        removeConn(conn.fd);
        return false;
    }
    pos += 18;
    while (pos < raw.size() && raw[pos] == ' ') ++pos;
    auto end = raw.find("\r\n", pos);
    key = raw.substr(pos, end - pos);

    // 计算 accept
    std::string accept_input = key + WS_GUID;
    uint8_t sha1[20];
    SHA1(reinterpret_cast<const uint8_t*>(accept_input.c_str()), accept_input.size(), sha1);
    std::string accept = base64Encode(sha1, 20);

    std::string response =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " + accept + "\r\n\r\n";

    ::write(conn.fd, response.data(), response.size());
    conn.handshaked = true;
    conn.recv_buf.erase(conn.recv_buf.begin(), conn.recv_buf.begin() + hdr_end + 4);

    if (conn_handler_) conn_handler_(conn.conn_id);
    ILOG << "WsListener: handshake OK conn_id=" << conn.conn_id;

    // 残留数据可能是紧跟着的第一帧
    if (!conn.recv_buf.empty()) parseFrames(conn);
    return true;
}

// ─────────────────────────────────────────────
// WS Frame Parser
// ─────────────────────────────────────────────

bool WsListener::parseFrames(WsConn& conn)
{
    auto& buf = conn.recv_buf;
    while (buf.size() >= 2)
    {
        uint8_t b0 = buf[0];
        uint8_t b1 = buf[1];
        bool  fin    = (b0 & 0x80) != 0;
        uint8_t opcode = b0 & 0x0F;
        bool  masked = (b1 & 0x80) != 0;
        uint64_t payload_len = b1 & 0x7F;

        size_t hdr_size = 2;
        if (payload_len == 126)      hdr_size += 2;
        else if (payload_len == 127) hdr_size += 8;
        if (masked) hdr_size += 4;

        if (buf.size() < hdr_size) break;

        size_t offset = 2;
        if (payload_len == 126)
        {
            payload_len = (static_cast<uint64_t>(buf[2]) << 8) | buf[3];
            offset = 4;
        }
        else if (payload_len == 127)
        {
            payload_len = 0;
            for (int i = 0; i < 8; ++i)
                payload_len = (payload_len << 8) | buf[2 + i];
            offset = 10;
        }

        if (buf.size() < hdr_size + payload_len) break;

        uint8_t mask[4] = {};
        if (masked)
        {
            memcpy(mask, buf.data() + offset, 4);
            offset += 4;
        }

        std::vector<uint8_t> payload(buf.begin() + offset, buf.begin() + offset + payload_len);
        if (masked)
        {
            for (size_t i = 0; i < payload.size(); ++i)
                payload[i] ^= mask[i % 4];
        }

        // 移除已处理数据
        buf.erase(buf.begin(), buf.begin() + hdr_size + payload_len);

        if (opcode == 0x8) // close
        {
            removeConn(conn.fd);
            return false;
        }
        if (opcode == 0x9) // ping
        {
            // 回 pong
            std::vector<uint8_t> pong = {0x8A, 0x00};
            ::write(conn.fd, pong.data(), pong.size());
            continue;
        }
        if (opcode != 0x2 && opcode != 0x1) continue; // 只处理 binary/text

        // payload 按 AppMsg 二进制协议解析（PackBase 格式）
        if (payload.size() < sizeof(Header)) continue;

        auto app_msg = std::make_shared<AppMsg>();
        memcpy(&app_msg->header_, payload.data(), sizeof(Header));
        app_msg->header_.conn_id_ = conn.conn_id;

        size_t data_offset = sizeof(Header);
        if (payload.size() >= data_offset + sizeof(uint16_t) * 2)
        {
            memcpy(&app_msg->msg_id_,   payload.data() + data_offset, sizeof(uint16_t));
            memcpy(&app_msg->data_len_, payload.data() + data_offset + 2, sizeof(uint16_t));
            data_offset += 4;
        }

        // data_ 指向 payload 内部（与 TCP 路径不同，这里是栈上 vector，需要 new）
        uint16_t dlen = app_msg->data_len_;
        if (dlen > 0 && payload.size() >= data_offset + dlen)
        {
            auto* data_buf = new char[dlen];
            memcpy(data_buf, payload.data() + data_offset, dlen);
            app_msg->data_ = data_buf;
        }

        if (recv_handler_) recv_handler_(conn.conn_id, app_msg);
    }
    return true;
}

// ─────────────────────────────────────────────
// Send & Close
// ─────────────────────────────────────────────

int32_t WsListener::send(uint64_t conn_id, std::shared_ptr<AppMsgWrapper> pack)
{
    std::shared_ptr<WsConn> conn;
    {
        std::lock_guard lk(conn_mutex_);
        auto it = id_to_conn_.find(conn_id);
        if (it == id_to_conn_.end()) return -1;
        conn = it->second;
    }

    if (!pack) return -1;
    auto& shm = GlobalSpace()->shm_slab_;
    auto* msg = reinterpret_cast<AppMsg*>(shm.base8() + pack->offset_);
    size_t total = sizeof(Header) + sizeof(uint16_t) * 2 + msg->data_len_;
    std::vector<uint8_t> raw(total);
    memcpy(raw.data(), &msg->header_, sizeof(Header));
    memcpy(raw.data() + sizeof(Header), &msg->msg_id_, sizeof(uint16_t));
    memcpy(raw.data() + sizeof(Header) + 2, &msg->data_len_, sizeof(uint16_t));
    if (msg->data_len_ > 0 && msg->data_)
        memcpy(raw.data() + sizeof(Header) + 4, msg->data_, msg->data_len_);

    auto frame = buildFrame(raw.data(), raw.size());
    ssize_t n = ::write(conn->fd, frame.data(), frame.size());
    return (n > 0) ? 0 : -1;
}

void WsListener::close_conn(uint64_t conn_id)
{
    int fd = -1;
    {
        std::lock_guard lk(conn_mutex_);
        auto it = id_to_conn_.find(conn_id);
        if (it == id_to_conn_.end()) return;
        fd = it->second->fd;
    }
    if (fd >= 0) removeConn(fd);
}

void WsListener::removeConn(int fd)
{
    uint64_t conn_id = 0;
    {
        std::lock_guard lk(conn_mutex_);
        auto it = fd_to_conn_.find(fd);
        if (it == fd_to_conn_.end()) return;
        conn_id = it->second->conn_id;
        id_to_conn_.erase(conn_id);
        fd_to_conn_.erase(it);
    }
    ::epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
    ::close(fd);
    if (close_handler_) close_handler_(conn_id);
    ILOG << "WsListener: connection closed conn_id=" << conn_id;
}
