import Foundation
import CryptoKit
import Network
import IOKit
import Darwin

final class DeviceInfoService {
    struct DeviceInfo: Encodable {
        let cpuModel: String
        let deviceUUID: String
        let diskSerialNumber: String
        let memory: String
        let macAddress: String
        let fingerprint: String
    }

    private let queue = DispatchQueue(label: "DeviceInfoBridge.server")
    private var listener: NWListener?
    private var activePort: UInt16?
    private var listenerTask: Task<Void, Never>?

    var onPortReady: ((UInt16) -> Void)?
    var onPortUnavailable: (() -> Void)?

    let deviceID: String
    let deviceInfo: DeviceInfo

    init() {
        let cpuModel = Self.cpuModel()
        let deviceUUID = Self.platformUUID()
        let diskSerialNumber = Self.diskSerialNumber()
        let memory = Self.memoryString(bytes: Self.memoryBytes())
        let macAddress = Self.normalizedMACAddress(Self.wifiMACAddress())
        let fingerprint = Self.sha256Hex([
            cpuModel,
            deviceUUID,
            diskSerialNumber,
            memory,
            macAddress
        ].joined(separator: "|"))

        self.deviceInfo = DeviceInfo(
            cpuModel: cpuModel,
            deviceUUID: deviceUUID,
            diskSerialNumber: diskSerialNumber,
            memory: memory,
            macAddress: macAddress,
            fingerprint: fingerprint
        )
        self.deviceID = fingerprint
    }

    func start() {
        guard listenerTask == nil else { return }
        listenerTask = Task {
            await self.run()
        }
    }

    func stop() {
        listener?.cancel()
        listener = nil
        listenerTask?.cancel()
        listenerTask = nil
    }

    var statusText: String {
        guard activePort != nil else { return "设备认证服务未启动" }
        return "设备认证服务已启动"
    }

    private func run() async {
        for port in 17980...17999 {
            if Task.isCancelled { return }
            if await startListener(on: UInt16(port)) {
                activePort = UInt16(port)
                onPortReady?(UInt16(port))
                return
            }
        }
        onPortUnavailable?()
    }

    private func startListener(on port: UInt16) async -> Bool {
        await withCheckedContinuation { continuation in
            do {
                let params = NWParameters.tcp
                params.allowLocalEndpointReuse = true
                let listenerPort = NWEndpoint.Port(rawValue: port)!
                params.requiredLocalEndpoint = .hostPort(host: .ipv4(IPv4Address("127.0.0.1")!), port: listenerPort)
                let listener = try NWListener(using: params)
                self.listener = listener
                listener.newConnectionHandler = { [weak self] connection in
                    guard let service = self else { return }
                    Task { @MainActor in
                        service.handle(connection: connection)
                    }
                }
                listener.stateUpdateHandler = { state in
                    switch state {
                    case .ready:
                        listener.stateUpdateHandler = nil
                        continuation.resume(returning: true)
                    case .failed, .cancelled:
                        listener.stateUpdateHandler = nil
                        continuation.resume(returning: false)
                    default:
                        break
                    }
                }
                listener.start(queue: queue)
            } catch {
                continuation.resume(returning: false)
            }
        }
    }

    private func handle(connection: NWConnection) {
        connection.start(queue: queue)
        receiveRequest(on: connection, buffer: Data(), remaining: 1)
    }

    private func receiveRequest(on connection: NWConnection, buffer: Data, remaining: Int) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16_384) { [weak self] data, _, isComplete, error in
            guard let service = self else { return }
            Task { @MainActor in
                service.processReceivedRequest(
                    on: connection,
                    buffer: buffer,
                    remaining: remaining,
                    data: data,
                    isComplete: isComplete,
                    error: error
                )
            }
        }
    }

    private func processReceivedRequest(on connection: NWConnection, buffer: Data, remaining: Int, data: Data?, isComplete: Bool, error: NWError?) {
        if let data, !data.isEmpty {
            let request = buffer + data
            if let parsed = HTTPRequest.parse(request) {
                respond(to: parsed, on: connection)
                return
            }
            if !isComplete, remaining > 0 {
                receiveRequest(on: connection, buffer: request, remaining: remaining - 1)
                return
            }
        }
        if error != nil || isComplete {
            connection.cancel()
        } else {
            receiveRequest(on: connection, buffer: buffer, remaining: remaining)
        }
    }

    private func respond(to request: HTTPRequest, on connection: NWConnection) {
        guard request.method == "GET", request.path == "/info" else {
            send(status: 404, body: Data("Not Found".utf8), origin: request.origin, on: connection)
            return
        }

        guard AppConfig.isAllowedOrigin(request.origin) else {
            send(status: 403, body: Data("Forbidden".utf8), origin: nil, on: connection)
            return
        }

        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted]
        let body = (try? encoder.encode(deviceInfo)) ?? Data("{}".utf8)
        send(status: 200, body: body, origin: request.normalizedOrigin, on: connection, contentType: "application/json; charset=utf-8")
    }

    private func send(status: Int, body: Data, origin: String?, on connection: NWConnection, contentType: String = "text/plain; charset=utf-8") {
        var headers: [String] = [
            "HTTP/1.1 \(status) \(HTTPResponse.reasonPhrase(for: status))",
            "Content-Type: \(contentType)",
            "Content-Length: \(body.count)",
            "Connection: close"
        ]
        if AppConfig.isAllowedOrigin(origin), let allowedOrigin = origin.map(AppConfig.normalizedOrigin) {
            headers.append("Access-Control-Allow-Origin: \(allowedOrigin)")
            headers.append("Vary: Origin")
            headers.append("Access-Control-Allow-Methods: GET, OPTIONS")
            headers.append("Access-Control-Allow-Headers: Content-Type")
        }
        let response = (headers.joined(separator: "\r\n") + "\r\n\r\n").data(using: .utf8)! + body
        connection.send(content: response, completion: .contentProcessed { _ in
            connection.cancel()
        })
    }

    private static func cpuModel() -> String {
        let displayInfo = systemProfilerDisplayInfo()
        let model = [
            sysctlString("machdep.cpu.brand_string"),
            displayInfo.model,
            sysctlString("hw.model")
        ].first { !$0.isEmpty } ?? ""

        let cpuCores = sysctlInt("hw.physicalcpu")
        let gpuCores = displayInfo.gpuCores

        switch (cpuCores, gpuCores) {
        case let (cpu, gpu) where cpu > 0 && gpu > 0:
            return "\(model) (\(cpu)-core CPU \(gpu)-core GPU)"
        case let (cpu, _) where cpu > 0:
            return "\(model) (\(cpu)-core CPU)"
        case let (_, gpu) where gpu > 0:
            return "\(model) (\(gpu)-core GPU)"
        default:
            return model
        }
    }

    private static func platformUUID() -> String {
        let matching = IOServiceMatching("IOPlatformExpertDevice")
        let service = IOServiceGetMatchingService(kIOMainPortDefault, matching)
        guard service != 0 else { return "" }
        defer { IOObjectRelease(service) }
        guard let uuid = IORegistryEntryCreateCFProperty(service, "IOPlatformUUID" as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue() as? String else {
            return ""
        }
        return uuid
    }

    private static func memoryBytes() -> UInt64 {
        var size: UInt64 = 0
        var len = MemoryLayout<UInt64>.size
        sysctlbyname("hw.memsize", &size, &len, nil, 0)
        return size
    }

    private static func memoryString(bytes: UInt64) -> String {
        let gib = Double(bytes) / 1_073_741_824
        if gib.rounded() == gib {
            return "\(Int(gib))GiB"
        }
        return String(format: "%.1fGiB", gib)
    }

    private static func diskSerialNumber() -> String {
        let service = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOBlockStorageDevice"))
        guard service != 0 else {
            return ""
        }
        defer { IOObjectRelease(service) }
        let key = "Serial Number"
        if let value = IORegistryEntrySearchCFProperty(service, kIOServicePlane, key as CFString, kCFAllocatorDefault, IOOptionBits(kIORegistryIterateRecursively | kIORegistryIterateParents)) as? String {
            return value
        }
        return ""
    }

    private static func wifiMACAddress() -> String {
        guard let interfaceName = wifiInterfaceName() else { return "" }
        return macAddress(forBSDInterfaceName: interfaceName)
    }

    private static func wifiInterfaceName() -> String? {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/sbin/system_profiler")
        process.arguments = ["SPAirPortDataType", "-json"]

        let output = Pipe()
        process.standardOutput = output
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return nil
        }

        guard process.terminationStatus == 0 else { return nil }
        let data = output.fileHandleForReading.readDataToEndOfFile()
        guard
            let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let airportItems = object["SPAirPortDataType"] as? [[String: Any]]
        else {
            return nil
        }

        for item in airportItems {
            guard let interfaces = item["spairport_airport_interfaces"] as? [[String: Any]] else {
                continue
            }
            if let name = interfaces.compactMap({ $0["_name"] as? String }).first(where: { !$0.isEmpty }) {
                return name
            }
        }

        return nil
    }

    private static func macAddress(forBSDInterfaceName interfaceName: String) -> String {
        var addresses: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&addresses) == 0, let firstAddress = addresses else {
            return ""
        }
        defer { freeifaddrs(addresses) }

        for address in sequence(first: firstAddress, next: { $0.pointee.ifa_next }) {
            let interface = address.pointee
            guard
                let name = interface.ifa_name,
                String(cString: name) == interfaceName,
                let socketAddress = interface.ifa_addr,
                socketAddress.pointee.sa_family == UInt8(AF_LINK)
            else {
                continue
            }

            let linkAddress = socketAddress.withMemoryRebound(to: sockaddr_dl.self, capacity: 1) { $0.pointee }
            let nameLength = Int(linkAddress.sdl_nlen)
            let addressLength = Int(linkAddress.sdl_alen)
            guard addressLength == 6 else { return "" }

            let bytes = withUnsafeBytes(of: linkAddress.sdl_data) { buffer in
                guard nameLength + addressLength <= buffer.count else { return [UInt8]() }
                return Array(buffer[nameLength..<(nameLength + addressLength)])
            }
            guard bytes.count == 6 else { return "" }
            return bytes.map { String(format: "%02X", $0) }.joined(separator: ":")
        }

        return ""
    }

    private static func normalizedMACAddress(_ macAddress: String) -> String {
        macAddress
            .filter { $0.isHexDigit }
            .uppercased()
    }

    private static func sysctlString(_ name: String) -> String {
        var size = 0
        guard sysctlbyname(name, nil, &size, nil, 0) == 0, size > 0 else { return "" }
        var buffer = [CChar](repeating: 0, count: size)
        guard sysctlbyname(name, &buffer, &size, nil, 0) == 0 else { return "" }
        return String(cString: buffer)
    }

    private static func sysctlInt(_ name: String) -> Int {
        var value: Int32 = 0
        var size = MemoryLayout<Int32>.size
        guard sysctlbyname(name, &value, &size, nil, 0) == 0 else { return 0 }
        return Int(value)
    }

    private static func sha256Hex(_ input: String) -> String {
        let data = Data(input.utf8)
        let digest = SHA256.hash(data: data)
        return digest.map { String(format: "%02X", $0) }.joined()
    }

    private static func systemProfilerDisplayInfo() -> (model: String, gpuCores: Int) {
        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/sbin/system_profiler")
        process.arguments = ["SPDisplaysDataType", "-json"]

        let output = Pipe()
        process.standardOutput = output
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return ("", 0)
        }

        guard process.terminationStatus == 0 else { return ("", 0) }
        let data = output.fileHandleForReading.readDataToEndOfFile()
        guard
            let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
            let displays = object["SPDisplaysDataType"] as? [[String: Any]],
            let display = displays.first
        else {
            return ("", 0)
        }

        let model = (display["sppci_model"] as? String) ?? (display["_name"] as? String) ?? ""
        let gpuCores = Int((display["sppci_cores"] as? String) ?? "") ?? 0
        return (model, gpuCores)
    }
}

private struct HTTPRequest {
    let method: String
    let path: String
    let origin: String?

    var normalizedOrigin: String? {
        origin.map(AppConfig.normalizedOrigin)
    }

    static func parse(_ data: Data) -> HTTPRequest? {
        guard let text = String(data: data, encoding: .utf8),
              let head = text.components(separatedBy: "\r\n\r\n").first else { return nil }
        let lines = head.components(separatedBy: "\r\n")
        guard let first = lines.first else { return nil }
        let parts = first.split(separator: " ")
        guard parts.count >= 2 else { return nil }
        var origin: String?
        for line in lines.dropFirst() {
            let pair = line.split(separator: ":", maxSplits: 1)
            guard pair.count == 2 else { continue }
            let key = pair[0].trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            let value = pair[1].trimmingCharacters(in: .whitespacesAndNewlines)
            if key == "origin" {
                origin = value
            }
        }
        return HTTPRequest(method: String(parts[0]), path: String(parts[1]), origin: origin)
    }
}

private enum HTTPResponse {
    static func reasonPhrase(for status: Int) -> String {
        switch status {
        case 200: return "OK"
        case 403: return "Forbidden"
        case 404: return "Not Found"
        default: return "OK"
        }
    }
}
