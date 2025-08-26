#include "CommandLineParser.h"
#include <algorithm>
#include <iostream>
#include <sstream>

CommandLineParser::CommandLineParser() : m_terminal_mode(false), m_help_mode(false)
{
    // 注册内置参数
    registerArg("h,help", "显示帮助信息", "false");
    registerArg("t,terminal", "启动终端模式", "false");
    registerArg("tick-interval", "设置定时器间隔(毫秒)", "10");
    registerArg("config", "配置文件路径", "config.json");
    registerArg("enable-telnet", "启用telnet终端", "false");
    registerArg("telnet-port", "telnet端口号", "23");
}

CommandLineParser::~CommandLineParser() = default;

bool CommandLineParser::parse(int argc, char *argv[])
{
    if (argc < 1)
        return false;

    m_program_name = argv[0];

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg.empty())
            continue;

        if (arg[0] == '-')
        {
            i = parseArg(arg, i, argc, argv);
        }
        else
        {
            m_positional_args.push_back(arg);
        }
    }

    // 检查特殊模式
    if (getValue("help") == "true" || getValue("h") == "true")
    {
        m_help_mode = true;
    }

    if (getValue("terminal") == "true" || getValue("t") == "true")
    {
        m_terminal_mode = true;
    }

    // 触发回调函数
    for (const auto &pair : m_values)
    {
        auto it = m_args.find(pair.first);
        if (it != m_args.end() && it->second.callback)
        {
            it->second.callback(pair.second);
        }
    }

    return true;
}

void CommandLineParser::registerArg(const std::string &name, const std::string &description,
                                    const std::string &default_value, std::function<void(const std::string &)> callback)
{
    ArgInfo info;
    info.name = name;
    info.description = description;
    info.default_value = default_value;
    info.value = default_value;
    info.callback = callback;

    // 解析短名称和长名称
    size_t comma_pos = name.find(',');
    if (comma_pos != std::string::npos)
    {
        std::string short_name = name.substr(0, comma_pos);
        std::string long_name = name.substr(comma_pos + 1);

        m_args[long_name] = info;
        m_name_map[short_name] = long_name;
        m_values[long_name] = default_value;
    }
    else
    {
        m_args[name] = info;
        m_values[name] = default_value;
    }
}

std::string CommandLineParser::getValue(const std::string &name) const
{
    std::string canonical_name = findCanonicalName(name);
    if (canonical_name.empty())
        return "";

    auto it = m_values.find(canonical_name);
    return (it != m_values.end()) ? it->second : "";
}

bool CommandLineParser::hasArg(const std::string &name) const
{
    std::string canonical_name = findCanonicalName(name);
    return !canonical_name.empty() && m_values.find(canonical_name) != m_values.end();
}

std::vector<std::string> CommandLineParser::getPositionalArgs() const
{
    return m_positional_args;
}

std::string CommandLineParser::getProgramName() const
{
    return m_program_name;
}

void CommandLineParser::showHelp() const
{
    std::cout << "Usage: " << m_program_name << " [OPTIONS]\n\n";
    std::cout << "Options:\n";

    for (const auto &pair : m_args)
    {
        const ArgInfo &info = pair.second;
        std::cout << "  ";

        // 查找短名称
        std::string short_name;
        for (const auto &name_pair : m_name_map)
        {
            if (name_pair.second == pair.first)
            {
                short_name = name_pair.first;
                break;
            }
        }

        if (!short_name.empty())
        {
            std::cout << "-" << short_name << ", ";
        }

        std::cout << "--" << pair.first;
        std::cout << "\t" << info.description;
        std::cout << " (默认: " << info.default_value << ")\n";
    }
}

bool CommandLineParser::isTerminalMode() const
{
    return m_terminal_mode;
}

bool CommandLineParser::isHelpMode() const
{
    return m_help_mode;
}

const std::map<std::string, CommandLineParser::ArgInfo> &CommandLineParser::getAllArgs() const
{
    return m_args;
}

int CommandLineParser::parseArg(const std::string &arg, int index, int argc, char *argv[])
{
    std::string name;
    std::string value;
    bool has_value = false;

    if (arg.substr(0, 2) == "--")
    {
        // 长格式参数
        name = arg.substr(2);
        size_t eq_pos = name.find('=');
        if (eq_pos != std::string::npos)
        {
            value = name.substr(eq_pos + 1);
            name = name.substr(0, eq_pos);
            has_value = true;
        }
    }
    else if (arg[0] == '-')
    {
        // 短格式参数
        name = arg.substr(1);
        if (name.length() > 1)
        {
            // 可能是组合的短参数或带值的短参数
            value = name.substr(1);
            name = name.substr(0, 1);
            has_value = true;
        }
    }

    std::string canonical_name = findCanonicalName(name);
    if (canonical_name.empty())
    {
        std::cerr << "Unknown argument: " << arg << std::endl;
        return index;
    }

    if (!has_value)
    {
        // 检查下一个参数是否为值
        if (index + 1 < argc && argv[index + 1][0] != '-')
        {
            value = argv[index + 1];
            index++;
        }
        else
        {
            value = "true"; // 布尔标志
        }
    }

    m_values[canonical_name] = value;
    return index;
}

std::string CommandLineParser::findCanonicalName(const std::string &name) const
{
    // 首先检查是否为规范名称
    if (m_args.find(name) != m_args.end())
    {
        return name;
    }

    // 检查是否为短名称
    auto it = m_name_map.find(name);
    if (it != m_name_map.end())
    {
        return it->second;
    }

    return "";
}