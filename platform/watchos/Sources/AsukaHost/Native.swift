// Native views on the watch (docs/native.md): a package's typed view
// (//asuka:view) built for watchOS gives a SwiftUI view, which the host
// draws at the core's frame and measures as SwiftUI lays it out.

import Foundation
import SwiftUI
#if canImport(AsukaOps)
import AsukaOps
#endif

/// A value of a native view's prop or event.
public enum AsukaNativeValue {
    case bool(Bool), int(Int64), float(Double), string(String)
    /// A typed view's prop or event (//asuka:view): its values, encoded.
    case bytes(Data)

    public var bool: Bool { if case .bool(let v) = self { return v }; return false }
    public var int: Int64 {
        switch self { case .int(let v): return v; case .float(let v): return Int64(v); default: return 0 }
    }
    public var float: Double {
        switch self { case .float(let v): return v; case .int(let v): return Double(v); default: return 0 }
    }
    public var string: String { if case .string(let v) = self { return v }; return "" }
    public var bytes: Data { if case .bytes(let v) = self { return v }; return Data() }
}

/// A view a package registers for ui.Native: on the watch, a SwiftUI view.
@MainActor public protocol AsukaNativeView: AnyObject {
    /// The SwiftUI view shown, at the frame the core gives it.
    var view: AnyView { get }
    /// Sets one of its props, by name, as it's set or changes.
    func set(_ name: String, _ value: AsukaNativeValue)
}

/// What a native view is given: a way to send its events.
@MainActor public final class AsukaNativeContext {
    public let id: UInt32
    init(id: UInt32) { self.id = id }

    /// Sends an event to the view's ui.NativeProps.On handler.
    public func emit(_ name: String, _ values: [String: AsukaNativeValue] = [:]) {
        var w = AsukaWriter()
        w.string(name)
        w.u32(UInt32(values.count))
        for (k, v) in values {
            w.string(k)
            switch v {
            case .bool(let b): w.u8(1); w.u8(b ? 1 : 0)
            case .int(let i): w.u8(2); w.u64(UInt64(bitPattern: i))
            case .float(let f): w.u8(3); w.f64(f)
            case .string(let s): w.u8(4); w.string(s)
            case .bytes(let d): w.u8(5); w.u32(UInt32(d.count)); w.bytes.append(contentsOf: d)
            }
        }
        let id = self.id, b = w.bytes
        AsukaRuntime.shared.emit(id, .native) { AsukaEvents.native(id, b) }
    }
}

@MainActor extension AsukaRuntime {
    /// A Native node's props: its type makes its view; the others are the
    /// view's.
    func setNativeProp(_ n: AsukaNode, _ prop: UInt16, _ v: AsukaValue) {
        switch prop {
        case AsukaProp.Native.type:
            guard let nv = AsukaNative.makeView(v.string, AsukaNativeContext(id: n.id)) else {
                print("asuka: no native view \"\(v.string)\" for the watch: its package's Package.swift doesn't list .watchOS")
                return
            }
            n.native = nv
        case AsukaProp.Native.prop:
            guard case .bytes(let b) = v, b.count >= 5 else { return }
            var r = AsukaReader(b)
            let name = r.string()
            let value: AsukaNativeValue
            switch r.u8() {
            case 1: value = .bool(r.u8() != 0)
            case 2: value = .int(Int64(bitPattern: r.u64()))
            case 3: value = .float(r.f64())
            case 5: value = .bytes(Data(r.bytes()))
            default: value = .string(r.string())
            }
            n.native?.set(name, value)
        default:
            break
        }
    }
}
