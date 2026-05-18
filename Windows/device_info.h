#pragma once

#include <string>

struct DeviceInfo {
    std::string cpuModel;
    std::string deviceUUID;
    std::string diskSerialNumber;
    std::string memory;
    std::string macAddress;
    std::string fingerprint;

    std::string ToJson() const;
};

DeviceInfo CollectDeviceInfo();
