#pragma once

#include "app_config.h"
#include "device_info.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <thread>

class HttpServer {
public:
    HttpServer(const AppConfig& config, const DeviceInfo& device_info);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    void Start(std::function<void(uint16_t)> on_ready, std::function<void()> on_unavailable);
    void Stop();

private:
    void Run();
    void HandleClient(uintptr_t client_socket);

    AppConfig config_;
    DeviceInfo device_info_;
    std::function<void(uint16_t)> on_ready_;
    std::function<void()> on_unavailable_;
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    uintptr_t listen_socket_{~static_cast<uintptr_t>(0)};
};
