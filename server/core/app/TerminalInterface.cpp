#include "TerminalInterface.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

TerminalInterface::TerminalInterface()
    : m_running(false), m_telnet_enabled(false), m_telnet_port(23), m_prompt("iApp> "), m_telnet_socket(-1)
{
    registerBuiltinCommands();
}

TerminalInterface::~TerminalInterface()
{
    stop();
}

bool TerminalInterface::start(bool enable_telnet, uint16_t telnet_port)
{
    if (m_running.load())
    {
        return false;
    }

    m_running.store(true);
    m_telnet_enabled.store(enable_telnet);
    m_telnet_port = telnet_port;

    // 启动标准输入输出线程
    m_stdio_thread = std::thread(&TerminalInterface::stdioThread, this);

    // 如果启用telnet，启动telnet服务器线程
    if (enable_telnet)
    {
        m_telnet_server_thread = std::thread(&TerminalInterface::telnetServerThread, this);
    }

    return true;
}

void TerminalInterface::stop()
{
    if (!m_running.load())
    {
        return;
    }

    m_running.store(false);

    // 关闭telnet socket
    if (m_telnet_socket != -1)
    {
        close(m_telnet_socket);
        m_telnet_socket = -1;
    }

    // 等待线程结束
    if (m_stdio_thread.joinable())
    {
        m_stdio_thread.join();
    }

    if (m_telnet_server_thread.joinable())
    {
        m_telnet_server_thread.join();
    }

    // 等待所有客户端线程结束
    std::lock_guard<std::mutex> lock(m_clients_mutex);
    for (auto &thread : m_client_threads)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
    m_client_threads.clear();
}

void TerminalInterface::registerCommand(const std::string &command, const std::string &description,
                                        CommandHandler handler)
{
    std::lock_guard<std::mutex> lock(m_commands_mutex);

    CommandInfo info;
    info.name = command;
    info.description = description;
    info.handler = handler;

    m_commands[command] = info;
}

std::string TerminalInterface::executeCommand(const std::string &command_line)
{
    return processCommand(command_line);
}

bool TerminalInterface::isRunning() const
{
    return m_running.load();
}

void TerminalInterface::setPrompt(const std::string &prompt)
{
    m_prompt = prompt;
}

std::string TerminalInterface::getPrompt() const
{
    return m_prompt;
}

void TerminalInterface::stdioThread()
{
    std::string line;

    while (m_running.load())
    {
        std::cout << m_prompt;
        std::cout.flush();

        if (!std::getline(std::cin, line))
        {
            break;
        }

        if (!line.empty())
        {
            std::string result = processCommand(line);
            if (!result.empty())
            {
                std::cout << result << std::endl;
            }
        }
    }
}

void TerminalInterface::telnetServerThread()
{
    // 创建socket
    m_telnet_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (m_telnet_socket == -1)
    {
        std::cerr << "Failed to create telnet socket" << std::endl;
        return;
    }

    // 设置socket选项
    int opt = 1;
    setsockopt(m_telnet_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // 绑定地址
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(m_telnet_port);

    if (bind(m_telnet_socket, (struct sockaddr *)&addr, sizeof(addr)) == -1)
    {
        std::cerr << "Failed to bind telnet socket to port " << m_telnet_port << std::endl;
        close(m_telnet_socket);
        m_telnet_socket = -1;
        return;
    }

    // 监听连接
    if (listen(m_telnet_socket, 5) == -1)
    {
        std::cerr << "Failed to listen on telnet socket" << std::endl;
        close(m_telnet_socket);
        m_telnet_socket = -1;
        return;
    }

    std::cout << "Telnet server listening on port " << m_telnet_port << std::endl;

    while (m_running.load())
    {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(m_telnet_socket, (struct sockaddr *)&client_addr, &client_len);
        if (client_socket == -1)
        {
            if (m_running.load())
            {
                std::cerr << "Failed to accept telnet connection" << std::endl;
            }
            continue;
        }

        // 为每个客户端创建处理线程
        std::lock_guard<std::mutex> lock(m_clients_mutex);
        m_client_threads.emplace_back(&TerminalInterface::telnetClientThread, this, client_socket);
    }
}

void TerminalInterface::telnetClientThread(int client_socket)
{
    char buffer[1024];
    std::string line;

    // 发送欢迎消息
    std::string welcome = "Welcome to iApp Terminal\r\n" + m_prompt;
    send(client_socket, welcome.c_str(), welcome.length(), 0);

    while (m_running.load())
    {
        int bytes_received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_received <= 0)
        {
            break;
        }

        buffer[bytes_received] = '\0';

        for (int i = 0; i < bytes_received; ++i)
        {
            char c = buffer[i];

            if (c == '\r' || c == '\n')
            {
                if (!line.empty())
                {
                    std::string result = processCommand(line);
                    if (!result.empty())
                    {
                        result += "\r\n";
                        send(client_socket, result.c_str(), result.length(), 0);
                    }
                    line.clear();
                }

                std::string prompt = m_prompt;
                send(client_socket, prompt.c_str(), prompt.length(), 0);
            }
            else if (c == '\b' || c == 127)
            { // 退格键
                if (!line.empty())
                {
                    line.pop_back();
                    send(client_socket, "\b \b", 3, 0);
                }
            }
            else if (c >= 32 && c <= 126)
            { // 可打印字符
                line += c;
                send(client_socket, &c, 1, 0);
            }
        }
    }

    close(client_socket);
}

std::vector<std::string> TerminalInterface::parseCommandLine(const std::string &command_line)
{
    std::vector<std::string> tokens;
    std::istringstream iss(command_line);
    std::string token;

    while (iss >> token)
    {
        tokens.push_back(token);
    }

    return tokens;
}

std::string TerminalInterface::processCommand(const std::string &command_line)
{
    std::vector<std::string> args = parseCommandLine(command_line);
    if (args.empty())
    {
        return "";
    }

    std::string command = args[0];
    args.erase(args.begin());

    std::lock_guard<std::mutex> lock(m_commands_mutex);
    auto it = m_commands.find(command);
    if (it != m_commands.end())
    {
        try
        {
            return it->second.handler(args);
        }
        catch (const std::exception &e)
        {
            return "Error executing command: " + std::string(e.what());
        }
    }

    return "Unknown command: " + command + ". Type 'help' for available commands.";
}

void TerminalInterface::registerBuiltinCommands()
{
    registerCommand("help", "显示帮助信息", [this](const std::vector<std::string> &args) { return helpCommand(args); });

    registerCommand("exit", "退出终端", [this](const std::vector<std::string> &args) { return exitCommand(args); });

    registerCommand("list", "列出所有可用命令",
                    [this](const std::vector<std::string> &args) { return listCommand(args); });
}

std::string TerminalInterface::helpCommand(const std::vector<std::string> &args)
{
    std::ostringstream oss;
    oss << "Available commands:\n";

    std::lock_guard<std::mutex> lock(m_commands_mutex);
    for (const auto &pair : m_commands)
    {
        oss << "  " << pair.first << " - " << pair.second.description << "\n";
    }

    return oss.str();
}

std::string TerminalInterface::exitCommand(const std::vector<std::string> &args)
{
    m_running.store(false);
    return "Goodbye!";
}

std::string TerminalInterface::listCommand(const std::vector<std::string> &args)
{
    std::ostringstream oss;
    oss << "Registered commands:\n";

    std::lock_guard<std::mutex> lock(m_commands_mutex);
    for (const auto &pair : m_commands)
    {
        oss << "  " << pair.first << "\n";
    }

    return oss.str();
}