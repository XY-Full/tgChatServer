#pragma once
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>

/**
 * @brief 客户端会话信息
 */
struct ClientSession
{
    uint64_t    conn_id   = 0;
    std::string user_id;                  // 鉴权成功后填充
    std::string proto;                    // "tcp" / "ws" / "kcp"
    bool        authed    = false;
    std::chrono::steady_clock::time_point connected_at;
};

/**
 * @brief 会话管理器
 *
 * 职责：
 *   - 维护 conn_id → session 双向映射
 *   - 维护 user_id → conn_id 映射（用于防重复登录踢旧连接）
 *   - 线程安全（shared_mutex 读写锁）
 *
 * 防重复登录策略：
 *   同一 user_id 已存在会话时，on_auth_ok 会返回旧的 conn_id，
 *   调用方负责主动关闭旧连接，然后再次调用 on_auth_ok 完成绑定。
 */
class SessionManager
{
public:
    SessionManager() = default;

    /**
     * @brief 新连接建立
     * @param conn_id  由 IListener 分配的连接 ID
     * @param proto    协议名称（"tcp"/"ws"/"kcp"）
     */
    void on_connect(uint64_t conn_id, const std::string& proto);

    /**
     * @brief 连接关闭，清理 session
     */
    void on_disconnect(uint64_t conn_id);

    /**
     * @brief 鉴权成功，绑定 user_id 到 conn_id
     * @return 若该 user_id 已有旧连接，返回旧 conn_id（调用方应先踢掉再重调）；否则返回 nullopt
     */
    std::optional<uint64_t> on_auth_ok(uint64_t conn_id, const std::string& user_id);

    /**
     * @brief 查询连接是否已鉴权
     */
    bool is_authed(uint64_t conn_id) const;

    /**
     * @brief 获取连接对应的 user_id，未鉴权返回空字符串
     */
    std::string get_user_id(uint64_t conn_id) const;

    /**
     * @brief 根据 user_id 查找当前连接 ID
     */
    std::optional<uint64_t> find_conn_by_user(const std::string& user_id) const;

    /**
     * @brief 获取当前在线连接数
     */
    size_t online_count() const;

    /**
     * @brief 获取 session 副本（用于日志/监控）
     */
    std::optional<ClientSession> get_session(uint64_t conn_id) const;

private:
    mutable std::shared_mutex                          mu_;
    std::unordered_map<uint64_t, ClientSession>        sessions_;
    std::unordered_map<std::string, uint64_t>          user_to_conn_;
};
