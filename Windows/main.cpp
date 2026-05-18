#include "app_config.h"
#include "device_info.h"
#include "http_server.h"
#include "resource.h"

#include <shellapi.h>
#include <windows.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace {

constexpr wchar_t kWindowClass[] = L"DeviceInfoBridgeWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kPortReadyMessage = WM_APP + 2;
constexpr UINT kPortUnavailableMessage = WM_APP + 3;
constexpr UINT_PTR kTrayId = 1;
constexpr int kCopyDeviceIdCommand = 1001;
constexpr int kQuitCommand = 1002;

UINT g_taskbar_created = 0;
std::unique_ptr<HttpServer> g_server;
DeviceInfo g_device_info;
uint16_t g_active_port = 0;
bool g_port_failed = false;

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) {
        return L"";
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 1) {
        return L"";
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}

void CopyText(HWND window, const std::string& text) {
    const std::wstring wide = Utf8ToWide(text);
    if (!OpenClipboard(window)) {
        return;
    }
    EmptyClipboard();

    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* data = GlobalLock(memory);
        if (data != nullptr) {
            memcpy(data, wide.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
            memory = nullptr;
        }
        if (memory != nullptr) {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void AddTrayIcon(HWND window) {
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcscpy_s(data.szTip, L"DeviceInfoBridge");
    Shell_NotifyIconW(NIM_ADD, &data);
}

void RemoveTrayIcon(HWND window) {
    NOTIFYICONDATAW data = {};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &data);
}

void ShowTrayMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }

    const std::wstring short_id = Utf8ToWide(g_device_info.fingerprint.substr(0, 8));
    const std::wstring id_text = L"设备ID：" + short_id + L"（点击复制）";
    AppendMenuW(menu, MF_STRING, kCopyDeviceIdCommand, id_text.c_str());
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"🟢 设备认证服务已启动");

    std::wstring port_text = L"端口：监听中";
    if (g_active_port != 0) {
        port_text = L"端口：" + std::to_wstring(g_active_port);
    } else if (g_port_failed) {
        port_text = L"端口：监听失败";
    }
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, port_text.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuitCommand, L"退出");

    POINT point = {};
    GetCursorPos(&point);
    SetForegroundWindow(window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, point.x, point.y, 0, window, nullptr);
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == g_taskbar_created) {
        AddTrayIcon(window);
        return 0;
    }

    switch (message) {
    case WM_CREATE:
        AddTrayIcon(window);
        return 0;
    case kTrayMessage:
        if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_LBUTTONUP) {
            ShowTrayMenu(window);
        }
        return 0;
    case kPortReadyMessage:
        g_active_port = static_cast<uint16_t>(wparam);
        g_port_failed = false;
        return 0;
    case kPortUnavailableMessage:
        g_active_port = 0;
        g_port_failed = true;
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wparam)) {
        case kCopyDeviceIdCommand:
            CopyText(window, g_device_info.fingerprint);
            return 0;
        case kQuitCommand:
            DestroyWindow(window);
            return 0;
        default:
            break;
        }
        break;
    case WM_DESTROY:
        RemoveTrayIcon(window);
        if (g_server) {
            g_server->Stop();
            g_server.reset();
        }
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
    g_device_info = CollectDeviceInfo();
    AppConfig config = AppConfig::Load();

    WNDCLASSW window_class = {};
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    window_class.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassW(&window_class);

    HWND window = CreateWindowExW(
        0,
        kWindowClass,
        L"DeviceInfoBridge",
        0,
        0,
        0,
        0,
        0,
        HWND_MESSAGE,
        nullptr,
        instance,
        nullptr);
    if (window == nullptr) {
        return 1;
    }

    g_server = std::make_unique<HttpServer>(config, g_device_info);
    g_server->Start(
        [window](uint16_t port) {
            PostMessageW(window, kPortReadyMessage, port, 0);
        },
        [window]() {
            PostMessageW(window, kPortUnavailableMessage, 0, 0);
        });

    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return static_cast<int>(message.wParam);
}
