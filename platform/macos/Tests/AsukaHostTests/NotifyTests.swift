import XCTest
import UserNotifications
@testable import AsukaHost

// A push's message, APNs's answer and the queues, and the presentations,
// categories and content the notify module makes.
@MainActor final class NotifyTests: XCTestCase {
    private struct Message: Decodable {
        var id, title, body, subtitle, category, thread: String
        var badge: Int
        var data: [String: String]
    }

    private func message(_ info: [AnyHashable: Any], id: String = "") throws -> Message {
        try JSONDecoder().decode(Message.self, from: AsukaPush.message(info, id: id))
    }

    func testPushMessage() throws {
        let payload: [AnyHashable: Any] = [
            "aps": ["alert": ["title": "T", "body": "B", "subtitle": "S"], "badge": 3, "thread-id": "th", "category": "c"],
            "url": "https://x", "n": 2, "o": ["a": 1], "yes": true,
        ]
        let m = try message(payload, id: "id1")
        XCTAssertEqual(m.id, "id1")
        XCTAssertEqual([m.title, m.body, m.subtitle, m.category, m.thread], ["T", "B", "S", "c", "th"])
        XCTAssertEqual(m.badge, 3)
        XCTAssertEqual(m.data, ["url": "https://x", "n": "2", "o": #"{"a":1}"#, "yes": "true"])

        // An alert that's a string is the body.
        XCTAssertEqual(try message(["aps": ["alert": "just text"]]).body, "just text")
        // What the notification shows comes first.
        let shown = try JSONDecoder().decode(Message.self, from: AsukaPush.message(["aps": ["alert": ["title": "A", "body": "B"]]], id: "", title: "X", body: "Y"))
        XCTAssertEqual([shown.title, shown.body], ["X", "Y"])
    }

    /// Calls a module's method, its answer as it came (synchronously
    /// answered methods only).
    private func call(_ m: [String: AsukaModuleMethod], _ name: String, _ args: Data = Data()) -> Result<Data, Error>? {
        var out: Result<Data, Error>?
        m[name]?(args) { out = $0 }
        return out
    }

    private func text(_ r: Result<Data, Error>?) -> String {
        switch r {
        case .success(let d)?: return "ok " + String(decoding: d, as: UTF8.self)
        case .failure(let e)?: return "err " + asukaErrorText(e)
        case nil: return "waiting"
        }
    }

    func testPushRegistered() throws {
        let p = AsukaPush.shared
        // APNs's answer goes to each who registered, once.
        var got: [String] = []
        p.registering = [{ got.append(self.text($0)) }, { got.append(self.text($0)) }]
        p.allowed = true
        p.registered(Data([0xab, 0x01]))
        XCTAssertEqual(got, [#"ok {"allowed":true,"service":"apns","token":"ab01"}"#, #"ok {"allowed":true,"service":"apns","token":"ab01"}"#])
        XCTAssertTrue(p.registering.isEmpty)
        // Or why not.
        got = []
        p.registering = [{ got.append(self.text($0)) }]
        p.failed(NSError(domain: "x", code: 1, userInfo: [NSLocalizedDescriptionKey: "no aps-environment"]))
        XCTAssertEqual(got, ["err no aps-environment"])
    }

    func testPushQueues() {
        let p = AsukaPush.shared
        let m = p.methods()
        // A data message waits to be asked for.
        p.received(["aps": ["content-available": 1], "k": "v"])
        let kept = text(call(m, "nextReceived"))
        XCTAssertTrue(kept.hasPrefix("ok {"), kept)
        XCTAssertTrue(kept.contains(#""k":"v""#), kept)
        // A call waits for the next; stop fails it, each queue on its own.
        var received: Result<Data, Error>?, opened: Result<Data, Error>?
        m["nextReceived"]?(Data()) { received = $0 }
        m["nextOpened"]?(Data()) { opened = $0 }
        XCTAssertNil(received)
        _ = call(m, "stop", Data("nextReceived".utf8))
        XCTAssertEqual(text(received), "err stopped")
        XCTAssertNil(opened)
        p.deliver("nextOpened", Data(#"{"id":"a"}"#.utf8))
        XCTAssertEqual(text(opened), #"ok {"id":"a"}"#)
        XCTAssertEqual(text(call(m, "nextRewrite")), "err platform/push: rewriting needs a notification service extension, which a Mac app doesn't have")
        XCTAssertEqual(text(call(m, "rewritten", Data("{}".utf8))), "ok ")
    }

    func testPushData() {
        // Nested values are JSON, their keys sorted, slashes as they are.
        let d = AsukaPush.data(["aps": [:] as [String: Any], "o": ["b": 1, "a": "x/y"], "f": 1.5, "no": false])
        XCTAssertEqual(d, ["o": #"{"a":"x/y","b":1}"#, "f": "1.5", "no": "false"])
    }

    func testQueue() {
        let q = AsukaNotifyQueue()
        q.deliver(Data("1".utf8))
        q.deliver(Data("2".utf8))
        var got: [String] = []
        for _ in 0..<3 { q.next { got.append(self.text($0)) } }
        XCTAssertEqual(got, ["ok 1", "ok 2"])
        q.deliver(Data("3".utf8))
        XCTAssertEqual(got, ["ok 1", "ok 2", "ok 3"])
        q.next { got.append(self.text($0)) }
        q.stop()
        XCTAssertEqual(got.last, "err stopped")
    }

    func testPresentations() {
        XCTAssertEqual(AsukaNotify.options(0), [.banner, .list, .sound, .badge])
        XCTAssertEqual(AsukaNotify.options(1), [.list, .badge])
        XCTAssertEqual(AsukaNotify.options(2), [])
    }

    func testCategories() throws {
        let json = #"[{"id":"chat","actions":[{"id":"reply","title":"Reply","destructive":true,"reply":{"button":"Send","placeholder":"Message"}},{"id":"open","title":"Open","open":true,"unlock":true}]}]"#
        let cs = AsukaNotify.categorySet(Data(json.utf8))
        XCTAssertEqual(cs.count, 1)
        let c = try XCTUnwrap(cs.first)
        XCTAssertEqual(c.identifier, "chat")
        XCTAssertEqual(c.actions.map(\.identifier), ["reply", "open"])
        let reply = try XCTUnwrap(c.actions[0] as? UNTextInputNotificationAction)
        XCTAssertEqual(reply.textInputButtonTitle, "Send")
        XCTAssertEqual(reply.textInputPlaceholder, "Message")
        XCTAssertEqual(reply.options, [.destructive])
        XCTAssertEqual(c.actions[1].options, [.foreground, .authenticationRequired])
        XCTAssertTrue(AsukaNotify.categorySet(Data("nope".utf8)).isEmpty)
    }

    func testContent() {
        let o: [String: Any] = ["title": "", "sender": ["name": "Ann"], "body": "Hi", "data": ["k": "v"], "url": "app://x",
                                "category": "chat", "thread": "t1", "badge": 4, "silent": true]
        let c = AsukaNotify.content(o, nil)
        // A message's title is its sender's name (macOS has no
        // communication notifications).
        XCTAssertEqual(c.title, "Ann")
        XCTAssertEqual(c.body, "Hi")
        XCTAssertNil(c.sound)
        XCTAssertEqual(c.userInfo["asuka.data"] as? [String: String], ["k": "v"])
        XCTAssertEqual(c.userInfo["url"] as? String, "app://x")
        XCTAssertEqual([c.categoryIdentifier, c.threadIdentifier], ["chat", "t1"])
        XCTAssertEqual(c.badge, 4)
        XCTAssertNotNil(AsukaNotify.content(["title": "x"], nil).sound)
    }

    /// A spec's trigger as the center's: calendars for the repeating
    /// kinds, an interval for every, an "at"'s instant; and what pending
    /// says of a request, its trigger as it was scheduled.
    func testTriggers() throws {
        func cal(_ t: [String: Any]) -> UNCalendarNotificationTrigger? {
            AsukaNotify.trigger(["seconds": 60, "trigger": t]) as? UNCalendarNotificationTrigger
        }
        let daily = try XCTUnwrap(cal(["kind": "daily", "hour": 9, "minute": 30]))
        XCTAssertTrue(daily.repeats)
        XCTAssertEqual([daily.dateComponents.hour, daily.dateComponents.minute], [9, 30])
        XCTAssertNil(daily.dateComponents.weekday)
        XCTAssertEqual(cal(["kind": "weekly", "weekday": 2, "hour": 8])?.dateComponents.weekday, 2)
        XCTAssertEqual(cal(["kind": "monthly", "day": 15])?.dateComponents.day, 15)
        let yearly = try XCTUnwrap(cal(["kind": "yearly", "month": 12, "day": 25, "hour": 7]))
        XCTAssertEqual([yearly.dateComponents.month, yearly.dateComponents.day, yearly.dateComponents.hour], [12, 25, 7])
        let at = Date().addingTimeInterval(3600)
        let once = try XCTUnwrap(cal(["kind": "at", "at": Int64(at.timeIntervalSince1970 * 1000)]))
        XCTAssertFalse(once.repeats)
        XCTAssertEqual(once.nextTriggerDate()?.timeIntervalSince1970 ?? 0, at.timeIntervalSince1970, accuracy: 1)
        let every = try XCTUnwrap(AsukaNotify.trigger(["trigger": ["kind": "every", "seconds": 5]]) as? UNTimeIntervalNotificationTrigger)
        XCTAssertTrue(every.repeats)
        XCTAssertEqual(every.timeInterval, 60)  // at least a minute
        XCTAssertNil(AsukaNotify.trigger(["seconds": 0]))
        XCTAssertEqual((AsukaNotify.trigger(["seconds": 4]) as? UNTimeIntervalNotificationTrigger)?.timeInterval, 4)

        let t: [String: Any] = ["kind": "daily", "hour": 9, "minute": 30]
        let c = AsukaNotify.content(["id": "w", "title": "Water", "trigger": t, "interruption": "passive", "relevance": 0.5], nil)
        let p = AsukaNotify.pending(UNNotificationRequest(identifier: "w", content: c, trigger: daily))
        XCTAssertEqual(p["id"] as? String, "w")
        XCTAssertEqual(p["title"] as? String, "Water")
        XCTAssertEqual((p["trigger"] as? [String: Any])?["kind"] as? String, "daily")
        XCTAssertEqual(p["next"] as? Int64, daily.nextTriggerDate().map { Int64($0.timeIntervalSince1970 * 1000) })
        if #available(macOS 12.0, *) {
            XCTAssertEqual(c.interruptionLevel, .passive)
            XCTAssertEqual(c.relevanceScore, 0.5)
        }
    }
}
