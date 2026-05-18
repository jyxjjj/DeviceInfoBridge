import SwiftUI
import AppKit

@main
struct DeviceInfoBridgeApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate

    var body: some Scene {
        Settings {
            EmptyView()
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    private let service = DeviceInfoService()
    private var statusItem: NSStatusItem?
    private var portMenuItem: NSMenuItem?

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.accessory)
        service.onPortReady = { [weak self] port in
            DispatchQueue.main.async {
                self?.portMenuItem?.title = "端口：\(port)"
            }
        }
        service.onPortUnavailable = { [weak self] in
            DispatchQueue.main.async {
                self?.portMenuItem?.title = "端口：监听失败"
            }
        }
        service.start()
        setupStatusItem()
    }

    func applicationWillTerminate(_ notification: Notification) {
        service.stop()
    }

    private func setupStatusItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        statusItem = item

        if let button = item.button {
            let icon = NSApp.applicationIconImage.copy() as? NSImage
            icon?.size = NSSize(width: 18, height: 18)
            icon?.isTemplate = false
            button.image = icon
            button.imagePosition = .imageOnly
        }

        let menu = NSMenu()

        let shortDeviceID = String(service.deviceID.prefix(8))
        let idItem = NSMenuItem(title: "设备ID：\(shortDeviceID)（点击复制）", action: #selector(copyDeviceID), keyEquivalent: "")
        idItem.target = self
        menu.addItem(idItem)

        menu.addItem(NSMenuItem(title: "🟢 设备认证服务已启动", action: nil, keyEquivalent: ""))

        let portItem = NSMenuItem(title: "端口：监听中", action: nil, keyEquivalent: "")
        portMenuItem = portItem
        menu.addItem(portItem)

        menu.addItem(.separator())

        let quitItem = NSMenuItem(title: "退出", action: #selector(quitApp), keyEquivalent: "")
        quitItem.target = self
        menu.addItem(quitItem)

        item.menu = menu
    }

    @objc private func copyDeviceID() {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(service.deviceID, forType: .string)
    }

    @objc private func quitApp() {
        NSApp.terminate(nil)
    }
}
