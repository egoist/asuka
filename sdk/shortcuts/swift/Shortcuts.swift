// platform/shortcuts on iOS and the Mac: the app's things for its App
// Intents' entity params, kept in its defaults, where the generated
// queries (the CLI's Shortcuts.swift: AsukaEntityQuery_<kind>) read them;
// then Siri's phrases that name them are brought up to date.
import Asuka
import Foundation

final class EntityStore: Store {
    func set(kind: String, json: String) throws {
        UserDefaults.standard.set(Data(json.utf8), forKey: "asuka.entities.\(kind)")
        // The app's shortcuts' own (by name: they're compiled with the
        // app, after this package), if it has any that take entities.
        if let bridge = NSClassFromString("AsukaShortcutsBridge") as? NSObject.Type {
            let update = NSSelectorFromString("update")
            if bridge.responds(to: update) { _ = bridge.perform(update) }
        }
    }
}

func makeStore() throws -> any Store { EntityStore() }
