#if ASUKA_ICLOUD // an app that imports asuka/sdk/icloud (the CLI's featurePermissions)
import Foundation

// The Mac host's way into asuka/sdk/icloud (ICloud.swift, the iOS host's):
// host/darwin's "icloud" module calls these through purego, on the main
// thread, and each call's answer comes back through the reply it booted
// with, on the main thread too.

/// reply(call, ok, bytes, count): ok 1, the answer; 0, an error's message.
public typealias AsukaICloudReply = @convention(c) (UInt64, Int32, UnsafePointer<UInt8>?, Int) -> Void

private var icloudReply: AsukaICloudReply?

/// Takes the reply.
@_cdecl("asuka_icloud_boot")
public func asuka_icloud_boot(_ reply: AsukaICloudReply) {
    icloudReply = reply
}

/// Keeps the values in a UserDefaults suite of their own, emptied, instead
/// of iCloud's store: for host/darwin's tests, whose process has no
/// entitlement for it.
@_cdecl("asuka_icloud_test_store")
public func asuka_icloud_test_store() {
    let d = UserDefaults(suiteName: AsukaICloud.testSuite)!
    d.removePersistentDomain(forName: AsukaICloud.testSuite)
    AsukaICloud.shared.kvStore = d
}

/// Runs a method of the module (internal/icloudspec's), answering call.
@_cdecl("asuka_icloud_call")
public func asuka_icloud_call(_ call: UInt64, _ method: UnsafePointer<CChar>, _ args: UnsafePointer<UInt8>?, _ count: Int) {
    let name = String(cString: method)
    let data = args.map { Data(bytes: $0, count: count) } ?? Data()
    AsukaICloud.shared.call(name, data) { icloudAnswer(call, $0) }
}

private func icloudAnswer(_ call: UInt64, _ r: Result<Data, Error>) {
    guard let reply = icloudReply else { return }
    switch r {
    case .success(let d):
        d.withUnsafeBytes { reply(call, 1, $0.bindMemory(to: UInt8.self).baseAddress, d.count) }
    case .failure(let e):
        let m = Array(e.localizedDescription.utf8)
        m.withUnsafeBufferPointer { reply(call, 0, $0.baseAddress, m.count) }
    }
}
#endif
