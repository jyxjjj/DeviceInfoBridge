#pragma once

#include <set>
#include <string>

class AppConfig {
public:
    static AppConfig Load();

    bool IsAllowedOrigin(const std::string& origin) const;
    static std::string NormalizeOrigin(const std::string& value);

private:
    std::set<std::string> allowed_origins_;
};
