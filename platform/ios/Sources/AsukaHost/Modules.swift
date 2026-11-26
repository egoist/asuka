#if canImport(UIKit)
import UIKit
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A native module's method: the call's bytes, and a reply (once).
public typealias AsukaModuleMethod = (_ args: Data, _ reply: @escaping (Result<Data, Error>) -> Void) -> Void

struct AsukaModuleError: LocalizedError {
    let message: String
    init(_ m: String) { message = m }
    var errorDescription: String? { message }
}

@MainActor extension AsukaRuntime {
    /// Registers one of the host's modules for ui.Call(module, method, ...).
    func registerModule(_ name: String, _ methods: [String: AsukaModuleMethod]) {
        modules[name] = methods
    }

    /// Runs a Call command and answers with a Result event.
    func call(_ root: UInt32, _ args: UnsafeRawBufferPointer) {
        guard args.count >= 12 else { return }
        var r = AsukaReader(args)
        let id = r.u32()
        let module = r.string(), method = r.string()
        let payload = Data(bytes: r.p, count: r.end - r.p)
        if !builtinModules {
            builtinModules = true
            registerBuiltinModules()
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
                // An answer, not an event the root listens for: sent as is.
                self.send(AsukaEvents.result(root, bytes))
            }
        }
        guard let m = modules[module]?[method] else {
            reply(.failure(AsukaModuleError("no module method \(module).\(method)")))
            return
        }
        m(payload, reply)
    }

    // The host's modules: what the platform SDK's packages over
    // platform/host call by name (notify, push, iCloud, the photo
    // library...), and the core's own (web, share). What a std package
    // implements in its own Swift (platform/secure, platform/device,
    // platform/location...) isn't here: it comes with the package, in only
    // the apps that import it.

    func registerBuiltinModules() {
        // An app's module of the same name stays the app's.
        let apps = modules
        defer { for (k, v) in apps { modules[k] = v } }
        #if ASUKA_LIBRARY
        modules["library"] = AsukaLibrary.methods()
        #endif
        modules["share"] = AsukaShare.methods(self)
        modules["view"] = AsukaCapture.methods(self)
        // fs.readAsset: an asset's bytes, read off the main thread.
        modules["assets"] = ["read": { args, reply in
            let name = String(decoding: args, as: UTF8.self)
            DispatchQueue.global(qos: .userInitiated).async {
                guard let url = AsukaAssets.url(name), url.isFileURL, let data = try? Data(contentsOf: url) else {
                    return reply(.failure(AsukaModuleError("no asset \(name)")))
                }
                reply(.success(data))
            }
        }]
        // platform/recent, platform/login and platform/dock: a desktop's. iOS's
        // Files keeps its own recent files, an app doesn't start at login,
        // and a phone has no Dock or taskbar.
        modules["recent"] = [
            "add": { _, reply in reply(.success(Data())) },
            "clear": { _, reply in reply(.success(Data())) },
            "list": { _, reply in reply(.success(Data("[]".utf8))) },
        ]
        modules["login"] = [
            "set": { _, reply in reply(.failure(AsukaModuleError("not available: an iOS app doesn't start at login"))) },
            "enabled": { _, reply in reply(.success(Data([0]))) },
            "openedAtLogin": { _, reply in reply(.success(Data([0]))) },
        ]
        modules["dock"] = [
            "progress": { _, reply in reply(.success(Data())) },
            "attention": { _, reply in reply(.success(Data([0, 0, 0, 0]))) },
            "cancelAttention": { _, reply in reply(.success(Data())) },
        ]
        #if ASUKA_ICLOUD
        // platform/icloud: the key-value store and the app's containers.
        modules["icloud"] = Dictionary(uniqueKeysWithValues: AsukaICloud.methods.map { name in
            (name, { args, reply in AsukaICloud.shared.call(name, args, reply) } as AsukaModuleMethod)
        })
        #endif
        #if ASUKA_WIDGET
        modules["widget"] = AsukaWidgetData.methods()
        #endif
        #if canImport(ActivityKit) && ASUKA_LIVEACTIVITY
        modules["liveActivity"] = AsukaLiveActivities.shared.methods()
        #endif
        #if ASUKA_CONTACTS
        modules["contacts"] = AsukaContacts.shared.methods(self)
        #endif
        #if ASUKA_CALENDAR
        modules["calendar"] = AsukaCalendar.shared.methods(self)
        #endif
        #if ASUKA_PERIODIC
        modules["periodic"] = AsukaPeriodic.shared.methods()
        #endif
        #if ASUKA_PUSH
        modules["push"] = AsukaPush.shared.methods()
        #endif
        #if ASUKA_NOTIFICATION_SERVICE
        modules["push", default: [:]].merge(AsukaRewrites.shared.methods(stop: modules["push"]?["stop"])) { _, new in new }
        #endif
        #if ASUKA_COMPOSE
        modules["compose"] = AsukaCompose.shared.methods(self)
        #endif
        #if ASUKA_WEBVIEW
        modules["web"] = AsukaWebView.methods()
        #endif
        #if ASUKA_NOTIFY || ASUKA_PUSH || ASUKA_NOTIFICATION_SERVICE
        modules["notify"] = AsukaNotifyCenter.shared.methods()
        #endif
        modules["background"] = AsukaBackground.shared.methods()
        #if ASUKA_PHOTOLIBRARY
        modules["photoLibrary"] = AsukaPhotos.methods()
        #endif
    }
}

#endif
