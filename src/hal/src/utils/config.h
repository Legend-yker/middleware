#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace utils {

class Config {
public:
    Config() = default;

    bool loadFromJson(const std::string &path, std::string &err);
    bool loadFromString(const std::string &jsonStr, std::string &err);

    bool hasKey(const std::string &key) const;

    const nlohmann::json &root() const { return root_; }
    const std::string &filePath() const { return file_path_; }

    int getInt(const std::string &key, int defaultVal = 0) const;
    std::string getString(const std::string &key, const std::string &defaultVal = "") const;
    bool getBool(const std::string &key, bool defaultVal = false) const;

    nlohmann::json getArray(const std::string &key) const;
    nlohmann::json getObject(const std::string &key) const;

    bool validate(const std::vector<std::string> &requiredKeys, std::string &err) const;

private:
    nlohmann::json root_;
    std::string file_path_;
};

} // namespace utils
