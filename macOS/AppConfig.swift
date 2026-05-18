import Foundation

enum AppConfig {
    static let allowedOrigins: Set<String> = {
        let value = (Bundle.main.object(forInfoDictionaryKey: "AllowedOrigins") as? String)
            ?? (Bundle.main.object(forInfoDictionaryKey: "AllowedOrigin") as? String)
            ?? ""
        return Set(value
            .components(separatedBy: CharacterSet(charactersIn: ", \n\t"))
            .map(normalizedOrigin)
            .filter { !$0.isEmpty })
    }()

    static func isAllowedOrigin(_ origin: String?) -> Bool {
        guard let origin else { return false }
        return allowedOrigins.contains(normalizedOrigin(origin))
    }

    nonisolated static func normalizedOrigin(_ value: String) -> String {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let url = URL(string: trimmed),
              let scheme = url.scheme,
              let host = url.host else {
            return trimmed
        }

        if let port = url.port {
            return "\(scheme)://\(host):\(port)"
        }
        return "\(scheme)://\(host)"
    }
}
