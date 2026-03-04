#include "SessionManager.h"
#include <mutex>
#include <shared_mutex>

void SessionManager::on_connect(uint64_t conn_id, const std::string& proto)
{
    std::unique_lock lk(mu_);
    ClientSession s;
    s.conn_id      = conn_id;
    s.proto        = proto;
    s.authed       = false;
    s.connected_at = std::chrono::steady_clock::now();
    sessions_[conn_id] = std::move(s);
}

void SessionManager::on_disconnect(uint64_t conn_id)
{
    std::unique_lock lk(mu_);
    auto it = sessions_.find(conn_id);
    if (it == sessions_.end()) return;
    if (it->second.authed && !it->second.user_id.empty())
    {
        // 仅当 user→conn 映射还指向此 conn_id 时才删除（可能已被新连接覆盖）
        auto uit = user_to_conn_.find(it->second.user_id);
        if (uit != user_to_conn_.end() && uit->second == conn_id)
            user_to_conn_.erase(uit);
    }
    sessions_.erase(it);
}

std::optional<uint64_t> SessionManager::on_auth_ok(uint64_t conn_id, const std::string& user_id)
{
    std::unique_lock lk(mu_);

    // 检查 user_id 是否已有旧连接
    auto uit = user_to_conn_.find(user_id);
    if (uit != user_to_conn_.end() && uit->second != conn_id)
    {
        // 返回旧 conn_id，调用方负责踢掉旧连接
        uint64_t old_conn = uit->second;
        return old_conn;
    }

    // 绑定
    auto it = sessions_.find(conn_id);
    if (it == sessions_.end()) return std::nullopt;
    it->second.user_id = user_id;
    it->second.authed  = true;
    user_to_conn_[user_id] = conn_id;
    return std::nullopt;
}

bool SessionManager::is_authed(uint64_t conn_id) const
{
    std::shared_lock lk(mu_);
    auto it = sessions_.find(conn_id);
    return it != sessions_.end() && it->second.authed;
}

std::string SessionManager::get_user_id(uint64_t conn_id) const
{
    std::shared_lock lk(mu_);
    auto it = sessions_.find(conn_id);
    if (it == sessions_.end()) return {};
    return it->second.user_id;
}

std::optional<uint64_t> SessionManager::find_conn_by_user(const std::string& user_id) const
{
    std::shared_lock lk(mu_);
    auto it = user_to_conn_.find(user_id);
    if (it == user_to_conn_.end()) return std::nullopt;
    return it->second;
}

size_t SessionManager::online_count() const
{
    std::shared_lock lk(mu_);
    return sessions_.size();
}

std::optional<ClientSession> SessionManager::get_session(uint64_t conn_id) const
{
    std::shared_lock lk(mu_);
    auto it = sessions_.find(conn_id);
    if (it == sessions_.end()) return std::nullopt;
    return it->second;
}
