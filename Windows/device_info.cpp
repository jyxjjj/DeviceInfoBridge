#include "device_info.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <iphlpapi.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#ifndef IF_TYPE_IEEE80211
#define IF_TYPE_IEEE80211 71
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

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) {
        return "";
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return "";
    }
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, &result[0], size, nullptr, nullptr);
    result.pop_back();
    return result;
}

std::string RegistryString(HKEY root, const wchar_t* path, const wchar_t* name) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &key) != ERROR_SUCCESS) {
        return "";
    }

    DWORD type = 0;
    DWORD bytes = 0;
    LONG status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || bytes == 0) {
        RegCloseKey(key);
        return "";
    }

    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    status = RegQueryValueExW(key, name, nullptr, nullptr, reinterpret_cast<LPBYTE>(&value[0]), &bytes);
    RegCloseKey(key);
    if (status != ERROR_SUCCESS) {
        return "";
    }

    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return Trim(WideToUtf8(value));
}

int PhysicalCoreCount() {
    DWORD bytes = 0;
    if (GetLogicalProcessorInformation(nullptr, &bytes) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return 0;
    }

    std::vector<unsigned char> buffer(bytes);
    auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buffer.data());
    if (!GetLogicalProcessorInformation(info, &bytes)) {
        return 0;
    }

    int cores = 0;
    const DWORD count = bytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
    for (DWORD i = 0; i < count; ++i) {
        if (info[i].Relationship == RelationProcessorCore) {
            ++cores;
        }
    }
    return cores;
}

std::string CpuModel() {
    const std::string model = RegistryString(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");
    const int cores = PhysicalCoreCount();
    if (cores > 0 && !model.empty()) {
        return model + " (" + std::to_string(cores) + "-core CPU)";
    }
    return model;
}

std::string MemoryString() {
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return "";
    }

    const double gib = static_cast<double>(status.ullTotalPhys) / 1073741824.0;
    const double rounded = static_cast<double>(static_cast<unsigned long long>(gib + 0.5));
    std::ostringstream out;
    if (gib == rounded) {
        out << static_cast<unsigned long long>(rounded) << "GiB";
    } else {
        out << std::fixed << std::setprecision(1) << gib << "GiB";
    }
    return out.str();
}

std::string HexBytes(const unsigned char* bytes, size_t size) {
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setfill('0');
    for (size_t i = 0; i < size; ++i) {
        out << std::setw(2) << static_cast<int>(bytes[i]);
    }
    return out.str();
}

std::string FormatUuid(const unsigned char* uuid) {
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < 16; ++i) {
        all_zero = all_zero && uuid[i] == 0x00;
        all_ff = all_ff && uuid[i] == 0xFF;
    }
    if (all_zero || all_ff) {
        return "";
    }

    std::array<unsigned char, 16> ordered = {
        uuid[3], uuid[2], uuid[1], uuid[0],
        uuid[5], uuid[4],
        uuid[7], uuid[6],
        uuid[8], uuid[9],
        uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]
    };

    const std::string hex = HexBytes(ordered.data(), ordered.size());
    return hex.substr(0, 8) + "-" + hex.substr(8, 4) + "-" + hex.substr(12, 4) + "-" +
           hex.substr(16, 4) + "-" + hex.substr(20, 12);
}

std::string FirmwareUuid() {
    constexpr DWORD signature = 0x52534D42;  // "RSMB"
    const UINT size = GetSystemFirmwareTable(signature, 0, nullptr, 0);
    if (size == 0) {
        return "";
    }

    std::vector<unsigned char> buffer(size);
    if (GetSystemFirmwareTable(signature, 0, buffer.data(), size) != size || size < 8) {
        return "";
    }

    const unsigned char* table = buffer.data() + 8;
    size_t remaining = size - 8;
    while (remaining >= 4) {
        const unsigned char type = table[0];
        const unsigned char length = table[1];
        if (length < 4 || length > remaining) {
            break;
        }
        if (type == 1 && length >= 0x19) {
            return FormatUuid(table + 0x08);
        }

        size_t consumed = length;
        while (consumed + 1 < remaining) {
            if (table[consumed] == 0 && table[consumed + 1] == 0) {
                consumed += 2;
                break;
            }
            ++consumed;
        }
        if (consumed > remaining) {
            break;
        }
        table += consumed;
        remaining -= consumed;
    }
    return "";
}

std::string DiskSerialNumber() {
    HANDLE drive = CreateFileW(
        L"\\\\.\\PhysicalDrive0",
        0,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (drive == INVALID_HANDLE_VALUE) {
        return "";
    }

    STORAGE_PROPERTY_QUERY query = {};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;

    std::array<unsigned char, 1024> buffer = {};
    DWORD bytes = 0;
    const BOOL ok = DeviceIoControl(
        drive,
        IOCTL_STORAGE_QUERY_PROPERTY,
        &query,
        sizeof(query),
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        &bytes,
        nullptr);
    CloseHandle(drive);
    if (!ok || bytes < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        return "";
    }

    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    if (descriptor->SerialNumberOffset == 0 || descriptor->SerialNumberOffset >= bytes) {
        return "";
    }
    const char* serial = reinterpret_cast<const char*>(buffer.data() + descriptor->SerialNumberOffset);
    return Trim(std::string(serial));
}

std::string MacAddress() {
    ULONG bytes = 15 * 1024;
    std::vector<unsigned char> buffer(bytes);
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &bytes);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(bytes);
        result = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()), &bytes);
    }
    if (result != NO_ERROR) {
        return "";
    }

    PIP_ADAPTER_ADDRESSES fallback = nullptr;
    for (auto* adapter = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data()); adapter; adapter = adapter->Next) {
        if (adapter->PhysicalAddressLength != 6) {
            continue;
        }
        if (adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK || adapter->IfType == IF_TYPE_TUNNEL) {
            continue;
        }
        if (fallback == nullptr) {
            fallback = adapter;
        }
        if (adapter->OperStatus == IfOperStatusUp &&
            (adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211)) {
            return HexBytes(adapter->PhysicalAddress, adapter->PhysicalAddressLength);
        }
    }

    if (fallback != nullptr) {
        return HexBytes(fallback->PhysicalAddress, fallback->PhysicalAddressLength);
    }
    return "";
}

std::string Sha256Hex(const std::string& input) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD bytes = 0;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
        return "";
    }
    auto close_algorithm = [&]() {
        if (algorithm) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    };

    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size), &bytes, 0) != 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_size), sizeof(hash_size), &bytes, 0) != 0) {
        close_algorithm();
        return "";
    }

    std::vector<unsigned char> object(object_size);
    std::vector<unsigned char> digest(hash_size);
    if (BCryptCreateHash(algorithm, &hash, object.data(), object_size, nullptr, 0, 0) != 0 ||
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())), static_cast<ULONG>(input.size()), 0) != 0 ||
        BCryptFinishHash(hash, digest.data(), hash_size, 0) != 0) {
        if (hash) {
            BCryptDestroyHash(hash);
        }
        close_algorithm();
        return "";
    }

    BCryptDestroyHash(hash);
    close_algorithm();
    return HexBytes(digest.data(), digest.size());
}

std::string JsonEscape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': out << "\\\\"; break;
        case '"': out << "\\\""; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
            } else {
                out << static_cast<char>(ch);
            }
        }
    }
    return out.str();
}

}  // namespace

std::string DeviceInfo::ToJson() const {
    std::ostringstream out;
    out << "{\n"
        << "  \"cpuModel\" : \"" << JsonEscape(cpuModel) << "\",\n"
        << "  \"deviceUUID\" : \"" << JsonEscape(deviceUUID) << "\",\n"
        << "  \"diskSerialNumber\" : \"" << JsonEscape(diskSerialNumber) << "\",\n"
        << "  \"memory\" : \"" << JsonEscape(memory) << "\",\n"
        << "  \"macAddress\" : \"" << JsonEscape(macAddress) << "\",\n"
        << "  \"fingerprint\" : \"" << JsonEscape(fingerprint) << "\"\n"
        << "}";
    return out.str();
}

DeviceInfo CollectDeviceInfo() {
    DeviceInfo info;
    info.cpuModel = CpuModel();
    info.deviceUUID = FirmwareUuid();
    info.diskSerialNumber = DiskSerialNumber();
    info.memory = MemoryString();
    info.macAddress = MacAddress();
    info.fingerprint = Sha256Hex(
        info.cpuModel + "|" +
        info.deviceUUID + "|" +
        info.diskSerialNumber + "|" +
        info.memory + "|" +
        info.macAddress);
    return info;
}
