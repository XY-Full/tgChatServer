#include "ConfigManager.h"
#include "JsonParser.h"
#include "Log.h"
#include <filesystem>

// 定义默认配置
static const char *DEFAULT_CONFIG_CONTENT = R"({
    "app": {
        "name": "DefaultApp",
        "version": "1.0.0"
    },
    "logging": {
        "level": "INFO",
        "file": "app.log"
    },
    "network": {
        "port": 8080,
        "timeout_ms": 5000
    }
})";

class ConfigManager::Impl
{
public:
    nlohmann::json config_root;
    std::string config_file_path;
    std::chrono::system_clock::time_point last_modified_time;
    std::atomic<bool> hot_reload_enabled;
    std::atomic<bool> hot_reload_running;
    int check_interval_ms;
    std::thread hot_reload_thread;
    mutable std::mutex config_mutex;    // Protects config_root
    mutable std::mutex listeners_mutex; // Protects event_listeners
    std::map<int, EventListener> event_listeners;
    int next_listener_id;
    std::unique_ptr<JsonParser> parser; // 使用抽象解析器

    Impl()
        : hot_reload_enabled(false), hot_reload_running(false), check_interval_ms(1000), next_listener_id(0),
          parser(std::make_unique<JsonParser>())
    {
        // Initialize with an empty JSON object
        config_root = nlohmann::json::object();
    }

    ~Impl()
    {
    }

    // Helper to get file last modified time
    std::chrono::system_clock::time_point getFileModifiedTime(const std::string &path) const
    {
        try
        {
            // std::filesystem::last_write_time returns a file_time_type
            // We need to convert it to system_clock::time_point
            // This conversion might be tricky and platform-dependent.
            // A common approach is to convert to time_t first.
            auto ftime = std::filesystem::last_write_time(path);
            return std::chrono::system_clock::from_time_t(
                std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count());
        }
        catch (const std::filesystem::filesystem_error &e)
        {
            ELOG << "Error getting file modified time for " << path << ": " << e.what();
            return std::chrono::system_clock::time_point::min();
        }
    }

    // Helper to create default config file
    bool createDefaultConfig(const std::string &filePath)
    {
        nlohmann::json defaultConfig;
        if (!parser->loadFromString(DEFAULT_CONFIG_CONTENT, defaultConfig))
        {
            ELOG << "Error: Failed to parse default config content.";
            return false;
        }
        return parser->saveToFile(filePath, defaultConfig);
    }

    // Helper to get JSON value by key path
    template <typename T> T getJsonValue(const std::string &key, const T &defaultValue) const
    {
        std::lock_guard<std::mutex> lock(config_mutex);
        auto node = parser->getValue(config_root, key);
        if (node && !node->is_null())
        {
            try
            {
                return node->get<T>();
            }
            catch (const nlohmann::json::exception &e)
            {
                ELOG << "Warning: Type mismatch for key '" << key << "'. " << e.what()
                          << ". Returning default value.";
            }
        }
        return defaultValue;
    }

    // Helper to set JSON value by key path
    template <typename T> void setJsonValue(const std::string &key, const T &value)
    {
        std::lock_guard<std::mutex> lock(config_mutex);
        parser->setValue(config_root, key, value);
    }

    // Specialization for std::vector<std::string>
    std::vector<std::string> getJsonValue(const std::string &key, const std::vector<std::string> &defaultValue) const
    {
        std::lock_guard<std::mutex> lock(config_mutex);
        auto node = parser->getValue(config_root, key);
        if (node && node->is_array())
        {
            try
            {
                return node->get<std::vector<std::string>>();
            }
            catch (const nlohmann::json::exception &e)
            {
                ELOG << "Warning: Type mismatch for key '" << key << "'. " << e.what()
                          << ". Returning default value.";
            }
        }
        return defaultValue;
    }

    void setJsonValue(const std::string &key, const std::vector<std::string> &value)
    {
        std::lock_guard<std::mutex> lock(config_mutex);
        nlohmann::json jsonArray = nlohmann::json::array();
        for (const auto &item : value)
        {
            jsonArray.push_back(item);
        }
        parser->setValue(config_root, key, jsonArray);
    }
};

// ConfigManager Public Methods Implementation
ConfigManager::ConfigManager() : m_impl(std::make_unique<Impl>())
{
}
ConfigManager::~ConfigManager()
{
    stopHotReloadMonitor();
}

bool ConfigManager::loadConfig(const std::string &config_file, bool auto_create)
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    m_impl->config_file_path = config_file;

    if (!std::filesystem::exists(config_file))
    {
        if (auto_create)
        {
            ILOG << "Config file not found, creating default: " << config_file;
            if (!m_impl->createDefaultConfig(config_file))
            {
                ELOG << "Error: Failed to create default config file.";
                return false;
            }
        }
        else
        {
            ELOG << "Error: Config file not found and auto_create is false: " << config_file;
            return false;
        }
    }

    if (m_impl->parser->loadFromFile(config_file, m_impl->config_root))
    {
        m_impl->last_modified_time = m_impl->getFileModifiedTime(config_file);
        fireEvent(EventType::CONFIG_LOADED);
        return true;
    }
    return false;
}

bool ConfigManager::saveConfig(const std::string &config_file)
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    std::string target_file = config_file.empty() ? m_impl->config_file_path : config_file;
    if (target_file.empty())
    {
        ELOG << "Error: No config file path specified for saving.";
        return false;
    }

    if (m_impl->parser->saveToFile(target_file, m_impl->config_root))
    {
        m_impl->last_modified_time = m_impl->getFileModifiedTime(target_file);
        fireEvent(EventType::CONFIG_SAVED);
        return true;
    }
    return false;
}

void ConfigManager::enableHotReload(bool enabled, int check_interval_ms)
{
    if (enabled)
    {
        m_impl->check_interval_ms = check_interval_ms;
        if (!m_impl->hot_reload_enabled)
        {
            m_impl->hot_reload_enabled = true;
            startHotReloadMonitor();
        }
    }
    else
    {
        if (m_impl->hot_reload_enabled)
        {
            m_impl->hot_reload_enabled = false;
            stopHotReloadMonitor();
        }
    }
}

bool ConfigManager::hasKey(const std::string &key) const
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    return m_impl->parser->hasKey(m_impl->config_root, key);
}

template <typename T> T ConfigManager::getValue(const std::string &key, const T &default_value) const
{
    return m_impl->getJsonValue(key, default_value);
}

template <typename T> void ConfigManager::setValue(const std::string &key, const T &value, bool notify)
{
    m_impl->setJsonValue(key, value);
    if (notify)
    {
        fireEvent(EventType::CONFIG_CHANGED, key);
    }
}

std::string ConfigManager::getJsonString(bool pretty) const
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    return m_impl->parser->toString(m_impl->config_root, pretty);
}

bool ConfigManager::loadFromJsonString(const std::string &json_str, bool merge)
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    nlohmann::json new_config;
    if (!m_impl->parser->loadFromString(json_str, new_config))
    {
        return false;
    }

    if (merge)
    {
        m_impl->config_root.merge_patch(new_config); // nlohmann::json merge_patch for merging
    }
    else
    {
        m_impl->config_root = new_config;
    }
    fireEvent(EventType::CONFIG_LOADED); // Treat loading from string as a load event
    return true;
}

int ConfigManager::registerEventListener(EventListener listener)
{
    std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
    int id = m_impl->next_listener_id++;
    m_impl->event_listeners[id] = listener;
    return id;
}

bool ConfigManager::unregisterEventListener(int listener_id)
{
    std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
    return m_impl->event_listeners.erase(listener_id) > 0;
}

std::vector<std::string> ConfigManager::getAllKeys(const std::string &prefix) const
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    return m_impl->parser->getAllKeys(m_impl->config_root, prefix);
}

void ConfigManager::clear()
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    m_impl->config_root = nlohmann::json::object();
    fireEvent(EventType::CONFIG_CHANGED, ""); // Notify all config cleared
}

std::string ConfigManager::getConfigFilePath() const
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    return m_impl->config_file_path;
}

bool ConfigManager::isConfigFileModified() const
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    if (m_impl->config_file_path.empty())
    {
        return false;
    }
    auto current_modified_time = m_impl->getFileModifiedTime(m_impl->config_file_path);
    return current_modified_time > m_impl->last_modified_time;
}

std::chrono::system_clock::time_point ConfigManager::getLastModifiedTime() const
{
    std::lock_guard<std::mutex> lock(m_impl->config_mutex);
    return m_impl->last_modified_time;
}

void ConfigManager::fireEvent(EventType type, const std::string &key)
{
    ConfigEvent event;
    event.type = type;
    event.key = key;
    event.timestamp = std::chrono::system_clock::now();

    std::lock_guard<std::mutex> lock(m_impl->listeners_mutex);
    for (const auto &pair : m_impl->event_listeners)
    {
        pair.second(event);
    }
}

void ConfigManager::startHotReloadMonitor()
{
    if (m_impl->hot_reload_running)
    {
        return;
    }
    m_impl->hot_reload_running = true;
    m_impl->hot_reload_thread = std::thread(&ConfigManager::hotReloadMonitorThread, this);
}

void ConfigManager::stopHotReloadMonitor()
{
    if (!m_impl->hot_reload_running)
    {
        return;
    }
    m_impl->hot_reload_running = false;
    if (m_impl->hot_reload_thread.joinable())
    {
        m_impl->hot_reload_thread.join();
    }
}

void ConfigManager::hotReloadMonitorThread()
{
    while (m_impl->hot_reload_running)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(m_impl->check_interval_ms));
        if (m_impl->hot_reload_enabled && !m_impl->config_file_path.empty())
        {
            if (isConfigFileModified())
            {
                ILOG << "Config file modified, reloading: " << m_impl->config_file_path;
                // Acquire lock before reloading to prevent race conditions
                std::lock_guard<std::mutex> lock(m_impl->config_mutex);
                if (m_impl->parser->loadFromFile(m_impl->config_file_path, m_impl->config_root))
                {
                    m_impl->last_modified_time = m_impl->getFileModifiedTime(m_impl->config_file_path);
                    fireEvent(EventType::CONFIG_RELOADED);
                }
                else
                {
                    ELOG << "Error reloading config file: " << m_impl->config_file_path;
                }
            }
        }
    }
}

// Explicit template instantiations for common types
template <>
std::string ConfigManager::getValue<std::string>(const std::string &key, const std::string &default_value) const
{
    return m_impl->getJsonValue(key, default_value);
}

template <> int ConfigManager::getValue<int>(const std::string &key, const int &default_value) const
{
    return m_impl->getJsonValue(key, default_value);
}

template <> double ConfigManager::getValue<double>(const std::string &key, const double &default_value) const
{
    return m_impl->getJsonValue(key, default_value);
}

template <> bool ConfigManager::getValue<bool>(const std::string &key, const bool &default_value) const
{
    return m_impl->getJsonValue(key, default_value);
}

template <>
std::vector<std::string> ConfigManager::getValue<std::vector<std::string>>(
    const std::string &key, const std::vector<std::string> &default_value) const
{
    return m_impl->getJsonValue(key, default_value);
}

template <> void ConfigManager::setValue<std::string>(const std::string &key, const std::string &value, bool notify)
{
    m_impl->setJsonValue(key, value);
    if (notify)
        fireEvent(EventType::CONFIG_CHANGED, key);
}

template <> void ConfigManager::setValue<int>(const std::string &key, const int &value, bool notify)
{
    m_impl->setJsonValue(key, value);
    if (notify)
        fireEvent(EventType::CONFIG_CHANGED, key);
}

template <> void ConfigManager::setValue<double>(const std::string &key, const double &value, bool notify)
{
    m_impl->setJsonValue(key, value);
    if (notify)
        fireEvent(EventType::CONFIG_CHANGED, key);
}

template <> void ConfigManager::setValue<bool>(const std::string &key, const bool &value, bool notify)
{
    m_impl->setJsonValue(key, value);
    if (notify)
        fireEvent(EventType::CONFIG_CHANGED, key);
}

template <>
void ConfigManager::setValue<std::vector<std::string>>(const std::string &key, const std::vector<std::string> &value,
                                                       bool notify)
{
    m_impl->setJsonValue(key, value);
    if (notify)
        fireEvent(EventType::CONFIG_CHANGED, key);
}