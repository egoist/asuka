#if canImport(UIKit) && ASUKA_PHOTOLIBRARY // an app that calls platform/photos (see the CLI's iosFeatures)
import CryptoKit
import Photos
import PhotosUI
import UIKit

/// platform/photos' module, on PhotoKit (the Mac host's PhotoLibrary.swift
/// is the same module on the Mac's):
/// - "current" and "request" answer what's allowed; "manage" shows the
///   limited library picker.
/// - "list" ({screenshots, offset, limit, kind: 0 photos, 1 videos, 2
///   both, album: a collection's id or ""}) answers JSON assets (id,
///   width, height, created in ms, screenshot, video, duration in ms,
///   favorite), newest first; "count" (the same) a number.
/// - "thumbnail" ({id, size}) a JPEG's path (a video's frame); "export"
///   ({id}) JSON (path, name): the photo or the video as edited, if it was.
/// - "albums" JSON albums (id, title, count, system): the user's regular
///   albums, then the smart albums that hold something; "createAlbum"
///   ({title}) the new album; "addToAlbum" ({album, ids}) ""; "delete"
///   ({ids}) "true", or "false" if the user said no to the system's
///   question.
/// - "nextChange" the next change ({inserted, removed, updated}: ids),
///   those that came while nobody waited coalesced into one; "stop"
///   ("nextChange") fails the waiting one with "stopped".
/// Off the main thread where they wait.
enum AsukaPhotos {
    static func access(_ s: PHAuthorizationStatus) -> String {
        switch s {
        case .authorized: return "allowed"
        case .limited: return "limited"
        case .denied, .restricted: return "denied"
        default: return "notAsked"
        }
    }

    static func readable() -> Bool {
        let s = PHPhotoLibrary.authorizationStatus(for: .readWrite)
        return s == .authorized || s == .limited
    }

    static func cacheDir(_ name: String) throws -> URL {
        let dir = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent(name, isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }

    static func asset(_ id: String) throws -> PHAsset {
        guard let a = PHAsset.fetchAssets(withLocalIdentifiers: [id], options: nil).firstObject else { throw AsukaModuleError("no such photo") }
        return a
    }

    static func hash(_ s: String) -> String {
        SHA256.hash(data: Data(s.utf8)).prefix(16).map { String(format: "%02x", $0) }.joined()
    }

    static func object(_ args: Data) -> [String: Any] {
        (try? JSONSerialization.jsonObject(with: args)) as? [String: Any] ?? [:]
    }

    /// What a list or count asks for: kind (0 photos, 1 videos, 2 both),
    /// screenshots only, of an album ("": the library), newest first.
    static func fetch(_ o: [String: Any]) throws -> PHFetchResult<PHAsset> {
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

    static func json(_ a: PHAsset) -> [String: Any] {
        [
            "id": a.localIdentifier, "width": a.pixelWidth, "height": a.pixelHeight,
            "created": Int64((a.creationDate?.timeIntervalSince1970 ?? 0) * 1000),
            "screenshot": a.mediaSubtypes.contains(.photoScreenshot),
            "video": a.mediaType == .video, "duration": Int64((a.duration * 1000).rounded()),
            "favorite": a.isFavorite,
        ]
    }

    /// How many photos and videos an album holds.
    static func count(_ c: PHAssetCollection) -> Int {
        let opts = PHFetchOptions()
        opts.predicate = NSPredicate(format: "mediaType == %d || mediaType == %d", PHAssetMediaType.image.rawValue, PHAssetMediaType.video.rawValue)
        return PHAsset.fetchAssets(in: c, options: opts).count
    }

    /// The albums: the user's (and apps') regular ones, then the system's
    /// smart albums that hold something (not the hidden one).
    static func albums() -> [[String: Any]] {
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
    static func assets(_ ids: [String]) throws -> PHFetchResult<PHAsset> {
        let found = PHAsset.fetchAssets(withLocalIdentifiers: ids, options: nil)
        if found.count < Set(ids).count { throw AsukaModuleError("no such photo") }
        return found
    }

    /// A PhotoKit change's error, as the module's (the user's no to the
    /// system's question is "cancelled").
    static func cancelled(_ e: Error?) -> Bool {
        guard let e = e as NSError? else { return false }
        return e.domain == PHPhotosErrorDomain && e.code == PHPhotosError.Code.userCancelled.rawValue
    }

    @MainActor static func methods() -> [String: AsukaModuleMethod] {
        [
            "current": { _, reply in
                reply(.success(Data(access(PHPhotoLibrary.authorizationStatus(for: .readWrite)).utf8)))
            },
            "request": { _, reply in
                PHPhotoLibrary.requestAuthorization(for: .readWrite) { s in
                    reply(.success(Data(access(s).utf8)))
                    DispatchQueue.main.async { AsukaPhotoChanges.shared.start() }
                }
            },
            "manage": { _, reply in
                guard let vc = AsukaRuntime.shared.topController else {
                    reply(.failure(AsukaModuleError("no window")))
                    return
                }
                PHPhotoLibrary.shared().presentLimitedLibraryPicker(from: vc) { _ in reply(.success(Data())) }
            },
            "list": { args, reply in
                let o = object(args)
                let offset = max(o["offset"] as? Int ?? 0, 0), limit = o["limit"] as? Int ?? 100
                DispatchQueue.global(qos: .userInitiated).async {
                    reply(Result {
                        guard readable() else { return Data("[]".utf8) }
                        let found = try fetch(o)
                        var out: [[String: Any]] = []
                        if offset < found.count {
                            for i in offset..<min(offset + limit, found.count) { out.append(json(found.object(at: i))) }
                        }
                        return try JSONSerialization.data(withJSONObject: out)
                    })
                }
            },
            "count": { args, reply in
                let o = object(args)
                DispatchQueue.global(qos: .userInitiated).async {
                    reply(Result {
                        guard readable() else { return Data("0".utf8) }
                        return Data(String(try fetch(o).count).utf8)
                    })
                }
            },
            "thumbnail": { args, reply in
                let o = object(args)
                let size = o["size"] as? Int ?? 300
                DispatchQueue.global(qos: .userInitiated).async {
                    do {
                        let a = try asset(o["id"] as? String ?? "")
                        let stamp = a.modificationDate?.timeIntervalSince1970 ?? 0
                        let url = try cacheDir("asuka-photo-thumbs").appendingPathComponent("\(hash(a.localIdentifier + "\(stamp)"))-\(size).jpg")
                        if FileManager.default.fileExists(atPath: url.path) {
                            reply(.success(Data(url.path.utf8)))
                            return
                        }
                        // A video's is a frame of it: PhotoKit's own poster.
                        let po = PHImageRequestOptions()
                        po.deliveryMode = .highQualityFormat
                        po.resizeMode = .fast
                        po.isNetworkAccessAllowed = true
                        po.isSynchronous = true
                        var image: UIImage?
                        PHImageManager.default().requestImage(for: a, targetSize: CGSize(width: size, height: size), contentMode: .aspectFill, options: po) { img, _ in image = img }
                        guard let data = image?.jpegData(compressionQuality: 0.8) else { throw AsukaModuleError("no thumbnail") }
                        try data.write(to: url, options: .atomic)
                        reply(.success(Data(url.path.utf8)))
                    } catch {
                        reply(.failure(error))
                    }
                }
            },
            "export": { args, reply in
                let o = object(args)
                DispatchQueue.global(qos: .userInitiated).async {
                    do {
                        let a = try asset(o["id"] as? String ?? "")
                        let rs = PHAssetResource.assetResources(for: a)
                        func find(_ t: PHAssetResourceType) -> PHAssetResource? { rs.first { $0.type == t } }
                        // The photo or the video as edited, if it was.
                        let r: PHAssetResource?
                        let original: PHAssetResource?
                        if a.mediaType == .video {
                            original = find(.video)
                            r = find(.fullSizeVideo) ?? original ?? rs.first
                        } else {
                            original = find(.photo)
                            r = find(.fullSizePhoto) ?? original ?? rs.first
                        }
                        guard let r else { throw AsukaModuleError("no such photo") }
                        let name = original?.originalFilename ?? r.originalFilename
                        var ext = (r.originalFilename as NSString).pathExtension
                        if ext.isEmpty { ext = a.mediaType == .video ? "mov" : "jpg" }
                        let url = try cacheDir("asuka-photo-exports").appendingPathComponent("\(UUID().uuidString).\(ext)")
                        let wo = PHAssetResourceRequestOptions()
                        wo.isNetworkAccessAllowed = true
                        PHAssetResourceManager.default().writeData(for: r, toFile: url, options: wo) { error in
                            if let error {
                                reply(.failure(error))
                                return
                            }
                            reply(Result { try JSONSerialization.data(withJSONObject: ["path": url.path, "name": name]) })
                        }
                    } catch {
                        reply(.failure(error))
                    }
                }
            },
            "albums": { _, reply in
                DispatchQueue.global(qos: .userInitiated).async {
                    reply(Result { try JSONSerialization.data(withJSONObject: readable() ? albums() : []) })
                }
            },
            "createAlbum": { args, reply in
                guard readable() else { reply(.failure(AsukaModuleError("not allowed"))); return }
                let title = object(args)["title"] as? String ?? ""
                var id = ""
                PHPhotoLibrary.shared().performChanges({
                    id = PHAssetCollectionChangeRequest.creationRequestForAssetCollection(withTitle: title).placeholderForCreatedAssetCollection.localIdentifier
                }) { ok, e in
                    guard ok else { reply(.failure(e ?? AsukaModuleError("couldn't make the album"))); return }
                    reply(Result { try JSONSerialization.data(withJSONObject: ["id": id, "title": title, "count": 0, "system": false] as [String: Any]) })
                }
            },
            "addToAlbum": { args, reply in
                guard readable() else { reply(.failure(AsukaModuleError("not allowed"))); return }
                let o = object(args)
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
                do { found = try assets(ids) } catch { reply(.failure(error)); return }
                if ids.isEmpty { reply(.success(Data())); return }
                PHPhotoLibrary.shared().performChanges({
                    PHAssetCollectionChangeRequest(for: c)?.addAssets(found)
                }) { ok, e in
                    reply(ok ? .success(Data()) : .failure(e ?? AsukaModuleError("couldn't add to the album")))
                }
            },
            "delete": { args, reply in
                guard readable() else { reply(.failure(AsukaModuleError("not allowed"))); return }
                let ids = object(args)["ids"] as? [String] ?? []
                let found: PHFetchResult<PHAsset>
                do { found = try assets(ids) } catch { reply(.failure(error)); return }
                if ids.isEmpty { reply(.success(Data("true".utf8))); return }
                // The system asks the user first.
                PHPhotoLibrary.shared().performChanges({
                    PHAssetChangeRequest.deleteAssets(found)
                }) { ok, e in
                    if ok { reply(.success(Data("true".utf8))) } else if cancelled(e) { reply(.success(Data("false".utf8))) } else {
                        reply(.failure(e ?? AsukaModuleError("couldn't delete")))
                    }
                }
            },
            "nextChange": { _, reply in AsukaPhotoChanges.shared.next(reply) },
            "stop": { args, reply in
                if String(decoding: args, as: UTF8.self) == "nextChange" { AsukaPhotoChanges.shared.stop() }
                reply(.success(Data()))
            },
        ]
    }
}

/// The library's changes, for "nextChange": PhotoKit's observer, registered
/// once someone waits (and the app may read the library), with a fetch of
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

    /// Registers, once it's wanted and the library can be read (a request
    /// allowed it: registering before would ask).
    func start() {
        guard wanted, !registered, AsukaPhotos.readable() else { return }
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
#endif
