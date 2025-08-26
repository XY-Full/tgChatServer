#ifndef TERMINAL_INTERFACE_H
#define TERMINAL_INTERFACE_H

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief 交互式终端接口类
 *
 * 提供标准输入输出和telnet的命令行交互功能
 * 支持命令注册和执行
 * 支持多客户端连接（telnet模式）
 */
class TerminalInterface
{
public:
    /**
     * @brief 命令处理函数类型
     */
    using CommandHandler = std::function<std::string(const std::vector<std::string> &)>;

    /**
     * @brief 命令信息结构体
     */
    struct CommandInfo
    {
        std::string name;        ///< 命令名称
        std::string description; ///< 命令描述
        CommandHandler handler;  ///< 命令处理函数
    };

public:
    TerminalInterface();
    ~TerminalInterface();

    bool start(bool enable_telnet = false, uint16_t telnet_port = 23);
    void stop();

    void registerCommand(const std::string &command, const std::string &description, CommandHandler handler);

    std::string executeCommand(const std::string &command_line);

    bool isRunning() const;
    void setPrompt(const std::string &prompt);
    std::string getPrompt() const;

private:
    void stdioThread();
    void telnetServerThread();
    void telnetClientThread(int client_socket);

    std::vector<std::string> parseCommandLine(const std::string &command_line);
    std::string processCommand(const std::string &command_line);
    void registerBuiltinCommands();

    std::string helpCommand(const std::vector<std::string> &args);
    std::string exitCommand(const std::vector<std::string> &args);
    std::string listCommand(const std::vector<std::string> &args);

private:
    std::atomic<bool> m_running;
    std::atomic<bool> m_telnet_enabled;
    uint16_t m_telnet_port;
    std::string m_prompt;

    std::map<std::string, CommandInfo> m_commands;
    std::mutex m_commands_mutex;

    std::thread m_stdio_thread;
    std::thread m_telnet_server_thread;
    std::vector<std::thread> m_client_threads;

    int m_telnet_socket;
    std::mutex m_clients_mutex;
};

#endif // TERMINAL_INTERFACE_H