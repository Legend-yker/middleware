#include "utils/config.h"

#include <fstream>
#include <sstream>

namespace hal::utils {

bool Config::loadFromJson(const std::string &path, std::string &err) {
    std::ifstream in(path);
    if (!in) {
        err = "cannot open file: " + path;
        return false;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    if (!loadFromString(buf.str(), err))
        return false;
    file_path_ = path;
    return true;
}

bool Config::loadFromString(const std::string &jsonStr, std::string &err) {
    try {
        root_ = nlohmann::json::parse(jsonStr);
    } catch (const std::exception &e) {
        err = std::string("json parse error: ") + e.what();
        return false;
    }
    return true;
}

bool Config::hasKey(const std::string &key) const {
    return root_.is_object() && root_.contains(key);
}

int Config::getInt(const std::string &key, int defaultVal) const {
    if (hasKey(key) && root_[key].is_number_integer())
        return root_[key].get<int>();
    return defaultVal;
}

std::string Config::getString(const std::string &key, const std::string &defaultVal) const {
    if (hasKey(key) && root_[key].is_string())
        return root_[key].get<std::string>();
    return defaultVal;
}

bool Config::getBool(const std::string &key, bool defaultVal) const {
    if (hasKey(key) && root_[key].is_boolean())
        return root_[key].get<bool>();
    return defaultVal;
}

nlohmann::json Config::getArray(const std::string &key) const {
    if (hasKey(key) && root_[key].is_array())
        return root_[key];
    return nlohmann::json::array();
}

nlohmann::json Config::getObject(const std::string &key) const {
    if (hasKey(key) && root_[key].is_object())
        return root_[key];
    return nlohmann::json::object();
}

bool Config::validate(const std::vector<std::string> &requiredKeys, std::string &err) const {
    if (!root_.is_object()) {
        err = "config root must be a JSON object";
        return false;
    }
    for (const auto &key : requiredKeys) {
        if (!root_.contains(key)) {
            err = "missing required key: " + key;
            return false;
        }
    }
    return true;
}

} // namespace hal::utils
