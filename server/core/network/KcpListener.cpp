#include "KcpListener.h"
#include "GlobalSpace.h"
#include "Helper.h"
#include "MsgWrapper.h"
#include "PackBase.h"
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

// ─────────────────────────────────────────────
// KCP output callback：将 KCP 封帧后的数据通过 UDP 发出
// ─────────────────────────────────────────────

int KcpListener::kcpOutput(const char* buf, int len, ikcpcb* /*kcp*/, void* user)
{
    auto* sess = static_cast<KcpSession*>(user);
    // udp_fd_ 通过 session 内存中存的指针获取
    // 这里采用全局获取：将 udp_fd 存到 session 内（见 ioLoop）
    // 实际实现：session 对象里存 udp_fd（通过 kcp->user 指针的 wrapper）
    // 为简洁，使用 static thread_local 变量传递 fd（acceptLoop 内 set）
    extern thread_local int tl_udp_fd;
    ::sendto(tl_udp_fd,
             buf, len, 0,
             reinterpret_cast<const sockaddr*>(&sess->addr),
             sizeof(sess->addr));
    return 0;
}

thread_local int tl_udp_fd = -1;

// ─────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────

std::string KcpListener::addrKey(const sockaddr_in& addr)
{
    std::ostringstream oss;
    oss << ntohl(addr.sin_addr.s_addr) << ":" << ntohs(addr.sin_port);
    return oss.str();
}

// ─────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────

bool KcpListener::start()
{
    udp_fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (udp_fd_ < 0)
    {
        ELOG << "KcpListener: socket() failed: " << strerror(errno);
        return false;
    }
    int opt = 1;
    setsockopt(udp_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port_));
    if (::bind(udp_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ELOG << "KcpListener: bind() failed on port " << port_ << ": " << strerror(errno);
        ::close(udp_fd_);
        return false;
    }

    running_ = true;
    io_thread_     = std::thread(&KcpListener::ioLoop, this);
    update_thread_ = std::thread(&KcpListener::updateLoop, this);
    ILOG << "KcpListener: started on UDP port " << port_;
    return true;
}

void KcpListener::stop()
{
    if (!running_.exchange(false)) return;
    if (udp_fd_ >= 0) { ::close(udp_fd_); udp_fd_ = -1; }
    if (io_thread_.joinable())     io_thread_.join();
    if (update_thread_.joinable()) update_thread_.join();

    std::lock_guard lk(mu_);
    for (auto& kv : conv_to_sess_)
        ikcp_release(kv.second->kcp);
    conv_to_sess_.clear();
    addr_to_conv_.clear();
    id_to_conv_.clear();
    ILOG << "KcpListener: stopped";
}

// ─────────────────────────────────────────────
// IO loop（UDP recv）
// ─────────────────────────────────────────────

void KcpListener::ioLoop()
{
    tl_udp_fd = udp_fd_;
    uint8_t buf[65536];
    while (running_)
    {
        sockaddr_in from{};
        socklen_t flen = sizeof(from);
        ssize_t n = ::recvfrom(udp_fd_, buf, sizeof(buf), 0,
                               reinterpret_cast<sockaddr*>(&from), &flen);
        if (n > 0)
        {
            processUdpPacket(buf, static_cast<size_t>(n), from);
        }
        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            if (running_) ELOG << "KcpListener: recvfrom error: " << strerror(errno);
            break;
        }
        else
        {
            // EAGAIN：无数据，短暂 sleep 避免 busy-loop
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void KcpListener::processUdpPacket(const uint8_t* data, size_t len, const sockaddr_in& from)
{
    if (len < 24) return;  // KCP 头部最小 24 字节 (IKCP_OVERHEAD 定义在 ikcp.c 中不可导出)

    // KCP conv 在包头第 0-3 字节（little-endian）
    uint32_t conv = 0;
    memcpy(&conv, data, 4);

    std::lock_guard lk(mu_);
    std::string key = addrKey(from);

    auto it = conv_to_sess_.find(conv);
    if (it == conv_to_sess_.end())
    {
        // 新会话
        uint64_t cid = next_conn_id_++;
        auto sess = std::make_shared<KcpSession>();
        sess->conn_id = cid;
        sess->addr    = from;
        sess->last_active = std::chrono::steady_clock::now();

        ikcpcb* kcp = ikcp_create(conv, sess.get());
        kcp->output = &KcpListener::kcpOutput;
        ikcp_nodelay(kcp, 1, 10, 2, 1);   // 极速模式
        ikcp_wndsize(kcp, 128, 128);
        sess->kcp = kcp;

        conv_to_sess_[conv] = sess;
        addr_to_conv_[key]  = conv;
        id_to_conv_[cid]    = conv;

        if (conn_handler_) conn_handler_(cid);
        ILOG << "KcpListener: new session conv=" << conv << " conn_id=" << cid;
        it = conv_to_sess_.find(conv);
    }

    auto& sess = it->second;
    sess->last_active = std::chrono::steady_clock::now();
    ikcp_input(sess->kcp, reinterpret_cast<const char*>(data), static_cast<long>(len));
    dispatchKcpData(*sess);
}

void KcpListener::dispatchKcpData(KcpSession& session)
{
    // 从 KCP 接收完整消息（可能多条）
    char buf[65536];
    while (true)
    {
        int n = ikcp_recv(session.kcp, buf, sizeof(buf));
        if (n < 0) break;

        // 解析 AppMsg
        if (static_cast<size_t>(n) < sizeof(Header)) continue;

        // 使用连续内存分配，将 AppMsg 和 data 放在同一块内存中，
        // 由 shared_ptr custom deleter 统一管理，避免 data_ 内存泄漏
        uint16_t msg_id   = 0;
        uint16_t data_len = 0;
        size_t off = sizeof(Header);
        if (static_cast<size_t>(n) >= off + 4)
        {
            memcpy(&msg_id,   buf + off,     sizeof(uint16_t));
            memcpy(&data_len, buf + off + 2, sizeof(uint16_t));
            off += 4;
        }

        size_t total_size = sizeof(AppMsg) + data_len;
        auto* raw = new uint8_t[total_size];
        auto* msg_base = reinterpret_cast<AppMsg*>(raw);
        new (msg_base) AppMsg{};  // placement new 初始化
        memcpy(&msg_base->header_, buf, sizeof(Header));
        msg_base->header_.conn_id_ = session.conn_id;
        msg_base->msg_id_   = msg_id;
        msg_base->data_len_ = data_len;
        if (data_len > 0 && static_cast<size_t>(n) >= off + data_len)
        {
            msg_base->data_ = reinterpret_cast<char*>(raw + sizeof(AppMsg));
            memcpy(msg_base->data_, buf + off, data_len);
        }
        else
        {
            msg_base->data_ = nullptr;
        }

        // custom deleter 确保释放整块 raw 数组（包含 data 区域）
        auto app_msg = std::shared_ptr<AppMsg>(msg_base, [raw](AppMsg* p) {
            p->~AppMsg();
            delete[] raw;
        });

        if (recv_handler_) recv_handler_(session.conn_id, app_msg);
    }
}

// ─────────────────────────────────────────────
// KCP update loop（驱动 ARQ 重传）
// ─────────────────────────────────────────────

void KcpListener::updateLoop()
{
    while (running_)
    {
        auto now_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());

        std::vector<uint32_t> expired;
        {
            std::lock_guard lk(mu_);
            for (auto& kv : conv_to_sess_)
            {
                ikcp_update(kv.second->kcp, now_ms);
                auto idle = std::chrono::steady_clock::now() - kv.second->last_active;
                if (std::chrono::duration_cast<std::chrono::seconds>(idle).count() > IDLE_TIMEOUT_SEC)
                    expired.push_back(kv.first);
            }
        }
        for (auto conv : expired) removeSession(conv);

        std::this_thread::sleep_for(std::chrono::milliseconds(KCP_UPDATE_INTERVAL_MS));
    }
}

// ─────────────────────────────────────────────
// Send & Close
// ─────────────────────────────────────────────

int32_t KcpListener::send(uint64_t conn_id, std::shared_ptr<AppMsgWrapper> pack)
{
    if (!pack) return -1;
    std::lock_guard lk(mu_);
    auto it = id_to_conv_.find(conn_id);
    if (it == id_to_conv_.end()) return -1;
    auto& sess = conv_to_sess_.at(it->second);

    auto& shm = GlobalSpace()->shm_slab_;
    auto* msg = reinterpret_cast<AppMsg*>(shm.base8() + pack->offset_);
    size_t total = sizeof(Header) + 4 + msg->data_len_;
    std::vector<uint8_t> raw(total);
    memcpy(raw.data(), &msg->header_, sizeof(Header));
    memcpy(raw.data() + sizeof(Header),     &msg->msg_id_,   2);
    memcpy(raw.data() + sizeof(Header) + 2, &msg->data_len_, 2);
    if (msg->data_len_ > 0 && msg->data_)
        memcpy(raw.data() + sizeof(Header) + 4, msg->data_, msg->data_len_);

    // KCP send（非阻塞，内部缓冲）
    tl_udp_fd = udp_fd_;
    int r = ikcp_send(sess->kcp, reinterpret_cast<const char*>(raw.data()), static_cast<int>(total));
    return r < 0 ? -1 : 0;
}

void KcpListener::close_conn(uint64_t conn_id)
{
    uint64_t cid = 0;
    {
        std::lock_guard lk(mu_);
        auto it = id_to_conv_.find(conn_id);
        if (it == id_to_conv_.end()) return;
        uint32_t conv = it->second;
        auto sit = conv_to_sess_.find(conv);
        if (sit == conv_to_sess_.end()) return;
        cid = sit->second->conn_id;
        std::string key = addrKey(sit->second->addr);
        ikcp_release(sit->second->kcp);
        sit->second->kcp = nullptr;
        conv_to_sess_.erase(sit);
        addr_to_conv_.erase(key);
        id_to_conv_.erase(conn_id);
    }
    // 将回调移到锁外执行，避免回调内再调用 close_conn 导致递归死锁
    if (cid && close_handler_) close_handler_(cid);
}

void KcpListener::removeSession(uint32_t conv)
{
    uint64_t cid = 0;
    {
        std::lock_guard lk(mu_);
        auto it = conv_to_sess_.find(conv);
        if (it == conv_to_sess_.end()) return;
        cid = it->second->conn_id;
        std::string key = addrKey(it->second->addr);
        ikcp_release(it->second->kcp);
        it->second->kcp = nullptr;
        conv_to_sess_.erase(it);
        addr_to_conv_.erase(key);
        id_to_conv_.erase(cid);
    }
    // 将回调移到锁外执行，避免回调内再调用 close_conn/removeSession 导致递归死锁
    if (cid && close_handler_) close_handler_(cid);
    ILOG << "KcpListener: session removed conv=" << conv;
}
