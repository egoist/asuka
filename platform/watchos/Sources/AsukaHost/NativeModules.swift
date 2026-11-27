// Typed native modules (//asuka:native) and views (//asuka:view): what
// the Swift that asuka native generates for a package calls, and what the
// host's transport calls (the core's calls, from any thread). The same file
// is in the iOS host, the Mac's runtime (platform/macos) and the watch's
// (platform/watchos), which tests keep equal.
//
// The wire format is core/src/native's: values in declaration order,
// little-endian.

import Foundation
import UniformTypeIdentifiers
#if os(watchOS)
import SwiftUI
/// The platform's view: UIView on iOS, NSView on the Mac, a SwiftUI view
/// on the watch (whose host is SwiftUI's).
public typealias AsukaPlatformView = AnyView
#elseif canImport(UIKit)
import UIKit
/// The platform's view: UIView on iOS, NSView on the Mac.
public typealias AsukaPlatformView = UIView
#else
import AppKit
/// The platform's view: UIView on iOS, NSView on the Mac.
public typealias AsukaPlatformView = NSView
#endif

#if os(watchOS)
/// The platform's color: UIColor on iOS, NSColor on the Mac, SwiftUI's
/// Color on the watch.
public typealias AsukaPlatformColor = Color
#elseif canImport(UIKit)
/// The platform's color: UIColor on iOS, NSColor on the Mac.
public typealias AsukaPlatformColor = UIColor
#else
/// The platform's color: UIColor on iOS, NSColor on the Mac.
public typealias AsukaPlatformColor = NSColor
#endif

#if os(watchOS)
// The watch has no controllers to present over: a package's Swift
// presents with SwiftUI's own (a sheet of the app's).
#elseif canImport(UIKit)
/// Where a package's Swift presents a controller of its own (a picker,
/// the system's camera): the frontmost controller of the app's key
/// window.
@MainActor public enum AsukaPresenting {
    public static var top: UIViewController? {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let window = scenes.flatMap(\.windows).first { $0.isKeyWindow } ?? scenes.first?.windows.first
        var c = window?.rootViewController
        while let p = c?.presentedViewController, !p.isBeingDismissed { c = p }
        return c
    }
}
#else
/// Where a package's Swift shows a sheet of its own: the app's key
/// window, else its main one.
@MainActor public enum AsukaPresenting {
    public static var window: NSWindow? { NSApp.keyWindow ?? NSApp.mainWindow ?? NSApp.windows.first { $0.isVisible } }
}
#endif

/// The app's assets, for a package's Swift: a source as the hosts take
/// one (an http(s) or file URL, an absolute path, or a file in the app's
/// assets/) as a URL.
public enum AsukaAssets {
    /// The app's assets/: in the app's bundle (or the framework holding an
    /// embedded app's code), else ASUKA_ASSETS, else assets beside the
    /// working directory (a Mac app run from its project).
    public static let dir: URL? = {
        for b in [Bundle.main] + Bundle.allFrameworks {
            if let u = b.resourceURL?.appendingPathComponent("assets"), FileManager.default.fileExists(atPath: u.path) { return u }
        }
        if let d = ProcessInfo.processInfo.environment["ASUKA_ASSETS"], !d.isEmpty { return URL(fileURLWithPath: d) }
        #if canImport(UIKit) || os(watchOS)
        return Bundle.main.resourceURL?.appendingPathComponent("assets")
        #else
        return URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("assets")
        #endif
    }()

    public static func url(_ source: String) -> URL? {
        if source.isEmpty { return nil }
        if source.hasPrefix("http://") || source.hasPrefix("https://") || source.hasPrefix("file://") || source.hasPrefix("data:") { return URL(string: source) }
        if source.hasPrefix("/") { return URL(fileURLWithPath: source) }
        let rel = source.hasPrefix("./") ? String(source.dropFirst(2)) : source
        return dir?.appendingPathComponent(rel)
    }
}

/// Asuka's colors in a package's Swift: a Color crosses as an Int (the
/// core's encoding), which color makes the platform's, a semantic one
/// following light and dark.
public enum AsukaColors {
    public static func color(_ v: Int) -> AsukaPlatformColor? {
        let u = UInt64(bitPattern: Int64(v))
        let lo = UInt32(truncatingIfNeeded: u)
        switch u >> 32 {
        case 2:
            #if os(watchOS)
            return Color(.sRGB, red: Double((lo >> 24) & 0xff) / 255, green: Double((lo >> 16) & 0xff) / 255, blue: Double((lo >> 8) & 0xff) / 255, opacity: Double(lo & 0xff) / 255)
            #elseif canImport(UIKit)
            return UIColor(red: CGFloat((lo >> 24) & 0xff) / 255, green: CGFloat((lo >> 16) & 0xff) / 255, blue: CGFloat((lo >> 8) & 0xff) / 255, alpha: CGFloat(lo & 0xff) / 255)
            #else
            return NSColor(srgbRed: CGFloat((lo >> 24) & 0xff) / 255, green: CGFloat((lo >> 16) & 0xff) / 255, blue: CGFloat((lo >> 8) & 0xff) / 255, alpha: CGFloat(lo & 0xff) / 255)
            #endif
        case 1:
            guard let c = semantic(Int(lo & 0xffff)) else { return nil }
            // Its opacity is in bits 24-31, as transparency.
            let a = 255 - CGFloat((lo >> 24) & 0xff)
            #if os(watchOS)
            return a == 255 ? c : c.opacity(a / 255)
            #else
            return a == 255 ? c : c.withAlphaComponent(a / 255)
            #endif
        default:
            return nil
        }
    }

    static func semantic(_ i: Int) -> AsukaPlatformColor? {
        #if os(watchOS)
        // The watch's: light text on black.
        let t: [AsukaPlatformColor?] = [nil, .primary, .secondary, Color.white.opacity(0.4), .black, Color(white: 0.11),
            .black, Color.white.opacity(0.2), .accentColor, .red, .orange, .yellow, .green, .blue,
            .purple, .pink, .gray, .clear, Color(white: 0.11), .white]
        #elseif canImport(UIKit)
        let t: [AsukaPlatformColor?] = [nil, .label, .secondaryLabel, .tertiaryLabel, .systemBackground, .secondarySystemBackground,
            .systemGroupedBackground, .separator, .tintColor, .systemRed, .systemOrange, .systemYellow, .systemGreen, .systemBlue,
            .systemPurple, .systemPink, .systemGray, .clear, .secondarySystemGroupedBackground, .white]
        #else
        let t: [AsukaPlatformColor?] = [nil, .labelColor, .secondaryLabelColor, .tertiaryLabelColor, .windowBackgroundColor, .controlBackgroundColor,
            .windowBackgroundColor, .separatorColor, .controlAccentColor, .systemRed, .systemOrange, .systemYellow, .systemGreen, .systemBlue,
            .systemPurple, .systemPink, .systemGray, .clear, .controlBackgroundColor, .white]
        #endif
        return i >= 0 && i < t.count ? t[i] : nil
    }
}

// Where the host has views: iOS (UIKit), the watch (SwiftUI) and the Mac's
// runtime (which has no AsukaOps); not the iOS host's package built for
// the Mac (swift test, for its decoder's tests: AsukaOps is a module of
// its own there).
#if canImport(UIKit) || !canImport(AsukaOps)

/// Encodes values (core/src/native's writer).
public struct AsukaNativeWriter {
    public private(set) var bytes: [UInt8]

    public init() {
        bytes = []
        bytes.reserveCapacity(64)
    }

    /// An answer's writer: its status, ok, first.
    public static func ok() -> AsukaNativeWriter {
        var w = AsukaNativeWriter()
        w.bytes.append(0)
        return w
    }

    @inline(__always) mutating func put<T: FixedWidthInteger>(_ v: T) {
        withUnsafeBytes(of: v.littleEndian) { bytes.append(contentsOf: $0) }
    }

    public mutating func bool(_ v: Bool) { bytes.append(v ? 1 : 0) }
    public mutating func int8(_ v: Int8) { bytes.append(UInt8(bitPattern: v)) }
    public mutating func int16(_ v: Int16) { put(v) }
    public mutating func int32(_ v: Int32) { put(v) }
    public mutating func int64(_ v: Int64) { put(v) }
    public mutating func int(_ v: Int) { put(Int64(v)) }
    public mutating func uint8(_ v: UInt8) { bytes.append(v) }
    public mutating func uint16(_ v: UInt16) { put(v) }
    public mutating func uint32(_ v: UInt32) { put(v) }
    public mutating func uint64(_ v: UInt64) { put(v) }
    public mutating func uint(_ v: UInt) { put(UInt64(v)) }
    public mutating func float32(_ v: Float) { put(v.bitPattern) }
    public mutating func float64(_ v: Double) { put(v.bitPattern) }

    public mutating func string(_ v: String) {
        var v = v
        v.withUTF8 { b in
            put(UInt32(b.count))
            bytes.append(contentsOf: b)
        }
    }

    public mutating func data(_ v: Data) {
        put(UInt32(v.count))
        bytes.append(contentsOf: v)
    }

    /// A time: seconds and nanoseconds since 1970.
    public mutating func time(_ v: Date) {
        let t = v.timeIntervalSince1970
        var s = t.rounded(.down)
        var ns = ((t - s) * 1e9).rounded()
        if ns >= 1e9 {
            s += 1
            ns -= 1e9
        }
        put(Int64(s))
        put(Int32(ns))
    }

    /// A duration, in seconds (Go's: nanoseconds).
    public mutating func duration(_ v: TimeInterval) { put(Int64((v * 1e9).rounded())) }
    /// A Decimal (std/decimal's): its coefficient, without trailing zeros,
    /// and its exponent. One of more digits than an Int64 holds is rounded
    /// half to even to 18 significant digits; NaN is 0.
    public mutating func decimal(_ v: Decimal) {
        let (c, e) = AsukaDecimalParts.of(v)
        put(c)
        put(e)
    }
    public mutating func len(_ n: Int) { put(UInt32(n)) }
    public mutating func present(_ v: Bool) { bool(v) }
}

/// A Decimal's parts as std/decimal keeps them: its coefficient without
/// trailing zeros, and its exponent.
enum AsukaDecimalParts {
    static func of(_ v: Decimal) -> (Int64, Int64) {
        if v.isNaN || v.isZero { return (0, 0) }
        // Its plain digits ("-123.4500"): those, and where the point is.
        var text = Substring(NSDecimalNumber(decimal: v).stringValue)
        let negative = text.hasPrefix("-")
        if negative { text = text.dropFirst() }
        var digits = [UInt8]()
        var exponent: Int64 = 0
        var point = false
        for ch in text.utf8 {
            if ch == UInt8(ascii: ".") {
                point = true
            } else if ch >= UInt8(ascii: "0") && ch <= UInt8(ascii: "9") {
                if !(digits.isEmpty && ch == UInt8(ascii: "0")) { digits.append(ch - UInt8(ascii: "0")) }
                if point { exponent -= 1 }
            }
        }
        // More than 18 significant digits: rounded half to even.
        if digits.count > 18 {
            let rest = digits[18...]
            exponent += Int64(rest.count)
            var kept = Array(digits[..<18])
            let first = rest.first!
            let more = rest.dropFirst().contains { $0 != 0 }
            if first > 5 || first == 5 && (more || kept.last! % 2 == 1) {
                var i = kept.count - 1
                while i >= 0 && kept[i] == 9 {
                    kept[i] = 0
                    i -= 1
                }
                if i >= 0 { kept[i] += 1 } else { kept.insert(1, at: 0) }
            }
            digits = kept
        }
        while let last = digits.last, last == 0 {
            digits.removeLast()
            exponent += 1
        }
        var c: Int64 = 0
        for d in digits { c = c * 10 + Int64(d) }
        return (negative ? -c : c, exponent)
    }
}

/// One of the app's views a method was passed (an Asuka Ref): the id its
/// host knows it by.
public struct AsukaViewRef: Sendable, Hashable {
    public let id: UInt64
    public init(id: UInt64) { self.id = id }

    /// The view while the app shows it (UIView on iOS, NSView on the Mac),
    /// else nil: a WebView's is its WKWebView.
    @MainActor public var view: AsukaPlatformView? { id == 0 ? nil : AsukaNative.views(id) }
}

/// Decodes values (core/src/native's reader). After a short read every
/// read returns its zero value, and failed is true.
public struct AsukaNativeReader {
    let bytes: [UInt8]
    var p = 0
    public private(set) var failed = false

    public init(_ bytes: [UInt8]) { self.bytes = bytes }
    public init(_ data: Data) { self.bytes = [UInt8](data) }

    @inline(__always) mutating func take(_ n: Int) -> ArraySlice<UInt8>? {
        guard !failed, n >= 0, bytes.count - p >= n else {
            failed = true
            return nil
        }
        defer { p += n }
        return bytes[p..<p + n]
    }

    @inline(__always) mutating func get<T: FixedWidthInteger>(_: T.Type) -> T {
        guard let s = take(MemoryLayout<T>.size) else { return 0 }
        var v: T = 0
        withUnsafeMutableBytes(of: &v) { $0.copyBytes(from: s) }
        return T(littleEndian: v)
    }

    public mutating func bool() -> Bool { get(UInt8.self) != 0 }
    public mutating func int8() -> Int8 { Int8(bitPattern: get(UInt8.self)) }
    public mutating func int16() -> Int16 { get(Int16.self) }
    public mutating func int32() -> Int32 { get(Int32.self) }
    public mutating func int64() -> Int64 { get(Int64.self) }
    public mutating func int() -> Int { Int(get(Int64.self)) }
    public mutating func uint8() -> UInt8 { get(UInt8.self) }
    public mutating func uint16() -> UInt16 { get(UInt16.self) }
    public mutating func uint32() -> UInt32 { get(UInt32.self) }
    public mutating func uint64() -> UInt64 { get(UInt64.self) }
    public mutating func uint() -> UInt { UInt(get(UInt64.self)) }
    public mutating func viewRef() -> AsukaViewRef { AsukaViewRef(id: get(UInt64.self)) }
    public mutating func float32() -> Float { Float(bitPattern: get(UInt32.self)) }
    public mutating func float64() -> Double { Double(bitPattern: get(UInt64.self)) }

    public mutating func string() -> String {
        let n = Int(get(UInt32.self))
        guard let s = take(n) else { return "" }
        return String(decoding: s, as: UTF8.self)
    }

    public mutating func data() -> Data {
        let n = Int(get(UInt32.self))
        guard let s = take(n) else { return Data() }
        return Data(s)
    }

    public mutating func time() -> Date {
        let s = get(Int64.self)
        let ns = get(Int32.self)
        return Date(timeIntervalSince1970: Double(s) + Double(ns) / 1e9)
    }

    public mutating func duration() -> TimeInterval { Double(get(Int64.self)) / 1e9 }
    /// A Decimal (std/decimal's): coefficient × 10^exponent.
    public mutating func decimal() -> Decimal {
        let c = get(Int64.self), e = get(Int64.self)
        guard c != 0, let exp = Int(exactly: e) else { return 0 }
        return Decimal(sign: c < 0 ? .minus : .plus, exponent: exp, significand: Decimal(c.magnitude))
    }

    /// A slice's or a map's length, bounded by what's left.
    public mutating func len() -> Int {
        let n = Int(get(UInt32.self))
        if n > bytes.count - p {
            failed = true
            return 0
        }
        return n
    }

    public mutating func present() -> Bool { bool() }
    /// A callback's id (0: none).
    public mutating func callback() -> UInt64 { get(UInt64.self) }

    /// Throws when a read went past what's there.
    public func check() throws {
        if failed { throw AsukaNativeError.malformed }
    }
}

/// An error a package declares (its enum ...: Error): the app gets that value.
public protocol AsukaDeclaredError: Error {
    var asukaIndex: UInt32 { get }
}

/// The runtime's own errors.
public enum AsukaNativeError: Error, LocalizedError {
    /// What the platform doesn't have: Go gets errors.ErrUnsupported.
    case unsupported(String)
    case malformed
    case closed

    public var errorDescription: String? {
        switch self {
        case .unsupported(let m): return m
        case .malformed: return "asuka: a malformed call"
        case .closed: return "asuka: the object is closed"
        }
    }
}

/// A typed view's custom size (on the Mac; on iOS its view's
/// sizeThatFits is it).
public protocol AsukaSizing {
    @MainActor func sizeThatFits(_ size: CGSize) -> CGSize
}

/// A typed view that frees what it holds as the core destroys it.
public protocol AsukaDestroying {
    @MainActor func destroy()
}

/// A Go package's native code, as asuka native generates it: the host
/// finds it by its Objective-C name (the module's Swift, in Go).
public protocol AsukaModuleProvider: AnyObject {
    static func make(_ iface: UInt32) throws -> AnyObject
    static func call(_ object: AnyObject, _ iface: UInt32, _ method: UInt32, _ r: inout AsukaNativeReader, _ w: inout AsukaNativeWriter) throws
    static func start(_ object: AnyObject, _ iface: UInt32, _ method: UInt32, _ input: AsukaNativeReader, _ done: @escaping (Result<AsukaNativeWriter, Error>) -> Void) -> Task<Void, Never>?
    @MainActor static func makeView(_ name: String, _ ctx: AsukaNativeContext) -> AsukaNativeView?
}

/// The typed modules' runtime: the objects Go holds, by handle; the
/// calls it started; and the way back to Go (done, invoke), which the
/// host's transport sets.
public enum AsukaNative {
    struct Entry {
        let object: AnyObject
        let provider: AsukaModuleProvider.Type
        let iface: UInt32
    }

    static let lock = NSLock()
    static var objects: [UInt64: Entry] = [:]
    static var nextHandle: UInt64 = 0
    static var tasks: [UInt64: Task<Void, Never>] = [:]
    static var finished: Set<UInt64> = []
    static var providers: [String: AsukaModuleProvider.Type] = [:]

    /// Answers a started call (the host's: to Go).
    public static var done: (UInt64, [UInt8]) -> Void = { _, _ in }
    /// Calls a Go callback: its answer, nil for a notification (the
    /// host's: to Go).
    public static var invoke: (UInt64, [UInt8]) -> [UInt8]? = { _, _ in nil }
    /// The view a host shows by an id (the host's: its views' table).
    @MainActor public static var views: (UInt64) -> AsukaPlatformView? = { _ in nil }

    /// The provider of a class's name, looked up once: its Objective-C
    /// name (AsukaModule_…), or the package's path (platform/network, as the
    /// C core names it), which names that class (internal/nativegen's
    /// SwiftClass).
    public static func provider(_ name: String) -> AsukaModuleProvider.Type? {
        lock.lock()
        defer { lock.unlock() }
        if let p = providers[name] { return p }
        let cls = name.hasPrefix("AsukaModule_") ? name : swiftClass(name)
        guard let p = NSClassFromString(cls) as? AsukaModuleProvider.Type else { return nil }
        providers[name] = p
        return p
    }

    /// A package path's Swift class: AsukaModule_ and its letters and
    /// digits, the rest _.
    static func swiftClass(_ path: String) -> String {
        "AsukaModule_" + String(path.unicodeScalars.map { s -> Character in
            s.isASCII && (CharacterSet.letters.contains(s) || CharacterSet.decimalDigits.contains(s)) ? Character(s) : "_"
        })
    }

    /// Keeps an object for Go: its handle.
    public static func retain(_ o: AnyObject, _ p: AsukaModuleProvider.Type, _ iface: UInt32) -> UInt64 {
        lock.lock()
        defer { lock.unlock() }
        nextHandle += 1
        objects[nextHandle] = Entry(object: o, provider: p, iface: iface)
        return nextHandle
    }

    /// The object of a handle Go passed (nil for 0, or one let go).
    public static func object(_ h: UInt64) -> AnyObject? {
        lock.lock()
        defer { lock.unlock() }
        return objects[h]?.object
    }

    static func entry(_ h: UInt64) -> Entry? {
        lock.lock()
        defer { lock.unlock() }
        return objects[h]
    }

    // What Go calls, through the host's transport.

    /// Makes an object of a module's interface.
    public static func new(_ cls: String, _ iface: UInt32) -> [UInt8] {
        guard let p = provider(cls) else {
            return answer(AsukaNativeError.unsupported("no Swift \(cls): build the app with its packages' native code"))
        }
        do {
            let o = try p.make(iface)
            var w = AsukaNativeWriter.ok()
            w.uint64(retain(o, p, iface))
            return w.bytes
        } catch {
            return answer(error)
        }
    }

    /// Calls an object's method, on this thread.
    public static func call(_ h: UInt64, _ method: UInt32, _ input: [UInt8]) -> [UInt8] {
        guard let e = entry(h) else { return answer(AsukaNativeError.closed) }
        var r = AsukaNativeReader(input)
        var w = AsukaNativeWriter.ok()
        do {
            try e.provider.call(e.object, e.iface, method, &r, &w)
            return w.bytes
        } catch {
            return answer(error)
        }
    }

    /// Starts a method that takes a context; done answers it.
    public static func start(_ h: UInt64, _ method: UInt32, _ input: [UInt8], _ call: UInt64) {
        guard let e = entry(h) else {
            done(call, answer(AsukaNativeError.closed))
            return
        }
        let task = e.provider.start(e.object, e.iface, method, AsukaNativeReader(input)) { finish(call, $0) }
        lock.lock()
        if finished.remove(call) == nil, let task { tasks[call] = task }
        lock.unlock()
    }

    static func finish(_ call: UInt64, _ result: Result<AsukaNativeWriter, Error>) {
        lock.lock()
        if tasks.removeValue(forKey: call) == nil { finished.insert(call) }
        lock.unlock()
        switch result {
        case .success(let w): done(call, w.bytes)
        case .failure(let e): done(call, answer(e))
        }
    }

    /// Cancels a started call (its task), which still answers.
    public static func cancel(_ call: UInt64) {
        lock.lock()
        let t = tasks[call]
        lock.unlock()
        t?.cancel()
    }

    /// Lets an object go.
    public static func release(_ h: UInt64) {
        lock.lock()
        let e = objects.removeValue(forKey: h)
        lock.unlock()
        if let e, !Thread.isMainThread {
            // Its last reference goes on the main thread, where a view or
            // a main-thread object expects to be freed.
            DispatchQueue.main.async { _ = e }
        }
    }

    // What the generated Swift calls.

    /// Calls a Go callback that answers nothing (it runs on Go's main thread).
    public static func notify(_ id: UInt64, _ w: AsukaNativeWriter) {
        if id != 0 { _ = invoke(id, w.bytes) }
    }

    /// Calls a Go callback and reads its answer.
    public static func ask(_ id: UInt64, _ w: AsukaNativeWriter) -> AsukaNativeReader {
        AsukaNativeReader(id == 0 ? [] : (invoke(id, w.bytes) ?? []))
    }

    final class Box<T> { var v: Result<T, Error>? }

    /// Runs body on the main thread: now if this is it, else waiting for it.
    public static func onMain<T>(_ body: @MainActor () throws -> T) throws -> T {
        let box = Box<T>()
        if Thread.isMainThread {
            MainActor.assumeIsolated { box.v = Result { try body() } }
        } else {
            DispatchQueue.main.sync { MainActor.assumeIsolated { box.v = Result { try body() } } }
        }
        return try box.v!.get()
    }

    /// An error's answer: declared (its index), unsupported, canceled, or
    /// its domain, code and message.
    public static func answer(_ e: Error) -> [UInt8] {
        var w = AsukaNativeWriter()
        if let d = e as? AsukaDeclaredError {
            w.uint8(1)
            w.uint32(d.asukaIndex)
        } else if case AsukaNativeError.unsupported(let m) = e {
            w.uint8(3)
            w.string(m)
        } else if e is CancellationError {
            w.uint8(4)
        } else {
            let ns = e as NSError
            w.uint8(2)
            w.string(ns.domain)
            w.int64(Int64(ns.code))
            w.string((e as? LocalizedError)?.errorDescription ?? ns.localizedDescription)
        }
        return w.bytes
    }

    /// A typed view of a kind (its module's class # its name).
    @MainActor public static func makeView(_ kind: String, _ ctx: AsukaNativeContext) -> AsukaNativeView? {
        guard let i = kind.firstIndex(of: "#"), let p = provider(String(kind[..<i])) else { return nil }
        return p.makeView(String(kind[kind.index(after: i)...]), ctx)
    }

    /// Puts an answer where Go's transport reads it: into out when it fits
    /// (its length returned), else into memory of its own (*big, which
    /// asukaNativeFree frees).
    public static func write(_ a: [UInt8], _ out: UnsafeMutablePointer<UInt8>?, _ cap: Int, _ big: UnsafeMutablePointer<UnsafeMutablePointer<UInt8>?>?) -> Int {
        a.withUnsafeBufferPointer { src in
            if a.count <= cap, let out {
                out.update(from: src.baseAddress!, count: a.count)
            } else {
                let m = malloc(max(a.count, 1))!.assumingMemoryBound(to: UInt8.self)
                m.update(from: src.baseAddress!, count: a.count)
                big?.pointee = m
            }
        }
        return a.count
    }
}

// What a package's Swift presents over, or attaches a sheet to.
extension AsukaNative {
    #if os(watchOS)
    // The watch's app has one screen, SwiftUI's: nothing to present over.
    #elseif canImport(UIKit)
    /// The app's window: its foreground scene's key window.
    @MainActor public static var window: UIWindow? {
        let scenes = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }
        let active = scenes.filter { $0.activationState == .foregroundActive } + scenes
        for s in active {
            if let w = s.windows.first(where: { $0.isKeyWindow }) ?? s.windows.first { return w }
        }
        return nil
    }

    /// The view controller to present over: the window's top one (what's
    /// presented over its root, and over that).
    @MainActor public static var presenter: UIViewController? {
        var vc = window?.rootViewController
        while let p = vc?.presentedViewController, !p.isBeingDismissed { vc = p }
        return vc
    }
    #else
    /// The app's window the user works in: the key one, else the main
    /// one, else the first that's shown.
    @MainActor public static var window: NSWindow? {
        NSApp.keyWindow ?? NSApp.mainWindow ?? NSApp.windows.first(where: { $0.isVisible })
    }
    #endif
}

// The files packages hand the system (platform/preview, platform/print).
extension AsukaNative {
    /// The app's assets: the bundle's Resources/assets (an embedded app's
    /// framework's), else $ASUKA_ASSETS (asuka dev on the Mac), else
    /// assets in the working directory.
    public static var assets: URL {
        for b in [Bundle(for: AsukaNativeBundle.self), Bundle.main] {
            if let u = b.resourceURL?.appendingPathComponent("assets"), FileManager.default.fileExists(atPath: u.path) { return u }
        }
        if let d = ProcessInfo.processInfo.environment["ASUKA_ASSETS"], !d.isEmpty { return URL(fileURLWithPath: d, isDirectory: true) }
        return URL(fileURLWithPath: "assets", isDirectory: true)
    }

    /// The local file a source names: a path, a file:// URL, an asset's
    /// path, or an http(s) URL, downloaded to the caches first
    /// (asuka-files/, under its name, with the extension of type, or the
    /// server's, if it has none).
    public static func localFile(_ source: String, type: String = "") async throws -> URL {
        if source.hasPrefix("http://") || source.hasPrefix("https://"), let url = URL(string: source) {
            return try await download(url, type)
        }
        var path = source
        if path.hasPrefix("file://") { path = URL(string: path)?.path ?? path }
        if !path.hasPrefix("/") { path = assets.appendingPathComponent(path).path }
        guard FileManager.default.fileExists(atPath: path) else {
            throw NSError(domain: "asuka", code: 0, userInfo: [NSLocalizedDescriptionKey: "no file at \(source)"])
        }
        return URL(fileURLWithPath: path)
    }

    private static func download(_ url: URL, _ type: String) async throws -> URL {
        // The file's moved out before the task's handler returns, which
        // deletes it.
        let (tmp, response): (URL, URLResponse) = try await withCheckedThrowingContinuation { c in
            URLSession.shared.downloadTask(with: url) { tmp, response, error in
                guard let tmp, let response else {
                    c.resume(throwing: error ?? NSError(domain: "asuka", code: 0, userInfo: [NSLocalizedDescriptionKey: "couldn't download \(url)"]))
                    return
                }
                let kept = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
                do {
                    try FileManager.default.moveItem(at: tmp, to: kept)
                    c.resume(returning: (kept, response))
                } catch {
                    c.resume(throwing: error)
                }
            }.resume()
        }
        if let h = response as? HTTPURLResponse, !(200..<300).contains(h.statusCode) {
            throw NSError(domain: "asuka", code: h.statusCode, userInfo: [NSLocalizedDescriptionKey: "couldn't download \(url): HTTP \(h.statusCode)"])
        }
        var name = response.suggestedFilename ?? url.lastPathComponent
        if name.isEmpty || name == "/" { name = "file" }
        let mime = type.isEmpty ? (response.mimeType ?? "") : type
        if (name as NSString).pathExtension.isEmpty, let ext = UTType(mimeType: mime)?.preferredFilenameExtension {
            name += "." + ext
        }
        let fm = FileManager.default
        let dir = fm.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("asuka-files/" + UUID().uuidString, isDirectory: true)
        try fm.createDirectory(at: dir, withIntermediateDirectories: true)
        let dst = dir.appendingPathComponent(name.replacingOccurrences(of: "/", with: "_"))
        try fm.moveItem(at: tmp, to: dst)
        return dst
    }

    /// The app's name, as the home screen or the Dock has it.
    public static var appName: String {
        Bundle.main.object(forInfoDictionaryKey: "CFBundleDisplayName") as? String
            ?? Bundle.main.object(forInfoDictionaryKey: "CFBundleName") as? String ?? ProcessInfo.processInfo.processName
    }
}

/// The bundle the runtime is in (an embedded app's framework).
final class AsukaNativeBundle {}

#endif
