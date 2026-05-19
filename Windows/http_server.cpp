#include "http_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>

namespace {

constexpr SOCKET kInvalidSocket = INVALID_SOCKET;

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

struct HttpRequest {
    std::string method;
    std::string path;
    std::string origin;
};

bool ParseRequest(const std::string& text, HttpRequest* request) {
    const size_t header_end = text.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return false;
    }

    std::istringstream input(text.substr(0, header_end));
    std::string line;
    if (!std::getline(input, line)) {
        return false;
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream first(line);
    if (!(first >> request->method >> request->path)) {
        return false;
    }

    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        std::string key = Trim(line.substr(0, colon));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        if (key == "origin") {
            request->origin = Trim(line.substr(colon + 1));
        }
    }
    return true;
}

std::string ReasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    default: return "OK";
    }
}

void SendAll(SOCKET socket, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int chunk = send(socket, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (chunk <= 0) {
            return;
        }
        sent += static_cast<size_t>(chunk);
    }
}

SOCKET BindPort(uint16_t port) {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == kInvalidSocket) {
        return kInvalidSocket;
    }

    BOOL exclusive = TRUE;
    setsockopt(socket_handle, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive));

    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);

    if (bind(socket_handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR ||
        listen(socket_handle, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(socket_handle);
        return kInvalidSocket;
    }
    return socket_handle;
}

}  // namespace

HttpServer::HttpServer(const AppConfig& config, const DeviceInfo& device_info)
    : config_(config), device_info_(device_info) {}

HttpServer::~HttpServer() {
    Stop();
}

void HttpServer::Start(std::function<void(uint16_t)> on_ready, std::function<void()> on_unavailable) {
    if (running_.exchange(true)) {
        return;
    }
    on_ready_ = std::move(on_ready);
    on_unavailable_ = std::move(on_unavailable);
    server_thread_ = std::thread(&HttpServer::Run, this);
}

void HttpServer::Stop() {
    const bool was_running = running_.exchange(false);
    if (was_running) {
        const SOCKET socket_handle = static_cast<SOCKET>(listen_socket_);
        if (socket_handle != kInvalidSocket) {
            shutdown(socket_handle, SD_BOTH);
            closesocket(socket_handle);
        }
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void HttpServer::Run() {
    WSADATA data = {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        if (on_unavailable_) {
            on_unavailable_();
        }
        running_ = false;
        return;
    }

    SOCKET socket_handle = kInvalidSocket;
    uint16_t active_port = 0;
    for (uint16_t port = 17980; port <= 17999; ++port) {
        socket_handle = BindPort(port);
        if (socket_handle != kInvalidSocket) {
            active_port = port;
            break;
        }
    }

    if (socket_handle == kInvalidSocket) {
        if (on_unavailable_) {
            on_unavailable_();
        }
        running_ = false;
        WSACleanup();
        return;
    }

    listen_socket_ = static_cast<uintptr_t>(socket_handle);
    if (on_ready_) {
        on_ready_(active_port);
    }

    while (running_) {
        SOCKET client = accept(socket_handle, nullptr, nullptr);
        if (client == kInvalidSocket) {
            if (running_) {
                Sleep(10);
            }
            continue;
        }
        DWORD timeout_ms = 3000;
        setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
        HandleClient(static_cast<uintptr_t>(client));
    }

    listen_socket_ = static_cast<uintptr_t>(kInvalidSocket);
    WSACleanup();
}

void HttpServer::HandleClient(uintptr_t client_socket) {
    const SOCKET socket_handle = static_cast<SOCKET>(client_socket);
    std::string request_text;
    request_text.reserve(16 * 1024);

    std::array<char, 8192> buffer = {};
    for (int i = 0; i < 2; ++i) {
        const int received = recv(socket_handle, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (received <= 0) {
            closesocket(socket_handle);
            return;
        }
        request_text.append(buffer.data(), static_cast<size_t>(received));
        if (request_text.find("\r\n\r\n") != std::string::npos) {
            break;
        }
    }

    HttpRequest request;
    int status = 404;
    std::string body = "Not Found";
    std::string content_type = "text/plain; charset=utf-8";
    std::string cors_origin;

    if (ParseRequest(request_text, &request)) {
        if (request.method == "GET" && request.path == "/info") {
            if (config_.IsAllowedOrigin(request.origin)) {
                status = 200;
                body = device_info_.ToJson();
                content_type = "application/json; charset=utf-8";
                cors_origin = AppConfig::NormalizeOrigin(request.origin);
            } else {
                status = 403;
                body = "Forbidden";
            }
        } else if (config_.IsAllowedOrigin(request.origin)) {
            cors_origin = AppConfig::NormalizeOrigin(request.origin);
        }
    }

    std::ostringstream response;
    response << "HTTP/1.1 " << status << " " << ReasonPhrase(status) << "\r\n"
             << "Content-Type: " << content_type << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n";
    if (!cors_origin.empty()) {
        response << "Access-Control-Allow-Origin: " << cors_origin << "\r\n"
                 << "Vary: Origin\r\n"
                 << "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                 << "Access-Control-Allow-Headers: Content-Type\r\n";
    }
    response << "\r\n" << body;

    SendAll(socket_handle, response.str());
    shutdown(socket_handle, SD_BOTH);
    closesocket(socket_handle);
}
