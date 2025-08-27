#ifndef CONFIG_MANAGER_H
#define CONFIG_MANAGER_H

#include <atomic>
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief 配置管理器类
 *
 * 负责配置文件的读取、写入和热加载
 * 支持JSON格式的配置文件
 * 支持配置项的层级访问（使用点号分隔）
 * 支持配置文件的热加载
 * 提供事件通知机制，在配置变更时通知订阅者
 */
class ConfigManager
{
public:
    /**
     * @brief 配置变更的事件类型
     */
    enum class EventType
    {
        CONFIG_LOADED,  ///< 配置文件已加载
        CONFIG_SAVED,   ///< 配置文件已保存
        CONFIG_CHANGED, ///< 配置项已更改
        CONFIG_RELOADED ///< 配置文件已重新加载
    };

    /**
     * @brief 配置事件结构体
     */
    struct ConfigEvent
    {
        EventType type;                                  ///< 事件类型
        std::string key;                                 ///< 相关的配置键（对于CONFIG_CHANGED）
        std::chrono::system_clock::time_point timestamp; ///< 事件时间戳
    };

    /**
     * @brief 配置事件监听器回调函数类型
     */
    using EventListener = std::function<void(const ConfigEvent &)>;

public:
    /**
     * @brief 构造函数
     */
    ConfigManager();

    /**
     * @brief 析构函数
     */
    ~ConfigManager();

    /**
     * @brief 加载配置文件
     * @param config_file 配置文件路径
     * @param auto_create 如果文件不存在是否自动创建
     * @return true表示成功，false表示失败
     */
    bool loadConfig(const std::string &config_file, bool auto_create = true);

    /**
     * @brief 保存配置文件
     * @param config_file 配置文件路径，如果为空则使用当前加载的文件路径
     * @return true表示成功，false表示失败
     */
    bool saveConfig(const std::string &config_file = "");

    /**
     * @brief 启用热加载
     * @param enabled 是否启用
     * @param check_interval_ms 检查间隔（毫秒）
     */
    void enableHotReload(bool enabled = true, int check_interval_ms = 1000);

    /**
     * @brief 检查配置键是否存在
     * @param key 配置键，支持使用点号分隔的多级键
     * @return true表示存在，false表示不存在
     */
    bool hasKey(const std::string &key) const;

    /**
     * @brief 获取配置值
     * @param key 配置键，支持使用点号分隔的多级键
     * @param default_value 默认值
     * @return 配置值，如果不存在则返回默认值
     */
    template <typename T> T getValue(const std::string &key, const T &default_value) const;

    /**
     * @brief 设置配置值
     * @param key 配置键，支持使用点号分隔的多级键
     * @param value 配置值
     * @param notify 是否触发通知事件
     */
    template <typename T> void setValue(const std::string &key, const T &value, bool notify = true);

    /**
     * @brief 获取整个配置的JSON字符串表示
     * @param pretty 是否格式化输出
     * @return JSON字符串
     */
    std::string getJsonString(bool pretty = true) const;

    /**
     * @brief 从JSON字符串加载配置
     * @param json_str JSON字符串
     * @param merge 是否合并到现有配置（false则替换整个配置）
     * @return true表示成功，false表示失败
     */
    bool loadFromJsonString(const std::string &json_str, bool merge = true);

    /**
     * @brief 注册配置事件监听器
     * @param listener 监听器回调函数
     * @return 监听器ID，用于后续取消注册
     */
    int registerEventListener(EventListener listener);

    /**
     * @brief 取消注册配置事件监听器
     * @param listener_id 监听器ID
     * @return true表示成功，false表示失败
     */
    bool unregisterEventListener(int listener_id);

    /**
     * @brief 获取所有配置键
     * @param prefix 键前缀（可选）
     * @return 配置键列表
     */
    std::vector<std::string> getAllKeys(const std::string &prefix = "") const;

    /**
     * @brief 清除所有配置
     */
    void clear();

    /**
     * @brief 获取当前加载的配置文件路径
     * @return 配置文件路径
     */
    std::string getConfigFilePath() const;

    /**
     * @brief 检查配置文件是否已修改（用于热加载）
     * @return true表示已修改，false表示未修改
     */
    bool isConfigFileModified() const;

    /**
     * @brief 获取配置文件最后修改时间
     * @return 最后修改时间
     */
    std::chrono::system_clock::time_point getLastModifiedTime() const;

private:
    /**
     * @brief 触发配置事件
     * @param type 事件类型
     * @param key 相关的配置键（可选）
     */
    void fireEvent(EventType type, const std::string &key = "");

    /**
     * @brief 启动热加载监控线程
     */
    void startHotReloadMonitor();

    /**
     * @brief 停止热加载监控线程
     */
    void stopHotReloadMonitor();

    /**
     * @brief 热加载监控线程函数
     */
    void hotReloadMonitorThread();

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// 特化的模板函数声明
// 这些特化将直接使用nlohmann::json的get<T>()和operator=进行实现
template <>
std::string ConfigManager::getValue<std::string>(const std::string &key, const std::string &default_value) const;
template <> int ConfigManager::getValue<int>(const std::string &key, const int &default_value) const;
template <> double ConfigManager::getValue<double>(const std::string &key, const double &default_value) const;
template <> bool ConfigManager::getValue<bool>(const std::string &key, const bool &default_value) const;
template <>
std::vector<std::string> ConfigManager::getValue<std::vector<std::string>>(
    const std::string &key, const std::vector<std::string> &default_value) const;

template <> void ConfigManager::setValue<std::string>(const std::string &key, const std::string &value, bool notify);
template <> void ConfigManager::setValue<int>(const std::string &key, const int &value, bool notify);
template <> void ConfigManager::setValue<double>(const std::string &key, const double &value, bool notify);
template <> void ConfigManager::setValue<bool>(const std::string &key, const bool &value, bool notify);
template <>
void ConfigManager::setValue<std::vector<std::string>>(const std::string &key, const std::vector<std::string> &value,
                                                       bool notify);

#endif // CONFIG_MANAGER_H