#include "JsonParser.h"
#include <filesystem> // For file system operations
#include <iostream>   // For logging errors

bool JsonParser::loadFromFile(const std::string &filePath, nlohmann::json &outConfig)
{
    std::ifstream file(filePath);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open config file: " << filePath << std::endl;
        return false;
    }
    try
    {
        file >> outConfig;
        return true;
    }
    catch (const nlohmann::json::parse_error &e)
    {
        std::cerr << "Error parsing JSON from file " << filePath << ": " << e.what() << std::endl;
        return false;
    }
}

bool JsonParser::saveToFile(const std::string &filePath, const nlohmann::json &config)
{
    std::ofstream file(filePath);
    if (!file.is_open())
    {
        std::cerr << "Error: Could not open file for writing: " << filePath << std::endl;
        return false;
    }
    try
    {
        file << config.dump(4); // Pretty print with 4 spaces indentation
        return true;
    }
    catch (const std::exception &e)
    {
        std::cerr << "Error writing JSON to file " << filePath << ": " << e.what() << std::endl;
        return false;
    }
}

bool JsonParser::loadFromString(const std::string &jsonString, nlohmann::json &outConfig)
{
    try
    {
        outConfig = nlohmann::json::parse(jsonString);
        return true;
    }
    catch (const nlohmann::json::parse_error &e)
    {
        std::cerr << "Error parsing JSON from string: " << e.what() << std::endl;
        return false;
    }
}

std::string JsonParser::toString(const nlohmann::json &config, bool pretty)
{
    if (pretty)
    {
        return config.dump(4);
    }
    else
    {
        return config.dump();
    }
}

std::optional<nlohmann::json> JsonParser::getValue(const nlohmann::json &config, const std::string &keyPath) const
{
    const nlohmann::json *node = getConstJsonRef(config, keyPath);
    if (node && !node->is_null())
    {
        return *node;
    }
    return std::nullopt;
}

bool JsonParser::setValue(nlohmann::json &config, const std::string &keyPath, const nlohmann::json &value)
{
    nlohmann::json *node = getMutableJsonRef(config, keyPath, true); // Create if not exist
    if (node)
    {
        *node = value;
        return true;
    }
    return false;
}

bool JsonParser::hasKey(const nlohmann::json &config, const std::string &keyPath) const
{
    const nlohmann::json *node = getConstJsonRef(config, keyPath);
    return node != nullptr && !node->is_null();
}

std::vector<std::string> JsonParser::getAllKeys(const nlohmann::json &config, const std::string &prefix) const
{
    std::vector<std::string> keys;
    if (config.is_object())
    {
        collectKeysRecursive(config, prefix, keys);
    }
    else if (config.is_array())
    {
        // For array, we might want to list elements as prefix[index]
        // This implementation focuses on object keys for simplicity,
        // but can be extended if array element keys are needed.
    }
    return keys;
}

std::vector<std::string> JsonParser::parseKeyPath(const std::string &keyPath) const
{
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream tokenStream(keyPath);
    while (std::getline(tokenStream, token, '.'))
    {
        tokens.push_back(token);
    }
    return tokens;
}

nlohmann::json *JsonParser::getMutableJsonRef(nlohmann::json &config, const std::string &keyPath, bool createIfNotExist)
{
    std::vector<std::string> tokens = parseKeyPath(keyPath);
    nlohmann::json *current = &config;

    for (size_t i = 0; i < tokens.size(); ++i)
    {
        const std::string &token = tokens[i];
        if (current->is_object())
        {
            if (current->contains(token))
            {
                current = &(*current)[token];
            }
            else if (createIfNotExist)
            {
                (*current)[token] = nlohmann::json::object(); // Create as object by default
                current = &(*current)[token];
            }
            else
            {
                return nullptr; // Key not found and not creating
            }
        }
        else if (current->is_array())
        {
            // Handle array indexing if needed, for now, treat as object path
            try
            {
                size_t index = std::stoul(token);
                if (index < current->size())
                {
                    current = &(*current)[index];
                }
                else if (createIfNotExist)
                {
                    // Extend array if index is out of bounds
                    while (current->size() <= index)
                    {
                        current->push_back(nlohmann::json::object()); // Fill with null or default object
                    }
                    current = &(*current)[index];
                }
                else
                {
                    return nullptr;
                }
            }
            catch (const std::exception &)
            {
                // Not a valid index, treat as object key if possible
                return nullptr;
            }
        }
        else if (current->is_null() && createIfNotExist)
        {
            // If current is null, and we need to create, assume it should be an object
            *current = nlohmann::json::object();
            (*current)[token] = nlohmann::json::object();
            current = &(*current)[token];
        }
        else
        {
            return nullptr; // Cannot traverse non-object/non-array
        }
    }
    return current;
}

const nlohmann::json *JsonParser::getConstJsonRef(const nlohmann::json &config, const std::string &keyPath) const
{
    std::vector<std::string> tokens = parseKeyPath(keyPath);
    const nlohmann::json *current = &config;

    for (const std::string &token : tokens)
    {
        if (current->is_object() && current->contains(token))
        {
            current = &(*current)[token];
        }
        else if (current->is_array())
        {
            try
            {
                size_t index = std::stoul(token);
                if (index < current->size())
                {
                    current = &(*current)[index];
                }
                else
                {
                    return nullptr; // Index out of bounds
                }
            }
            catch (const std::exception &)
            {
                return nullptr; // Not a valid index for array
            }
        }
        else
        {
            return nullptr; // Key not found or cannot traverse
        }
    }
    return current;
}

void JsonParser::collectKeysRecursive(const nlohmann::json &node, const std::string &currentPath,
                                      std::vector<std::string> &keys) const
{
    if (node.is_object())
    {
        for (nlohmann::json::const_iterator it = node.begin(); it != node.end(); ++it)
        {
            std::string newPath = currentPath.empty() ? it.key() : currentPath + "." + it.key();
            keys.push_back(newPath);
            collectKeysRecursive(it.value(), newPath, keys);
        }
    }
    else if (node.is_array())
    {
        for (size_t i = 0; i < node.size(); ++i)
        {
            std::string newPath = currentPath + "[" + std::to_string(i) + "]";
            // keys.push_back(newPath); // Decide if array elements themselves should be keys
            collectKeysRecursive(node[i], newPath, keys);
        }
    }
}