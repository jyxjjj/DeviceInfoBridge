#include "app_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <vector>

#ifndef ALLOWED_ORIGINS
#define ALLOWED_ORIGINS ""
#endif

namespace {

std::string Trim(const std::string& value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char ch) {
        return std::isspace(ch) != 0;
    }).base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

std::vector<std::string> SplitOrigins(const std::string& value) {
    std::vector<std::string> parts;
    std::string current;
    for (const char ch : value) {
        if (ch == ',' || std::isspace(static_cast<unsigned char>(ch)) != 0) {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current.push_back(ch);
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

std::wstring ExecutableDirectory() {
    std::wstring path(MAX_PATH, L'\0');
    DWORD length = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
    while (length == path.size()) {
        path.resize(path.size() * 2);
        length = GetModuleFileNameW(nullptr, &path[0], static_cast<DWORD>(path.size()));
    }
    path.resize(length);
    const size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L".";
    }
    return path.substr(0, slash);
}

std::string ReadIniAllowedOrigins() {
    const std::wstring path = ExecutableDirectory() + L"\\DeviceInfoBridge.ini";
    // 使用 _wfopen_s 替代已弃用的 _wfopen，满足 SDL 安全检查
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) {
        return "";
    }

    char buffer[4096] = {};
    while (fgets(buffer, sizeof(buffer), file) != nullptr) {
        std::string line(buffer);
        line = Trim(line);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') {
            continue;
        }
        const size_t equals = line.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = Trim(line.substr(0, equals));
        if (key == "AllowedOrigins" || key == "AllowedOrigin") {
            fclose(file);
            return Trim(line.substr(equals + 1));
        }
    }
    fclose(file);
    return "";
}

}  // namespace

AppConfig AppConfig::Load() {
    AppConfig config;
    std::string origins = ReadIniAllowedOrigins();
    if (origins.empty()) {
        origins = ALLOWED_ORIGINS;
    }

    for (const std::string& origin : SplitOrigins(origins)) {
        const std::string normalized = NormalizeOrigin(origin);
        if (!normalized.empty()) {
            config.allowed_origins_.insert(normalized);
        }
    }
    return config;
}

bool AppConfig::IsAllowedOrigin(const std::string& origin) const {
    if (origin.empty()) {
        return false;
    }
    return allowed_origins_.find(NormalizeOrigin(origin)) != allowed_origins_.end();
}

std::string AppConfig::NormalizeOrigin(const std::string& value) {
    std::string trimmed = Trim(value);
    const size_t scheme_end = trimmed.find("://");
    if (scheme_end == std::string::npos) {
        return trimmed;
    }

    std::string scheme = trimmed.substr(0, scheme_end);
    std::transform(scheme.begin(), scheme.end(), scheme.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    const size_t host_begin = scheme_end + 3;
    size_t host_end = trimmed.find_first_of("/?#", host_begin);
    if (host_end == std::string::npos) {
        host_end = trimmed.size();
    }

    std::string host_port = trimmed.substr(host_begin, host_end - host_begin);
    std::transform(host_port.begin(), host_port.end(), host_port.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });

    return scheme + "://" + host_port;
}
