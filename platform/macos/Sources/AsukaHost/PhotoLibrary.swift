// platform/photos on a Mac, with PhotoKit as on iOS ("photoLibrary", the
// iOS host's Photos.swift): "current" and "request" answer what's
// allowed; "list" ({screenshots, offset, limit, kind: 0 photos, 1 videos,
// 2 both, album}) JSON assets (id, width, height, created in ms,
// screenshot, video, duration in ms, favorite), newest first, and "count"
// how many; "thumbnail" a JPEG's path in the caches (a video's frame) and
// "export" JSON (path, name: the photo or video as edited); "albums" the
// user's albums then the smart ones that hold something (id, title,
// count, system), "createAlbum" ({title}) and "addToAlbum" ({album,
// ids}); "delete" ({ids}) "true", or "false" if the user said no to the
// system's question; "nextChange" the next change (inserted, removed and
// updated ids), coalesced while nobody waits, and "stop" ("nextChange").
// macOS has no limited library (nor its picker): "manage" does nothing.
// Before the user has allowed it, "list" answers none, "count" 0,
// "albums" none and the others "not allowed", without asking: request
// asks, with the app's NSPhotoLibraryUsageDescription.
//
// platform/library ("library": "save"): movies and pictures go to Photos
// (after asking to add to it, NSPhotoLibraryAddUsageDescription),
// answering with the asset's identifier; downloads to ~/Downloads and
// music to ~/Music, under the folder asked, answering with the file's
// path.
import AppKit
import CryptoKit
import Photos

@MainActor final class AsukaPhotoLibrary {
    static let shared = AsukaPhotoLibrary()

    // MARK: Access

    /// PhotoKit's authorization status for an access level (the
    /// automation's seam's, under it: "photos" read and write, "photosAdd"
    /// add only).
    static func status(_ level: PHAccessLevel) -> Int {
        if let s = AsukaAccessFake.status(level == .addOnly ? "photosAdd" : "photos") { return s }
        return PHPhotoLibrary.authorizationStatus(for: level).rawValue
    }

    /// Asks the user, then calls done on the main thread with the status.
    static func request(_ level: PHAccessLevel, _ done: @escaping (Int) -> Void) {
        let name = level == .addOnly ? "photosAdd" : "photos"
        if AsukaAccessFake.request(name) {
            let s = status(level)
            DispatchQueue.main.async { done(s) }
            return
        }
        PHPhotoLibrary.requestAuthorization(for: level) { s in DispatchQueue.main.async { done(s.rawValue) } }
    }

    /// A status as platform/photos' wire names it.
    static func access(_ s: Int) -> String {
        switch s {
        case 3: return "allowed"
        case 4: return "limited"
        case 1, 2: return "denied"
        default: return "notAsked"
        }
    }

    static func readable() -> Bool {
        let s = status(.readWrite)
        return s == 3 || s == 4
    }

    nonisolated static func asset(_ id: String) throws -> PHAsset {
        guard let a = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject else { throw AsukaModuleError("no such photo") }
        return a
    }

    /// A thumbnail's file name: the asset's and its edit's, hashed, and
    /// the size.
    nonisolated static func thumbName(_ id: String, _ modified: Double, _ size: Int) -> String {
        let stamp = modified == modified.rounded() && abs(modified) < 1e15 ? String(Int64(modified)) : String(modified)
        let sum = SHA256.hash(data: Data((id + stamp).utf8))
        return sum.prefix(16).map { String(format: "%02x", $0) }.joined() + "-\(size).jpg"
    }

    /// Writes an NSImage as a JPEG.
    nonisolated static func writeJPEG(_ img: NSImage, _ path: String, _ quality: Double) throws {
        guard let tiff = img.tiffRepresentation, let rep = NSBitmapImageRep(data: tiff),
              let data = rep.representation(using: .jpeg, properties: [.compressionFactor: quality]) else {
            throw AsukaModuleError("no thumbnail")
        }
        let tmp = path + ".tmp"
        try data.write(to: URL(fileURLWithPath: tmp))
        _ = try? FileManager.default.removeItem(atPath: path)
        try FileManager.default.moveItem(atPath: tmp, toPath: path)
    }

    /// What a list or count asks for: kind (0 photos, 1 videos, 2 both),
    /// screenshots only, of an album ("": the library), newest first.
    nonisolated static func fetch(_ o: [String: Any]) throws -> PHFetchResult<PHAsset> {
        let opts = PHFetchOptions()
        opts.sortDescriptors = [NSSortDescriptor(key: "creationDate", ascending: false)]
        var preds: [NSPredicate] = []
        switch o["kind"] as? Int ?? 0 {
        case 1: preds.append(NSPredicate(format: "mediaType == %d", PHAssetMediaType.video.rawValue))
        case 2: preds.append(NSPredicate(format: "mediaType == %d || mediaType == %d", PHAssetMediaType.image.rawValue, PHAssetMediaType.video.rawValue))
        default: preds.append(NSPredicate(format: "mediaType == %d", PHAssetMediaType.image.rawValue))
        }
        if o["screenshots"] as? Bool ?? false {
            preds.append(NSPredicate(format: "(mediaSubtypes & %d) != 0", PHAssetMediaSubtype.photoScreenshot.rawValue))
        }
        opts.predicate = NSCompoundPredicate(andPredicateWithSubpredicates: preds)
        let album = o["album"] as? String ?? ""
        if album.isEmpty { return PHAsset.fetchAssets(with: opts) }
        guard let c = PHAssetCollection.fetchAssetCollections(withLocalIdentifiers: [album], options: nil).firstObject else {
            throw AsukaModuleError("no such album")
        }
        return PHAsset.fetchAssets(in: c, options: opts)
    }

    nonisolated static func json(_ a: PHAsset) -> [String: Any] {
        [
            "id": a.localIdentifier, "width": a.pixelWidth, "height": a.pixelHeight,
            "created": a.creationDate.map { Int64($0.timeIntervalSince1970 * 1000) } ?? 0,
            "screenshot": a.mediaSubtypes.contains(.photoScreenshot),
            "video": a.mediaType == .video, "duration": Int64((a.duration * 1000).rounded()),
            "favorite": a.isFavorite,
        ]
    }

    /// How many photos and videos an album holds.
    nonisolated static func count(_ c: PHAssetCollection) -> Int {
        let opts = PHFetchOptions()
        opts.predicate = NSPredicate(format: "mediaType == %d || mediaType == %d", PHAssetMediaType.image.rawValue, PHAssetMediaType.video.rawValue)
        return PHAsset.fetchAssets(in: c, options: opts).count
    }

    /// The albums: the user's (and apps') regular ones, then the system's
    /// smart albums that hold something (not the hidden one).
    nonisolated static func albums() -> [[String: Any]] {
        var out: [[String: Any]] = []
        PHAssetCollection.fetchAssetCollections(with: .album, subtype: .albumRegular, options: nil).enumerateObjects { c, _, _ in
            out.append(["id": c.localIdentifier, "title": c.localizedTitle ?? "", "count": count(c), "system": false])
        }
        PHAssetCollection.fetchAssetCollections(with: .smartAlbum, subtype: .any, options: nil).enumerateObjects { c, _, _ in
            guard c.assetCollectionSubtype != .smartAlbumAllHidden else { return }
            let n = count(c)
            if n > 0 { out.append(["id": c.localIdentifier, "title": c.localizedTitle ?? "", "count": n, "system": true]) }
        }
        return out
    }

    /// The assets of ids, all of them (else "no such photo").
    nonisolated static func assets(_ ids: [String]) throws -> PHFetchResult<PHAsset> {
        let found = PHAsset.fetchAssets(withLocalIdentifiers: ids, options: nil)
        if found.count < Set(ids).count { throw AsukaModuleError("no such photo") }
        return found
    }

    /// Whether a change failed as the user said no to the system's
    /// question (PHPhotosError.userCancelled).
    nonisolated static func cancelled(_ e: Error?) -> Bool {
        guard let e = e as NSError? else { return false }
        return e.domain == PHPhotosErrorDomain && e.code == 3072
    }

    // MARK: The module

    func methods() -> [String: AsukaModuleMethod] {
        [
            "current": { _, reply in reply(.success(Data(Self.access(Self.status(.readWrite)).utf8))) },
            "request": { _, reply in
                let s = Self.status(.readWrite)
                if s != 0 {
                    reply(.success(Data(Self.access(s).utf8)))
                    return
                }
                Self.request(.readWrite) {
                    reply(.success(Data(Self.access($0).utf8)))
                    AsukaPhotoChanges.shared.start()
                }
            },
            // macOS has no limited library to manage.
            "manage": { _, reply in reply(.success(Data())) },
            "list": { args, reply in
                guard Self.readable() else { reply(.success(Data("[]".utf8))); return }
                let o = AsukaModules2Kit.object(args) ?? [:]
                let offset = max(o["offset"] as? Int ?? 0, 0), limit = o["limit"] as? Int ?? 100
                AsukaModules2Kit.offMain(reply) {
                    let found = try Self.fetch(o)
                    var out: [[String: Any]] = []
                    var i = offset
                    while i < min(offset + limit, found.count) {
                        out.append(Self.json(found.object(at: i)))
                        i += 1
                    }
                    return try JSONSerialization.data(withJSONObject: out)
                }
            },
            "count": { args, reply in
                guard Self.readable() else { reply(.success(Data("0".utf8))); return }
                let o = AsukaModules2Kit.object(args) ?? [:]
                AsukaModules2Kit.offMain(reply) { Data(String(try Self.fetch(o).count).utf8) }
            },
            "albums": { _, reply in
                guard Self.readable() else { reply(.success(Data("[]".utf8))); return }
                AsukaModules2Kit.offMain(reply) { try JSONSerialization.data(withJSONObject: Self.albums()) }
            },
            "createAlbum": { args, reply in
                guard Self.readable() else { reply(.failure(asukaNotAllowed)); return }
                let title = AsukaModules2Kit.object(args)?["title"] as? String ?? ""
                var id = ""
                PHPhotoLibrary.shared().performChanges({
                    id = PHAssetCollectionChangeRequest.creationRequestForAssetCollection(withTitle: title).placeholderForCreatedAssetCollection.localIdentifier
                }) { ok, e in
                    DispatchQueue.main.async {
                        guard ok else { reply(.failure(AsukaModules2Kit.error(e, "couldn't make the album"))); return }
                        reply(Result { try JSONSerialization.data(withJSONObject: ["id": id, "title": title, "count": 0, "system": false] as [String: Any]) })
                    }
                }
            },
            "addToAlbum": { args, reply in
                guard Self.readable() else { reply(.failure(asukaNotAllowed)); return }
                let o = AsukaModules2Kit.object(args) ?? [:]
                let ids = o["ids"] as? [String] ?? []
                guard let c = PHAssetCollection.fetchAssetCollections(withLocalIdentifiers: [o["album"] as? String ?? ""], options: nil).firstObject else {
                    reply(.failure(AsukaModuleError("no such album")))
                    return
                }
                guard c.assetCollectionType == .album, c.canPerform(.addContent) else {
                    reply(.failure(AsukaModuleError("nothing can be added to this album")))
                    return
                }
                let found: PHFetchResult<PHAsset>
                do { found = try Self.assets(ids) } catch { reply(.failure(error)); return }
                if ids.isEmpty { reply(.success(Data())); return }
                PHPhotoLibrary.shared().performChanges({
                    PHAssetCollectionChangeRequest(for: c)?.addAssets(found)
                }) { ok, e in
                    DispatchQueue.main.async { reply(ok ? .success(Data()) : .failure(AsukaModules2Kit.error(e, "couldn't add to the album"))) }
                }
            },
            "delete": { args, reply in
                guard Self.readable() else { reply(.failure(asukaNotAllowed)); return }
                let ids = AsukaModules2Kit.object(args)?["ids"] as? [String] ?? []
                let found: PHFetchResult<PHAsset>
                do { found = try Self.assets(ids) } catch { reply(.failure(error)); return }
                if ids.isEmpty { reply(.success(Data("true".utf8))); return }
                // The system asks the user first.
                PHPhotoLibrary.shared().performChanges({
                    PHAssetChangeRequest.deleteAssets(found)
                }) { ok, e in
                    DispatchQueue.main.async {
                        if ok { reply(.success(Data("true".utf8))) } else if Self.cancelled(e) { reply(.success(Data("false".utf8))) } else {
                            reply(.failure(AsukaModules2Kit.error(e, "couldn't delete")))
                        }
                    }
                }
            },
            "nextChange": { _, reply in AsukaPhotoChanges.shared.next(reply) },
            "stop": { args, reply in
                if String(decoding: args, as: UTF8.self) == "nextChange" { AsukaPhotoChanges.shared.stop() }
                reply(.success(Data()))
            },
            "thumbnail": { args, reply in
                guard Self.readable() else { reply(.failure(asukaNotAllowed)); return }
                let o = AsukaModules2Kit.object(args) ?? [:]
                let id = o["id"] as? String ?? "", size = o["size"] as? Int ?? 300
                AsukaModules2Kit.offMain(reply) {
                    let a = try Self.asset(id)
                    let stamp = a.modificationDate?.timeIntervalSince1970 ?? 0
                    let dir = (try AsukaModules2Kit.cacheFile("asuka-photo-thumbs", "") as NSString).deletingLastPathComponent
                    let path = (dir as NSString).appendingPathComponent(Self.thumbName(a.localIdentifier, stamp, size))
                    if FileManager.default.fileExists(atPath: path) { return Data(path.utf8) }
                    let po = PHImageRequestOptions()
                    po.deliveryMode = .highQualityFormat
                    po.resizeMode = .fast
                    po.isNetworkAccessAllowed = true
                    po.isSynchronous = true
                    var image: NSImage?
                    PHImageManager.default().requestImage(for: a, targetSize: CGSize(width: size, height: size), contentMode: .aspectFill, options: po) { img, _ in image = img }
                    guard let image else { throw AsukaModuleError("no thumbnail") }
                    try Self.writeJPEG(image, path, 0.8)
                    return Data(path.utf8)
                }
            },
            "export": { args, reply in
                guard Self.readable() else { reply(.failure(asukaNotAllowed)); return }
                let o = AsukaModules2Kit.object(args) ?? [:]
                let a: PHAsset
                do { a = try Self.asset(o["id"] as? String ?? "") } catch { reply(.failure(error)); return }
                let rs = PHAssetResource.assetResources(for: a)
                func find(_ t: PHAssetResourceType) -> PHAssetResource? { rs.first { $0.type == t } }
                // The photo or the video as edited, if it was.
                let video = a.mediaType == .video
                let original = find(video ? .video : .photo)
                guard let r = find(video ? .fullSizeVideo : .fullSizePhoto) ?? original ?? rs.first else { reply(.failure(AsukaModuleError("no such photo"))); return }
                let name = original?.originalFilename ?? r.originalFilename
                var ext = (r.originalFilename as NSString).pathExtension
                if ext.isEmpty { ext = video ? "mov" : "jpg" }
                let path: String
                do { path = try AsukaModules2Kit.cacheFile("asuka-photo-exports", "." + ext) } catch { reply(.failure(error)); return }
                let wo = PHAssetResourceRequestOptions()
                wo.isNetworkAccessAllowed = true
                PHAssetResourceManager.default().writeData(for: r, toFile: URL(fileURLWithPath: path), options: wo) { e in
                    DispatchQueue.main.async {
                        if let e {
                            reply(.failure(AsukaModules2Kit.error(e, "couldn't export the photo")))
                            return
                        }
                        reply(Result { try JSONSerialization.data(withJSONObject: ["path": path, "name": name]) })
                    }
                }
            },
        ]
    }
}

/// The library's changes, for "nextChange" (the iOS host's, the same):
/// PhotoKit's observer, registered once someone waits and the user has
/// allowed the library (registering before would ask), with a fetch of
/// every asset whose change details name what was inserted, removed and
/// updated, and one of the user's albums (a change there names nothing:
/// list again). What comes while nobody waits is kept, coalesced, till the
/// next call.
@MainActor final class AsukaPhotoChanges: NSObject, PHPhotoLibraryChangeObserver {
    static let shared = AsukaPhotoChanges()
    private var all: PHFetchResult<PHAsset>?
    private var albums: PHFetchResult<PHAssetCollection>?
    private var registered = false
    private var wanted = false
    private var pending: (inserted: [String], removed: [String], updated: [String])?
    private var waiting: [(Result<Data, Error>) -> Void] = []

    func next(_ reply: @escaping (Result<Data, Error>) -> Void) {
        wanted = true
        start()
        if let p = pending {
            pending = nil
            reply(Self.json(p))
        } else {
            waiting.append(reply)
        }
    }

    func stop() {
        let ws = waiting
        waiting = []
        for w in ws { w(.failure(AsukaModuleError("stopped"))) }
    }

    /// Registers, once it's wanted and PhotoKit itself allows the library
    /// (not the automation's seam: that would ask).
    func start() {
        let s = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        guard wanted, !registered, s == .authorized || s == .limited else { return }
        registered = true
        all = PHAsset.fetchAssets(with: nil)
        albums = PHAssetCollection.fetchAssetCollections(with: .album, subtype: .albumRegular, options: nil)
        PHPhotoLibrary.shared().register(self)
    }

    nonisolated func photoLibraryDidChange(_ change: PHChange) {
        DispatchQueue.main.async {
            MainActor.assumeIsolated { self.changed(change) }
        }
    }

    private func changed(_ change: PHChange) {
        var c: (inserted: [String], removed: [String], updated: [String])?
        if let all, let d = change.changeDetails(for: all) {
            self.all = d.fetchResultAfterChanges
            c = (d.insertedObjects.map(\.localIdentifier), d.removedObjects.map(\.localIdentifier), d.changedObjects.map(\.localIdentifier))
        }
        if let albums, let d = change.changeDetails(for: albums) {
            self.albums = d.fetchResultAfterChanges
            if c == nil { c = ([], [], []) }
        }
        guard let c else { return }
        if !waiting.isEmpty {
            waiting.removeFirst()(Self.json(c))
            return
        }
        var p = pending ?? ([], [], [])
        func add(_ to: inout [String], _ ids: [String]) { for i in ids where !to.contains(i) { to.append(i) } }
        add(&p.inserted, c.inserted)
        add(&p.removed, c.removed)
        add(&p.updated, c.updated)
        pending = p
    }

    private static func json(_ c: (inserted: [String], removed: [String], updated: [String])) -> Result<Data, Error> {
        Result { try JSONSerialization.data(withJSONObject: ["inserted": c.inserted, "removed": c.removed, "updated": c.updated]) }
    }
}

/// platform/library's module.
@MainActor enum AsukaLibrary {
    /// Where save puts a collection that isn't Photos': Downloads (0) and
    /// Music (2). Under the automation, ASUKA_TEST_LIBRARY_DIR's Downloads
    /// and Music: a test never writes to the user's.
    static func dir(_ collection: Int) -> String {
        var home = NSHomeDirectory()
        if AsukaAutomation.shared.active, let d = AsukaAutomation.shared.env["ASUKA_TEST_LIBRARY_DIR"], !d.isEmpty { home = d }
        return (home as NSString).appendingPathComponent(collection == 2 ? "Music" : "Downloads")
    }

    /// Copies (or moves) src into dir/folder as name, numbered
    /// "name (1).ext"... if it's taken, and answers the new path.
    nonisolated static func saveFile(_ src: String, _ dir: String, _ folder: String, _ name: String, move: Bool) throws -> String {
        let fm = FileManager.default
        var d = dir
        if !folder.isEmpty { d = (d as NSString).appendingPathComponent(folder) }
        try fm.createDirectory(atPath: d, withIntermediateDirectories: true)
        let ext = (name as NSString).pathExtension, base = (name as NSString).deletingPathExtension
        var dst = (d as NSString).appendingPathComponent(name)
        var i = 1
        // Taken, a dangling link too (lstat's: not what it points to).
        while (try? fm.attributesOfItem(atPath: dst)) != nil {
            dst = (d as NSString).appendingPathComponent("\(base) (\(i))" + (ext.isEmpty ? "" : "." + ext))
            i += 1
        }
        if move, rename(src, dst) == 0 { return dst }
        guard let input = FileHandle(forReadingAtPath: src) else { throw AsukaModuleError("can't read \(src)") }
        defer { input.closeFile() }
        let fd = open(dst, O_WRONLY | O_CREAT | O_EXCL, 0o644)
        guard fd >= 0 else { throw AsukaModuleError(String(cString: strerror(errno))) }
        let out = FileHandle(fileDescriptor: fd, closeOnDealloc: true)
        do {
            while true {
                let chunk = input.readData(ofLength: 1 << 20)
                if chunk.isEmpty { break }
                out.write(chunk)
            }
            try out.synchronize()
        } catch {
            try? fm.removeItem(atPath: dst)
            throw error
        }
        if move { try? fm.removeItem(atPath: src) }
        return dst
    }

    /// Adds a video (1) or picture (3) to Photos.
    static func savePhotos(_ path: String, _ collection: Int, _ name: String, move: Bool, _ reply: @escaping (Result<Data, Error>) -> Void) {
        let save = { (status: Int) in
            guard status == 3 || status == 4 else { reply(.failure(asukaNotAllowed)); return }
            var id = ""
            PHPhotoLibrary.shared().performChanges({
                let r = PHAssetCreationRequest.forAsset()
                let opts = PHAssetResourceCreationOptions()
                opts.originalFilename = name
                opts.shouldMoveFile = move
                r.addResource(with: collection == 1 ? .video : .photo, fileURL: URL(fileURLWithPath: path), options: opts)
                id = r.placeholderForCreatedAsset?.localIdentifier ?? ""
            }) { ok, e in
                DispatchQueue.main.async {
                    reply(ok ? .success(Data(id.utf8)) : .failure(AsukaModules2Kit.error(e, "couldn't save")))
                }
            }
        }
        let s = AsukaPhotoLibrary.status(.addOnly)
        if s != 0 {
            save(s)
            return
        }
        AsukaPhotoLibrary.request(.addOnly, save)
    }

    static func methods() -> [String: AsukaModuleMethod] {
        ["save": { args, reply in
            guard let o = AsukaModules2Kit.object(args), let path = o["path"] as? String, !path.isEmpty else {
                reply(.failure(AsukaModuleError("bad arguments")))
                return
            }
            guard FileManager.default.fileExists(atPath: path) else { reply(.failure(AsukaModuleError("no file at " + path))); return }
            var name = o["name"] as? String ?? ""
            if name.isEmpty { name = (path as NSString).lastPathComponent }
            let collection = o["collection"] as? Int ?? 0, folder = o["folder"] as? String ?? "", move = o["move"] as? Bool ?? false
            if collection == 1 || collection == 3 {
                savePhotos(path, collection, name, move: move, reply)
                return
            }
            let target = Self.dir(collection)
            AsukaModules2Kit.offMain(reply) { Data(try saveFile(path, target, folder, name, move: move).utf8) }
        }]
    }
}
