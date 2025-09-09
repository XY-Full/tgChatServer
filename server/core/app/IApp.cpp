#include "IApp.h"
#include "CommandLineParser.h"
#include "GlobalSpace.h"
#include "SignalHandler.h"
#include "TerminalInterface.h"
#include "Timer.h"
#include "jsonParser/ConfigManager.h"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

// ========== IApp Implementation ==========

IApp::IApp(const std::string &app_name)
    : m_app_name(app_name), m_running(false), m_should_reload(false), m_tick_interval_ms(10)
{
    m_config_manager = std::make_unique<ConfigManager>();
    m_signal_handler = std::make_unique<SignalHandler>();
    m_cmd_parser = std::make_unique<CommandLineParser>();
    m_terminal = std::make_unique<TerminalInterface>();

    m_last_tick_time = std::chrono::steady_clock::now();
}

IApp::~IApp()
{
    stop();
}

int IApp::run(int argc, char *argv[])
{
    if (!initialize(argc, argv))
    {
        std::cerr << "Failed to initialize application" << std::endl;
        return 1;
    }

    if (m_cmd_parser->isTerminalMode())
    {
        // Terminal mode
        bool enable_telnet = m_config_manager->getValue<bool>("terminal.enable_telnet", false);
        // 命令行参数优先级高于配置文件
        std::string telnet_enabled_str = m_cmd_parser->getValue("enable-telnet");
        if (!telnet_enabled_str.empty())
        {
            enable_telnet = (telnet_enabled_str == "true" || telnet_enabled_str == "1");
        }

        uint16_t telnet_port = m_config_manager->getValue<int>("terminal.telnet_port", 23);
        std::string telnet_port_str = m_cmd_parser->getValue("telnet-port");
        if (!telnet_port_str.empty())
        {
            telnet_port = std::stoi(telnet_port_str);
        }

        m_terminal->start(enable_telnet, telnet_port);

        while (m_running)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    else
    {
        // Normal mode
        mainLoop();
    }

    onCleanup();
    return 0;
}

void IApp::stop()
{
    m_running = false;
    m_cv.notify_all();

    if (m_tick_thread.joinable())
    {
        m_tick_thread.join();
    }

    if (m_terminal_thread.joinable())
    {
        m_terminal_thread.join();
    }

    m_terminal->stop();

    delete GlobalSpace()->timer_;
}

ConfigManager &IApp::getContext()
{
    return *m_config_manager;
}

bool IApp::initialize(int argc, char *argv[])
{
    // 解析命令行参数
    if (!m_cmd_parser->parse(argc, argv))
    {
        return false;
    }

    // 是否显示help信息
    if (m_cmd_parser->isHelpMode())
    {
        m_cmd_parser->showHelp();
        return false;
    }

    // 加载配置文件
    std::string config_file = m_cmd_parser->getValue("config");
    if (!m_config_manager->loadConfig(config_file, true))
    {
        std::cerr << "Failed to load config file: " << config_file << std::endl;
        return false;
    }

    // 设置tick间隔
    std::string tick_interval_str = m_cmd_parser->getValue("tick-interval");
    if (!tick_interval_str.empty())
    {
        try
        {
            m_tick_interval_ms = std::stoi(tick_interval_str);
        }
        catch (const std::exception &e)
        {
            std::cerr << "Invalid tick interval: " << tick_interval_str << std::endl;
            m_tick_interval_ms = m_config_manager->getValue<int>("app.tick_interval_ms", 10);
        }
    }
    else
    {
        m_tick_interval_ms = m_config_manager->getValue<int>("app.tick_interval_ms", 10);
    }

    // 初始化信号处理器
    m_signal_handler->initialize();
    m_signal_handler->registerHandler(SIGTERM, [this](int signal) {
        std::cout << "Received SIGTERM, shutting down..." << std::endl;
        m_running = false;
        m_cv.notify_all();
    });
    m_signal_handler->registerHandler(SIGINT, [this](int signal) {
        std::cout << "Received SIGINT, shutting down..." << std::endl;
        m_running = false;
        m_cv.notify_all();
    });
    m_signal_handler->registerHandler(SIGHUP, [this](int signal) {
        std::cout << "Received SIGHUP, reloading..." << std::endl;
        m_should_reload = true;
        m_cv.notify_all();
    });

    // 注册内置终端命令
    registerTerminalCommand(
        "config", "Show or reload configuration", [this](const std::vector<std::string> &args) -> std::string {
            if (args.empty())
            {
                return "Current configuration:\n" + m_config_manager->getJsonString(true);
            }
            else if (args[0] == "reload")
            {
                if (onReload())
                {
                    return "Configuration reloaded successfully.\n";
                }
                else
                {
                    return "Failed to reload configuration.\n";
                }
            }
            else if (args[0] == "save")
            {
                if (saveConfig())
                {
                    return "Configuration saved successfully.\n";
                }
                else
                {
                    return "Failed to save configuration.\n";
                }
            }
            else if (args.size() >= 3 && args[0] == "set")
            {
                m_config_manager->setValue(args[1], args[2]);
                return "Configuration updated: " + args[1] + " = " + args[2] + "\n";
            }
            else if (args.size() >= 2 && args[0] == "get")
            {
                return args[1] + " = " + m_config_manager->getValue<std::string>(args[1], "not found") + "\n";
            }
            return "Usage: config [reload|save|set <key> <value>|get <key>]\n";
        });

    registerTerminalCommand("status", "Show application status",
                            [this](const std::vector<std::string> &) -> std::string {
                                return std::string("Application status:\n") + "Name: " + m_app_name + "\n" +
                                       "Running: " + (m_running ? "yes" : "no") + "\n" +
                                       "Tick interval: " + std::to_string(m_tick_interval_ms) + " ms\n" +
                                       "Config file: " + m_config_manager->getConfigFilePath() + "\n";
                            });

    registerTerminalCommand("stop", "Stop the application", [this](const std::vector<std::string> &) -> std::string {
        m_running = false;
        m_cv.notify_all();
        return "Stopping application...\n";
    });

    registerTerminalCommand("tick", "Get or set tick interval",
                            [this](const std::vector<std::string> &args) -> std::string {
                                if (args.empty())
                                {
                                    return "Current tick interval: " + std::to_string(m_tick_interval_ms) + " ms\n";
                                }
                                else
                                {
                                    try
                                    {
                                        int new_interval = std::stoi(args[0]);
                                        if (new_interval <= 0)
                                        {
                                            return "Invalid tick interval: must be positive\n";
                                        }
                                        setTickInterval(new_interval);
                                        return "Tick interval set to " + std::to_string(m_tick_interval_ms) + " ms\n";
                                    }
                                    catch (const std::exception &e)
                                    {
                                        return "Invalid tick interval: " + args[0] + "\n";
                                    }
                                }
                            });

    // 启用配置热加载
    m_config_manager->enableHotReload(true);
    m_config_manager->registerEventListener([this](const ConfigManager::ConfigEvent &event) {
        if (event.type == ConfigManager::EventType::CONFIG_RELOADED)
        {
            std::cout << "Configuration file changed, triggering reload..." << std::endl;
            m_should_reload = true;
            m_cv.notify_all();
        }
    });

    // 调用用户初始化
    m_running = true;
    if (!onInit())
    {
        std::cerr << "Application initialization failed" << std::endl;
        return false;
    }

    // 启动线程
    m_tick_thread = std::thread(&IApp::tickThread, this);

    // 设置全局变量
    GlobalSpace()->bus_ = m_bus_client.get();
    GlobalSpace()->configMgr_ = m_config_manager.get();
    GlobalSpace()->timer_ = new Timer();

    return true;
}

void IApp::mainLoop()
{
    std::thread signal_thread([this]() {
        while (m_running)
        {
            int signal = m_signal_handler->waitForSignal();
            if (signal > 0)
            {
                handleSignal(signal);
            }
        }
    });

    while (m_running)
    {
        if (unlikely(m_should_reload))
        {
            m_should_reload = false;
            if (!onReload())
            {
                std::cerr << "Failed to reload application" << std::endl;
            }
        }

        if (!onMessageLoop())
        {
            break;
        }

        // 等待条件变量或超时
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait_for(lock, std::chrono::milliseconds(100), [this]() { return !m_running || m_should_reload; });
    }

    if (signal_thread.joinable())
    {
        signal_thread.join();
    }
}

void IApp::tickThread()
{
    while (m_running)
    {
        auto now = std::chrono::steady_clock::now();
        uint32_t delta_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_last_tick_time).count();
        m_last_tick_time = now;

        onTick(delta_ms);

        std::this_thread::sleep_for(std::chrono::milliseconds(m_tick_interval_ms));
    }
}

void IApp::terminalThread()
{
    bool enable_telnet = m_config_manager->getValue<bool>("terminal.enable_telnet", false);
    uint16_t telnet_port = m_config_manager->getValue<int>("terminal.telnet_port", 23);
    m_terminal->start(enable_telnet, telnet_port);
}

void IApp::handleSignal(int signal)
{
    switch (signal)
    {
    case SIGTERM:
    case SIGINT:
        m_running = false;
        m_cv.notify_all();
        break;
    case SIGHUP:
        m_should_reload = true;
        m_cv.notify_all();
        break;
    }
}

bool IApp::loadConfig()
{
    std::string config_file = m_cmd_parser->getValue("config");
    return m_config_manager->loadConfig(config_file);
}

bool IApp::saveConfig()
{
    std::string config_file = m_cmd_parser->getValue("config");
    return m_config_manager->saveConfig(config_file);
}

void IApp::registerCommandLineArg(const std::string &name, const std::string &description,
                                  const std::string &default_value, std::function<void(const std::string &)> callback)
{
    m_cmd_parser->registerArg(name, description, default_value);
    if (callback)
    {
        m_arg_callbacks[name] = callback;

        // 如果已经解析过命令行参数，立即调用回调函数
        std::string value = m_cmd_parser->getValue(name);
        if (!value.empty())
        {
            callback(value);
        }
    }
}

std::string IApp::getCommandLineArg(const std::string &name) const
{
    return m_cmd_parser->getValue(name);
}

void IApp::registerTerminalCommand(const std::string &command, const std::string &description,
                                   std::function<std::string(const std::vector<std::string> &)> callback)
{
    m_terminal->registerCommand(command, description, callback);
    m_terminal_commands[command] = callback;
}

void IApp::setTickInterval(uint32_t interval_ms)
{
    m_tick_interval_ms = interval_ms;
}

uint32_t IApp::getTickInterval() const
{
    return m_tick_interval_ms;
}
