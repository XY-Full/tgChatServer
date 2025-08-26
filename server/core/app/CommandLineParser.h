#ifndef COMMAND_LINE_PARSER_H
#define COMMAND_LINE_PARSER_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

/**
 * @brief 命令行参数解析器类
 *
 * 负责解析命令行参数，支持短格式和长格式参数
 * 支持注册参数及其默认值和说明
 * 支持注册参数回调函数，当参数值发生变化时触发
 * 支持命令行参数的帮助信息生成
 */
class CommandLineParser
{
public:
    /**
     * @brief 参数信息结构体
     */
    struct ArgInfo
    {
        std::string name;                                  ///< 参数名称
        std::string description;                           ///< 参数描述
        std::string default_value;                         ///< 默认值
        std::string value;                                 ///< 当前值
        std::function<void(const std::string &)> callback; ///< 回调函数
    };

public:
    CommandLineParser();
    ~CommandLineParser();

    bool parse(int argc, char *argv[]);
    void registerArg(const std::string &name, const std::string &description, const std::string &default_value,
                     std::function<void(const std::string &)> callback = nullptr);

    std::string getValue(const std::string &name) const;
    bool hasArg(const std::string &name) const;
    std::vector<std::string> getPositionalArgs() const;
    std::string getProgramName() const;
    void showHelp() const;
    bool isTerminalMode() const;
    bool isHelpMode() const;
    const std::map<std::string, ArgInfo> &getAllArgs() const;

private:
    int parseArg(const std::string &arg, int index, int argc, char *argv[]);
    std::string findCanonicalName(const std::string &name) const;

private:
    std::string m_program_name;
    std::map<std::string, ArgInfo> m_args;
    std::map<std::string, std::string> m_values;
    std::map<std::string, std::string> m_name_map;
    std::vector<std::string> m_positional_args;
    bool m_terminal_mode;
    bool m_help_mode;
};

#endif // COMMAND_LINE_PARSER_H