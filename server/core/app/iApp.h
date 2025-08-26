#ifndef IAPP_H
#define IAPP_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// 前向声明
class ConfigManager;
class SignalHandler;
class CommandLineParser;
class TerminalInterface;

/**
 * @brief iApp框架的核心基类
 *
 * 提供进程模型的基础框架，支持快速创建服务
 * 用户只需继承此类并实现相应的回调函数即可开发独立进程
 */
class iApp
{
public:
    /**
     * @brief 构造函数
     * @param app_name 应用程序名称
     */
    explicit iApp(const std::string &app_name = "iApp");

    /**
     * @brief 虚析构函数
     */
    virtual ~iApp();

    /**
     * @brief 启动应用程序
     * @param argc 命令行参数个数
     * @param argv 命令行参数数组
     * @return 返回码，0表示成功
     */
    int run(int argc, char *argv[]);

    /**
     * @brief 停止应用程序
     */
    void stop();

    /**
     * @brief 获取配置上下文
     * @return 配置管理器的引用
     */
    ConfigManager &getContext();

protected:
    // ========== 用户需要实现的回调函数 ==========

    /**
     * @brief 初始化回调函数
     * 在应用程序启动时调用，用于初始化资源
     * @return true表示初始化成功，false表示失败
     */
    virtual bool onInit() = 0;

    /**
     * @brief 定时运行回调函数
     * 按配置的时间间隔定期调用（默认10毫秒）
     * @param delta_ms 距离上次调用的时间间隔（毫秒）
     */
    virtual void onTick(uint32_t delta_ms) = 0;

    /**
     * @brief 结束清理回调函数
     * 在应用程序退出前调用，用于清理资源
     */
    virtual void onCleanup() = 0;

    /**
     * @brief 重载回调函数
     * 收到重载信号时调用，用于重新加载配置等
     * @return true表示重载成功，false表示失败
     */
    virtual bool onReload() = 0;

    /**
     * @brief 消息处理主循环回调函数
     * 在主循环中处理各种消息和事件
     * @return true表示继续运行，false表示退出
     */
    virtual bool onMessageLoop() = 0;

    // ========== 框架提供的辅助函数 ==========

    /**
     * @brief 注册命令行参数
     * @param name 参数名称
     * @param description 参数描述
     * @param default_value 默认值
     * @param callback 值变化时的回调函数（可选）
     */
    void registerCommandLineArg(const std::string &name, const std::string &description,
                                const std::string &default_value,
                                std::function<void(const std::string &)> callback = nullptr);

    /**
     * @brief 获取命令行参数值
     * @param name 参数名称
     * @return 参数值，如果不存在返回空字符串
     */
    std::string getCommandLineArg(const std::string &name) const;

    /**
     * @brief 注册终端命令
     * @param command 命令名称
     * @param description 命令描述
     * @param callback 命令执行回调函数
     */
    void registerTerminalCommand(const std::string &command, const std::string &description,
                                 std::function<std::string(const std::vector<std::string> &)> callback);

    /**
     * @brief 设置定时器间隔
     * @param interval_ms 间隔时间（毫秒）
     */
    void setTickInterval(uint32_t interval_ms);

    /**
     * @brief 获取当前定时器间隔
     * @return 间隔时间（毫秒）
     */
    uint32_t getTickInterval() const;

private:
    // ========== 内部实现 ==========

    /**
     * @brief 初始化框架
     * @param argc 命令行参数个数
     * @param argv 命令行参数数组
     * @return true表示成功，false表示失败
     */
    bool initialize(int argc, char *argv[]);

    /**
     * @brief 主循环
     */
    void mainLoop();

    /**
     * @brief 定时器线程函数
     */
    void tickThread();

    /**
     * @brief 终端接口线程函数
     */
    void terminalThread();

    /**
     * @brief 处理信号
     * @param signal 信号编号
     */
    void handleSignal(int signal);

    /**
     * @brief 加载配置文件
     * @return true表示成功，false表示失败
     */
    bool loadConfig();

    /**
     * @brief 保存配置文件
     * @return true表示成功，false表示失败
     */
    bool saveConfig();

private:
    // ========== 成员变量 ==========

    std::string m_app_name;            ///< 应用程序名称
    std::atomic<bool> m_running;       ///< 运行状态标志
    std::atomic<bool> m_should_reload; ///< 重载标志

    uint32_t m_tick_interval_ms;                            ///< 定时器间隔（毫秒）
    std::chrono::steady_clock::time_point m_last_tick_time; ///< 上次tick时间

    // 核心组件
    std::unique_ptr<ConfigManager> m_config_manager; ///< 配置管理器
    std::unique_ptr<SignalHandler> m_signal_handler; ///< 信号处理器
    std::unique_ptr<CommandLineParser> m_cmd_parser; ///< 命令行解析器
    std::unique_ptr<TerminalInterface> m_terminal;   ///< 终端接口

    // 线程相关
    std::thread m_tick_thread;     ///< 定时器线程
    std::thread m_terminal_thread; ///< 终端接口线程
    std::mutex m_mutex;            ///< 互斥锁
    std::condition_variable m_cv;  ///< 条件变量

    // 用户注册的回调函数
    std::map<std::string, std::function<void(const std::string &)>> m_arg_callbacks;
    std::map<std::string, std::function<std::string(const std::vector<std::string> &)>> m_terminal_commands;
};

// ========== 宏定义 ==========

/**
 * @brief 定义应用程序入口点的宏
 * @param AppClass 继承自iApp的应用程序类
 */
#define IAPP_MAIN(AppClass)                                                                                            \
    int main(int argc, char *argv[])                                                                                   \
    {                                                                                                                  \
        AppClass app;                                                                                                  \
        return app.run(argc, argv);                                                                                    \
    }

#endif // IAPP_H
