#ifndef NLOHMANN_JSON_PARSER_H
#define NLOHMANN_JSON_PARSER_H

#include "IConfigParser.h"
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <stdexcept>

class JsonParser : public IConfigParser
{
public:
    bool loadFromFile(const std::string &filePath, nlohmann::json &outConfig) override;
    bool saveToFile(const std::string &filePath, const nlohmann::json &config) override;
    bool loadFromString(const std::string &jsonString, nlohmann::json &outConfig) override;
    std::string toString(const nlohmann::json &config, bool pretty = true) override;
    std::optional<nlohmann::json> getValue(const nlohmann::json &config, const std::string &keyPath) const override;
    bool setValue(nlohmann::json &config, const std::string &keyPath, const nlohmann::json &value) override;
    bool hasKey(const nlohmann::json &config, const std::string &keyPath) const override;
    std::vector<std::string> getAllKeys(const nlohmann::json &config, const std::string &prefix = "") const override;

private:
    // 辅助函数：解析键路径
    std::vector<std::string> parseKeyPath(const std::string &keyPath) const;
    // 辅助函数：获取可变JSON引用
    nlohmann::json *getMutableJsonRef(nlohmann::json &config, const std::string &keyPath, bool createIfNotExist);
    // 辅助函数：获取常量JSON引用
    const nlohmann::json *getConstJsonRef(const nlohmann::json &config, const std::string &keyPath) const;
    // 辅助函数：递归收集键
    void collectKeysRecursive(const nlohmann::json &node, const std::string &currentPath,
                              std::vector<std::string> &keys) const;
};

#endif // NLOHMANN_JSON_PARSER_H