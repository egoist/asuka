// What the foundation asks of each part of the host as a node changes: each
// part's hooks (in its own files) are called here, so a part adds what it
// keeps of a node without the runtime knowing it.
//
// The parts, and their files' prefixes:
//   - nav: navigation and windows (Stack, Screen, Tabs, SplitView, Sheet,
//     Alert, Popover, Snackbar, Window, toolbars, menus, transitions,
//     matched views, window options and state, panels and flyouts)
//   - list: lists, refresh, paging and page dots (List, PageControl)
//   - text: rich text, rich and multiline fields, selection, keys
//   - visual: images, symbols, shapes, canvases, shaders, progress rings,
//     video, web views, native views, interaction (press, hover, drag and
//     drop, gestures)
//   - shell: the app (menu bar, tray, Dock, quit, files and links, single
//     instance, crash records, the updater's module, routes) and its
//     modules (dock, hotkey, login, recent, apps, dialogs, clipboard, ...)
//   - modules1, modules2: the packages' modules (camera, location, ...)
//   - focus: the keyboard's focus (Focusable, Tab, FocusTrap, KeyText) and
//     screen readers' roles, states and relations (Focus.swift, A11y.swift).
//     Overlays (Overlay.swift) are navigation's.
//   - asuka dev's error overlay (DevError.swift): AppKit's views over the
//     main window, none of the app's, so it has no hooks here; and its
//     inspector's highlight (DevHighlight.swift), likewise.
import AppKit
#if canImport(AsukaOps)
import AsukaOps
#endif

@MainActor extension AsukaRuntime {
    /// n is being destroyed (before its view goes, or stays for a
    /// transition): what its parts keep that the core no longer needs.
    func destroyParts(_ n: AsukaNode) {
        navDestroyed(n)
        listDestroyed(n)
        textDestroyed(n)
        visualDestroyed(n)
        focusDestroyed(n)
    }

    /// n's view goes: what its parts hold.
    func releaseParts(_ n: AsukaNode) {
        listReleased(n)
        textReleased(n)
        visualReleased(n)
    }

    /// n has a new frame: what follows its size.
    func placed(_ n: AsukaNode) {
        navPlaced(n)
        listPlaced(n)
        textPlaced(n)
        visualPlaced(n)
    }

    /// The appearance changed: colors the parts set as fixed CGColors.
    func partColors(_ n: AsukaNode) {
        listColors(n)
        textColors(n)
        visualColors(n)
    }

    /// n's direction was applied (changed: it reads another way now).
    func directionChanged(_ n: AsukaNode, _ changed: Bool) {
        navDirection(n, changed)
        listDirection(n, changed)
        textDirection(n, changed)
        visualDirection(n, changed)
    }

    /// The batch has been applied (frames: it had frames).
    func batchApplied(_ frames: Bool) {
        navBatchApplied(frames)
        listBatchApplied(frames)
        a11yBatchApplied()
    }

    /// What the parts add to a node's JSON for the automation's tree.
    func partJSON(_ n: AsukaNode, _ d: inout [String: Any]) {
        navJSON(n, &d)
        listJSON(n, &d)
        textJSON(n, &d)
        visualJSON(n, &d)
        focusJSON(n, &d)
    }

    func windowPartJSON(_ w: AsukaWin, _ d: inout [String: Any]) {
        navWindowJSON(w, &d)
        focusWindowJSON(w, &d)
    }

    /// The host's modules, registered the first time one is called.
    func registerAllModules() {
        registerShellModules()
        registerNavModules()
        registerTextModules()
        registerVisualModules()
        registerModules1()
        registerModules2()
    }
}
