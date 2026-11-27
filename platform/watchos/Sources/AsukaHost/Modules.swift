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
