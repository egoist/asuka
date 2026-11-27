// The updater's platform part on a Mac: the host module "update", which
// the std package platform/update calls (platform/host's call, JSON in and
// out; DESIGN.md's "Updates" is its spec). The package does what's the
// same everywhere (the feed, channels, versions, signatures, deltas,
// unpacking); the host what only the Mac does: whether this build can
// update itself, downloads (URLSession), and putting the verified bundle
// the package staged in place of the app's once it has quit
// (internal/update's Locate, CheckBundle and StageBundle at bf1839e2,
// ported).
import AppKit
import Darwin

@MainActor extension AsukaRuntime {
    /// The "update" module: info, download, next, cancel, install.
    func registerUpdateModule() {
        let u = AsukaUpdater.shared
        registerModule("update", [
            "info": { _, reply in u.info(reply) },
            "download": { args, reply in u.download(args, reply) },
            "next": { args, reply in u.next(args, reply) },
            "cancel": { args, reply in u.cancel(args, reply) },
            "install": { args, reply in u.install(args, reply) },
        ])
    }
}

@MainActor extension AsukaShell {
    /// The app quits (its quit allowed, or the session ending): a staged
    /// update is put in place by its helper once the process has exited,
    /// and with relaunch the helper opens the new bundle after. True if
    /// there's one (the relaunch is the helper's then).
    func applyStagedUpdate(relaunch: Bool) -> Bool { AsukaUpdater.shared.quitting(relaunch: relaunch) }
}

/// Why an app can't update itself (internal/update's Locate's errors):
/// info's reason, its words the message.
enum AsukaUpdateError: LocalizedError {
    case notConfigured, managed, translocated, notWritable, notInstalled
    case other(String)

    var reason: String {
        switch self {
        case .notConfigured: return "notConfigured"
        case .managed: return "managed"
        case .translocated: return "translocated"
        case .notWritable: return "notWritable"
        case .notInstalled: return "notInstalled"
        case .other: return "failed"
        }
    }

    var errorDescription: String? {
        switch self {
        case .notConfigured: return "the app has no update feed or key (asuka.toml's updates, in a build asuka build makes)"
        case .managed: return "a package manager or an app store installed the app, and updates it"
        case .translocated: return "macOS runs the app from a read-only copy: move it to the Applications folder, then open it again"
        case .notWritable: return "the app can't write where it's installed"
        case .notInstalled: return "the app isn't laid out as asuka build makes it (a bundle, a folder, an install prefix)"
        case .other(let s): return s
        }
    }
}

@MainActor final class AsukaUpdater: NSObject, URLSessionDownloadDelegate {
    static let shared = AsukaUpdater()

    /// A download, and the long poll waiting on it.
    final class Download {
        let id: Int
        let to: String
        var task: URLSessionDownloadTask?
        var downloaded: Int64 = 0
        /// The Content-Length, 0 while it's unknown.
        var total: Int64 = 0
        var done = false
        var error = ""
        /// The response's status, once known.
        var status = 0
        /// What the last answer said.
        var told: Int64 = -1
        var toldDone = false
        var waiting: ((Result<Data, Error>) -> Void)?

        init(id: Int, to: String) {
            self.id = id
            self.to = to
        }
    }

    private var nextID = 1
    private var downloads: [Int: Download] = [:]
    private var byTask: [Int: Download] = [:]
    private lazy var session = URLSession(configuration: .default, delegate: self, delegateQueue: .main)
    /// The update waiting for the app to quit, and its helper.
    private var staged: (work: String, target: String, helper: pid_t)?
    private var cleaned = false

    // MARK: info

    /// The bundle the app runs from, as internal/update's Locate finds it:
    /// its executable's links resolved, Name.app/Contents/MacOS/<exe>; not
    /// one App Translocation runs from a read-only copy of or one run from
    /// its disk image, nor the Mac App Store's (its receipt), which updates
    /// it.
    nonisolated static func locate() throws -> (bundle: String, exe: String) {
        guard var exe = Bundle.main.executablePath, !exe.isEmpty else { throw AsukaUpdateError.notInstalled }
        exe = (exe as NSString).resolvingSymlinksInPath
        let macos = (exe as NSString).deletingLastPathComponent
        let bundle = ((macos as NSString).deletingLastPathComponent as NSString).deletingLastPathComponent
        guard bundle.hasSuffix(".app"), (macos as NSString).lastPathComponent == "MacOS" else { throw AsukaUpdateError.notInstalled }
        if bundle.contains("/AppTranslocation/") { throw AsukaUpdateError.translocated }
        // Run from its disk image, a read-only volume: the user drags it
        // to Applications first, as from a translocated copy.
        var vol = statfs()
        if statfs(bundle, &vol) == 0, vol.f_flags & UInt32(MNT_RDONLY) != 0 { throw AsukaUpdateError.translocated }
        if FileManager.default.fileExists(atPath: bundle + "/Contents/_MASReceipt") { throw AsukaUpdateError.managed }
        return (bundle, exe)
    }

    /// Whether the user can make an update beside the bundle and move the
    /// bundle itself (its parent and its own entry).
    nonisolated static func writable(_ bundle: String) throws {
        let work = (bundle as NSString).deletingLastPathComponent
        var tmpl = Array((work + "/.asuka-probe-XXXXXX").utf8CString)
        guard mkdtemp(&tmpl) != nil else { throw AsukaUpdateError.notWritable }
        rmdir(tmpl)
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: bundle),
              let perm = attrs[.posixPermissions] as? Int, perm & 0o200 != 0 else { throw AsukaUpdateError.notWritable }
    }

    // The updates' target of a build for this Mac's architecture (a Mac
    // build is one architecture's: an Intel install never takes Apple
    // silicon's).
    #if arch(arm64)
    static let thisMac = "darwin-arm64"
    #else
    static let thisMac = "darwin-amd64"
    #endif

    func info(_ reply: @escaping (Result<Data, Error>) -> Void) {
        let rt = AsukaRuntime.shared
        let cfg = rt.config
        let target = cfg.updateTarget.isEmpty ? Self.thisMac : cfg.updateTarget
        let version = Bundle.main.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? cfg.version
        let (feed, key, name) = (cfg.updateFeed, cfg.updateKey, rt.appName)
        DispatchQueue.global(qos: .userInitiated).async {
            var d: [String: Any] = ["installable": false, "reason": "", "message": "", "target": target, "version": version,
                                    "name": name, "feed": feed, "key": key, "kind": "bundle", "root": "", "exe": "", "work": ""]
            do {
                let (bundle, exe) = try Self.locate()
                d["root"] = bundle
                d["exe"] = exe
                d["work"] = (bundle as NSString).deletingLastPathComponent
                if feed.isEmpty || key.isEmpty { throw AsukaUpdateError.notConfigured }
                try Self.writable(bundle)
                d["installable"] = true
            } catch {
                let e = error as? AsukaUpdateError ?? .other(asukaErrorText(error))
                d["reason"] = e.reason
                d["message"] = asukaErrorText(e)
            }
            reply(Result { try JSONSerialization.data(withJSONObject: d, options: [.sortedKeys]) })
        }
    }

    // MARK: Downloading

    private func object(_ args: Data) -> [String: Any] {
        (try? JSONSerialization.jsonObject(with: args)) as? [String: Any] ?? [:]
    }

    func download(_ args: Data, _ reply: @escaping (Result<Data, Error>) -> Void) {
        let a = object(args)
        guard let s = a["url"] as? String, let url = URL(string: s), let to = a["to"] as? String, !to.isEmpty else {
            return reply(.failure(AsukaModuleError("update.download: bad arguments")))
        }
        // HTTPS, or the computer's own (a test's, a local server's).
        let host = url.host ?? ""
        guard url.scheme == "https" || (url.scheme == "http" && (host == "127.0.0.1" || host == "localhost" || host == "::1")) else {
            return reply(.failure(AsukaModuleError("update.download: not an HTTPS URL: \(s)")))
        }
        // The file is made, or emptied, at once: what's there is this
        // download's.
        let fm = FileManager.default
        try? fm.createDirectory(atPath: (to as NSString).deletingLastPathComponent, withIntermediateDirectories: true)
        guard fm.createFile(atPath: to, contents: nil) else {
            return reply(.failure(AsukaModuleError("update.download: can't write \(to)")))
        }
        let d = Download(id: nextID, to: to)
        nextID += 1
        var req = URLRequest(url: url)
        req.setValue("asuka-updater", forHTTPHeaderField: "User-Agent")
        let task = session.downloadTask(with: req)
        d.task = task
        downloads[d.id] = d
        byTask[task.taskIdentifier] = d
        task.resume()
        reply(.success(Data("{\"id\":\(d.id)}".utf8)))
    }

    func next(_ args: Data, _ reply: @escaping (Result<Data, Error>) -> Void) {
        guard let id = object(args)["id"] as? Int, let d = downloads[id] else {
            return reply(.failure(AsukaModuleError("update.next: no such download")))
        }
        if let w = d.waiting { w(.failure(AsukaModuleError("stopped"))) } // one poll at a time
        d.waiting = reply
        answer(d)
    }

    func cancel(_ args: Data, _ reply: @escaping (Result<Data, Error>) -> Void) {
        if let id = object(args)["id"] as? Int, let d = downloads[id], !d.done {
            d.task?.cancel()
            finish(d, "canceled")
        }
        reply(.success(Data("{}".utf8)))
    }

    /// Answers the waiting poll if there's news: the end, or a move of 1%
    /// (of the size, or a MiB without one) since the last answer.
    private func answer(_ d: Download) {
        guard let w = d.waiting else { return }
        let step = d.total > 0 ? max(d.total / 100, 1) : 64 << 10
        let moved = d.told < 0 || d.downloaded - d.told >= step
        guard d.done || moved else { return }
        d.waiting = nil
        d.told = d.downloaded
        if d.done {
            // Told it ended: the id is forgotten.
            d.toldDone = true
            downloads.removeValue(forKey: d.id)
        }
        let o: [String: Any] = ["downloaded": d.downloaded, "total": d.total, "done": d.done, "error": d.error, "status": d.status]
        w(Result { try JSONSerialization.data(withJSONObject: o, options: [.sortedKeys]) })
    }

    private func finish(_ d: Download, _ error: String) {
        guard !d.done else { return }
        d.done = true
        d.error = error
        if let t = d.task { byTask.removeValue(forKey: t.taskIdentifier) }
        answer(d)
    }

    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                                totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        let id = downloadTask.taskIdentifier
        MainActor.assumeIsolated {
            guard let d = byTask[id] else { return }
            d.downloaded = totalBytesWritten
            d.total = max(totalBytesExpectedToWrite, 0)
            if d.status == 0, let r = downloadTask.response as? HTTPURLResponse { d.status = r.statusCode }
            answer(d)
        }
    }

    /// The file is moved where it goes now: URLSession removes it once
    /// this returns.
    nonisolated func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        let id = downloadTask.taskIdentifier
        let status = (downloadTask.response as? HTTPURLResponse)?.statusCode ?? 200
        MainActor.assumeIsolated {
            guard let d = byTask[id] else { return }
            d.status = status
            // A body is kept for 200 only: another status (a server's 204,
            // a 404) leaves the file empty, and says so in status.
            guard status == 200 else {
                d.downloaded = 0
                return finish(d, "")
            }
            let fm = FileManager.default
            do {
                try? fm.removeItem(atPath: d.to)
                try fm.moveItem(at: location, to: URL(fileURLWithPath: d.to))
                if let n = (try? fm.attributesOfItem(atPath: d.to))?[.size] as? Int64 {
                    d.downloaded = n
                    if d.total == 0 { d.total = n }
                }
                finish(d, "")
            } catch {
                finish(d, asukaErrorText(error))
            }
        }
    }

    nonisolated func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        let id = task.taskIdentifier
        let msg = error.map { asukaErrorText($0) }
        MainActor.assumeIsolated {
            guard let d = byTask[id] else { return }
            finish(d, msg ?? "")
        }
    }

    // MARK: Installing

    func install(_ args: Data, _ reply: @escaping (Result<Data, Error>) -> Void) {
        guard let staged = object(args)["staged"] as? String, !staged.isEmpty else {
            return reply(.failure(AsukaModuleError("update.install: bad arguments")))
        }
        // One staged after another takes its place.
        cancelStaged()
        let pid = getpid()
        DispatchQueue.global(qos: .userInitiated).async {
            let r: Result<(String, String, pid_t), Error> = Result { try Self.stage(staged, pid: pid) }
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    switch r {
                    case .success(let (work, target, helper)):
                        self.staged = (work, target, helper)
                        reply(.success(Data("{}".utf8)))
                    case .failure(let e):
                        reply(.failure(e))
                    }
                }
            }
        }
    }

    /// Checks the bundle the package staged (a directory in the install's
    /// work directory: .<Name>.app.update-<id>/..., on the app's volume,
    /// so each step is a rename), and starts the helper that puts it in
    /// place once pid has exited; the helper's pid goes in the work
    /// directory's "staged". Off the main thread.
    nonisolated static func stage(_ staged: String, pid: pid_t) throws -> (String, String, pid_t) {
        let (target, _) = try locate()
        try writable(target)
        var isDir: ObjCBool = false
        let new = (staged as NSString).standardizingPath
        guard FileManager.default.fileExists(atPath: new + "/Contents", isDirectory: &isDir), isDir.boolValue else {
            var st = stat()
            let why = stat(new + "/Contents", &st) == 0 ? "not a directory" : String(cString: strerror(errno))
            throw AsukaUpdateError.other("the update holds no app bundle at \(staged) (\(why))")
        }
        // The work directory: the staged bundle's ancestor beside the app's.
        let parent = (target as NSString).deletingLastPathComponent
        let prefix = "." + (target as NSString).lastPathComponent + ".update-"
        var work = new
        while (work as NSString).deletingLastPathComponent != parent {
            work = (work as NSString).deletingLastPathComponent
            guard work != "/" && !work.isEmpty else { throw AsukaUpdateError.other("the update isn't staged beside the app (\(parent))") }
        }
        guard (work as NSString).lastPathComponent.hasPrefix(prefix), work != new else {
            throw AsukaUpdateError.other("the update isn't staged in a work directory of the app's (\(prefix)*)")
        }
        try checkBundle(new, running: target)
        let helper = try startHelper(pid: pid, target: target, new: new, work: work)
        try? String(helper).write(toFile: work + "/staged", atomically: true, encoding: .utf8)
        return (work, target, helper)
    }

    /// Checks, as Sparkle does, that the update's bundle may take the
    /// place of the app's beyond its update signature: its code signature
    /// is valid (nothing changed since it was signed) and of the same
    /// signer (team ID) and identifier as the running app's, when that one
    /// is signed. It also takes the quarantine attribute off the new
    /// bundle, which macOS gives files only apps that ask download, so
    /// that Gatekeeper doesn't ask the user again about a version of an
    /// app they opened.
    nonisolated static func checkBundle(_ bundle: String, running: String) throws {
        _ = try? run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", bundle])
        guard let old = signer(running) else { return } // not signed, or changed since: nothing to compare
        do {
            try run("/usr/bin/codesign", ["--verify", "--deep", "--strict", bundle])
        } catch {
            throw AsukaUpdateError.other("the update's code signature isn't valid: \(asukaErrorText(error))")
        }
        guard let now = signer(bundle) else { throw AsukaUpdateError.other("the update isn't signed") }
        if now.team != old.team { throw AsukaUpdateError.other("the update is signed by team \(now.team), not the app's \(old.team)") }
        if now.id != old.id { throw AsukaUpdateError.other("the update is the app \(now.id), not \(old.id)") }
    }

    /// The team ID ("not set" for ad hoc signatures) and the identifier of
    /// a bundle whose code signature is valid.
    nonisolated static func signer(_ bundle: String) -> (team: String, id: String)? {
        guard (try? run("/usr/bin/codesign", ["--verify", "--strict", bundle])) != nil,
              let out = try? run("/usr/bin/codesign", ["-dv", "--verbose=2", bundle]) else { return nil }
        var team = "", id = ""
        for line in out.split(separator: "\n") {
            let l = line.trimmingCharacters(in: .whitespaces)
            if l.hasPrefix("TeamIdentifier=") { team = String(l.dropFirst("TeamIdentifier=".count)) }
            else if l.hasPrefix("Identifier=") { id = String(l.dropFirst("Identifier=".count)) }
        }
        return id.isEmpty ? nil : (team, id)
    }

    /// Runs a program and answers its output (and its errors'), failing
    /// with it when it fails.
    @discardableResult
    nonisolated static func run(_ path: String, _ args: [String]) throws -> String {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        try p.run()
        let out = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        let s = String(decoding: out, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        guard p.terminationStatus == 0 else {
            throw AsukaUpdateError.other(s.isEmpty ? "\((path as NSString).lastPathComponent) failed (\(p.terminationStatus))" : s)
        }
        return s
    }

    /// The helper's script (internal/update's installAfterExit): with the
    /// pid of the app, the target, the new bundle and the work directory,
    /// it waits for the app to exit, swaps the bundles (putting the old
    /// one back if the new one can't be moved in), tells Launch Services of
    /// the new one, and runs the command that work/relaunch sets as its
    /// arguments, if the quit asked to relaunch: the arguments are known
    /// only as the app quits. Each step is a rename on one volume.
    nonisolated static let script = """
        pid=$1 target=$2 new=$3 work=$4
        trap '' HUP INT TERM
        while kill -0 "$pid" 2>/dev/null; do sleep 0.05; done
        if [ -d "$new" ] && mv "$target" "$work/old"; then
        	if mv "$new" "$target"; then
        		touch "$target"
        		lsregister=/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister
        		[ -x "$lsregister" ] && "$lsregister" -f "$target" >/dev/null 2>&1
        	else
        		mv "$work/old" "$target"
        	fi
        fi
        relaunch=
        if [ -e "$work/relaunch" ]; then relaunch=1; . "$work/relaunch"; fi
        rm -rf "$work"
        if [ -n "$relaunch" ]; then exec "$@"; fi

        """

    /// Starts the helper: a shell of its own session, which neither the
    /// app's quitting nor the terminal's signals end. It's waited for, so
    /// that it's never a zombie of the app's; it outlives the app otherwise.
    nonisolated static func startHelper(pid: pid_t, target: String, new: String, work: String) throws -> pid_t {
        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETSID))
        var actions: posix_spawn_file_actions_t?
        posix_spawn_file_actions_init(&actions)
        defer { posix_spawn_file_actions_destroy(&actions) }
        for fd: Int32 in [0, 1, 2] {
            posix_spawn_file_actions_addopen(&actions, fd, "/dev/null", fd == 0 ? O_RDONLY : O_WRONLY, 0)
        }
        let argv = ["/bin/sh", "-c", script, "asuka-update", String(pid), target, new, work]
        var cargs: [UnsafeMutablePointer<CChar>?] = argv.map { strdup($0) } + [nil]
        defer { for a in cargs { free(a) } }
        var child: pid_t = 0
        let rc = posix_spawn(&child, "/bin/sh", &actions, &attr, &cargs, environ)
        guard rc == 0 else { throw AsukaUpdateError.other("can't start the update's helper: \(String(cString: strerror(rc)))") }
        Thread.detachNewThread {
            var status: Int32 = 0
            waitpid(child, &status, 0)
        }
        return child
    }

    /// At launch: removes what updates left beside the bundle, their work
    /// directories whose helper isn't running (the app quit while one was
    /// made, or one failed). Off the main thread.
    nonisolated static func cleanAtLaunch() {
        DispatchQueue.global(qos: .utility).async {
            if let (bundle, _) = try? locate() { cleanLeftovers(bundle) }
        }
    }

    /// Removes the work directories of updates staged before whose helper
    /// is gone.
    nonisolated static func cleanLeftovers(_ bundle: String) {
        let parent = (bundle as NSString).deletingLastPathComponent
        let prefix = "." + (bundle as NSString).lastPathComponent + ".update-"
        for name in (try? FileManager.default.contentsOfDirectory(atPath: parent)) ?? [] where name.hasPrefix(prefix) {
            let p = parent + "/" + name
            if let s = try? String(contentsOfFile: p + "/staged", encoding: .utf8),
               let helper = pid_t(s.trimmingCharacters(in: .whitespacesAndNewlines)), helper > 0,
               kill(helper, 0) == 0 || errno == EPERM {
                continue // its helper waits for its app
            }
            try? FileManager.default.removeItem(atPath: p)
        }
    }

    /// Stops the staged update's helper and removes it.
    private func cancelStaged() {
        guard let s = staged else { return }
        staged = nil
        kill(s.helper, SIGKILL)
        try? FileManager.default.removeItem(atPath: s.work)
    }

    /// The app quits: with relaunch, the helper opens the new bundle
    /// through Launch Services once it's in place (as the Dock opens apps:
    /// not as a child of this process), with the app's arguments; without
    /// -n: the app has exited, and the Dock gives the new version its tile.
    func quitting(relaunch: Bool) -> Bool {
        guard let s = staged else { return false }
        if relaunch {
            var argv = ["/usr/bin/open", s.target]
            let args = Array(CommandLine.arguments.dropFirst())
            if !args.isEmpty { argv += ["--args"] + args }
            let set = "set --" + argv.map { " '" + $0.replacingOccurrences(of: "'", with: "'\\''") + "'" }.joined() + "\n"
            try? set.write(toFile: s.work + "/relaunch", atomically: true, encoding: .utf8)
        }
        return true
    }
}
