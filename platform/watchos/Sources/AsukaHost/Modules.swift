// The watch host's modules: what the platform SDK's packages over
// platform/host call by name (ui.Call), as iOS's host has them
// (platform/ios Modules.swift), those watchOS has. What a package
// implements in its own Swift (platform/device, platform/secure...) comes
// with the package, as a typed native module, not here.

import Foundation
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A module's method: the call's bytes, and a reply (once).
public typealias AsukaModuleMethod = (_ args: Data, _ reply: @escaping (Result<Data, Error>) -> Void) -> Void

struct AsukaModuleError: LocalizedError {
    let message: String
    init(_ m: String) { message = m }
    var errorDescription: String? { message }
}

@MainActor final class AsukaModules {
    /// The methods, by module and method name.
    var methods: [String: [String: AsukaModuleMethod]] = [:]
    var registered = false
}

@MainActor extension AsukaRuntime {
    /// Registers a module for ui.Call(module, method, ...); an app's
    /// module of a built-in one's name stays the app's.
    public func registerModule(_ name: String, _ methods: [String: AsukaModuleMethod]) {
        modules.methods[name] = methods
    }

    /// Runs a Call command and answers with a Result event, on a later
    /// turn of the main queue, so the call doesn't wait on the core.
    func call(_ root: UInt32, _ args: UnsafeRawBufferPointer) {
        guard args.count >= 12 else { return }
        var r = AsukaReader(args)
        let id = r.u32()
        let module = r.string(), method = r.string()
        let payload = Data(bytes: r.p, count: r.end - r.p)
        if !modules.registered {
            modules.registered = true
            let apps = modules.methods
            registerBuiltinModules()
            for (k, v) in apps { modules.methods[k] = v }
        }
        var answered = false
        let reply: (Result<Data, Error>) -> Void = { [weak self] res in
            DispatchQueue.main.async {
                guard let self, !answered else { return }
                answered = true
                var out = [UInt8]()
                withUnsafeBytes(of: id.littleEndian) { out.append(contentsOf: $0) }
                switch res {
                case .success(let d): out.append(1); out.append(contentsOf: d)
                case .failure(let e): out.append(0); out.append(contentsOf: Array(e.localizedDescription.utf8))
                }
                let bytes = out
                self.send(AsukaEvents.result(root, bytes))
            }
        }
        guard let m = modules.methods[module]?[method] else {
            reply(.failure(AsukaModuleError("not available on Apple Watch: \(module).\(method)")))
            return
        }
        m(payload, reply)
    }

    /// The host's own modules.
    func registerBuiltinModules() {
        // fs.readAsset: an asset's bytes, read off the main thread.
        modules.methods["assets"] = ["read": { args, reply in
            let name = String(decoding: args, as: UTF8.self)
            DispatchQueue.global(qos: .userInitiated).async {
                guard let url = AsukaAssets.url(name), url.isFileURL, let data = try? Data(contentsOf: url) else {
                    return reply(.failure(AsukaModuleError("no asset \(name)")))
                }
                reply(.success(data))
            }
        }]
        // platform/recent, platform/login and platform/dock: a desktop's.
        modules.methods["recent"] = [
            "add": { _, reply in reply(.success(Data())) },
            "clear": { _, reply in reply(.success(Data())) },
            "list": { _, reply in reply(.success(Data("[]".utf8))) },
        ]
        modules.methods["login"] = [
            "set": { _, reply in reply(.failure(AsukaModuleError("not available: a watch app doesn't start at login"))) },
            "enabled": { _, reply in reply(.success(Data([0]))) },
            "openedAtLogin": { _, reply in reply(.success(Data([0]))) },
        ]
        modules.methods["dock"] = [
            "progress": { _, reply in reply(.success(Data())) },
            "attention": { _, reply in reply(.success(Data([0, 0, 0, 0]))) },
            "cancelAttention": { _, reply in reply(.success(Data())) },
        ]
        // platform/image: the watch keeps no decoded images, only what
        // URLSession's cache keeps of their files: prefetch fetches them
        // into it (with their headers), clearCache's disk empties it.
        modules.methods["image"] = [
            "prefetch": { args, reply in
                let b = [UInt8](args)
                var i = 0
                func u32() -> Int? {
                    guard b.count - i >= 4 else { return nil }
                    defer { i += 4 }
                    return Int(b[i]) | Int(b[i + 1]) << 8 | Int(b[i + 2]) << 16 | Int(b[i + 3]) << 24
                }
                func strings() -> [String] {
                    var out: [String] = []
                    for _ in 0..<(u32() ?? 0) {
                        guard let n = u32(), b.count - i >= n else { break }
                        out.append(String(decoding: b[i..<i + n], as: UTF8.self))
                        i += n
                    }
                    return out
                }
                let urls = strings(), h = strings()
                for u in urls {
                    guard let url = URL(string: u), url.scheme == "http" || url.scheme == "https" else { continue }
                    var req = URLRequest(url: url)
                    for k in stride(from: 0, to: h.count - 1, by: 2) { req.setValue(h[k + 1], forHTTPHeaderField: h[k]) }
                    URLSession.shared.dataTask(with: req) { _, _, _ in }.resume()
                }
                reply(.success(Data()))
            },
            "clearCache": { args, reply in
                if (args.first ?? 3) & 2 != 0 { URLCache.shared.removeAllCachedResponses() }
                reply(.success(Data()))
            },
        ]
        // platform/widget: the complications' entries (WidgetData.swift).
        modules.methods["widget"] = AsukaWidgetData.methods()
        #if ASUKA_PUSH
        // platform/push: APNs, as iOS's (Push.swift).
        modules.methods["push"] = AsukaPush.shared.methods()
        #endif
        #if ASUKA_NOTIFY || ASUKA_PUSH
        // platform/notify: UserNotifications, as iOS's (Notify.swift).
        modules.methods["notify"] = AsukaNotifyCenter.shared.methods()
        #endif
    }
}
