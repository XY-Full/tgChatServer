#ifndef I_CONFIG_PARSER_H
#define I_CONFIG_PARSER_H

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

class IConfigParser
{
public:
    virtual ~IConfigParser() = default;

    // 从文件加载配置
    virtual bool loadFromFile(const std::string &filePath, nlohmann::json &outConfig) = 0;

    // 保存配置到文件
    virtual bool saveToFile(const std::string &filePath, const nlohmann::json &config) = 0;

    // 从字符串加载配置
    virtual bool loadFromString(const std::string &jsonString, nlohmann::json &outConfig) = 0;

    // 将配置序列化为字符串
    virtual std::string toString(const nlohmann::json &config, bool pretty = true) = 0;

    // 获取指定路径的JSON值
    virtual std::optional<nlohmann::json> getValue(const nlohmann::json &config, const std::string &keyPath) const = 0;

    // 设置指定路径的JSON值
    virtual bool setValue(nlohmann::json &config, const std::string &keyPath, const nlohmann::json &value) = 0;

    // 检查键是否存在
    virtual bool hasKey(const nlohmann::json &config, const std::string &keyPath) const = 0;

    // 收集所有键
    virtual std::vector<std::string> getAllKeys(const nlohmann::json &config, const std::string &prefix = "") const = 0;
};

#endif // I_CONFIG_PARSER_H