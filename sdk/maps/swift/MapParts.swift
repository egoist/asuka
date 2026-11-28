// platform/maps's parts on iOS, macOS and tvOS: the map view, a marker's
// annotation, its images, the camera's arithmetic, and the Maps the
// package's functions call (moveTo, moveCamera, fit, camera).
import Asuka
import Foundation
import MapKit
#if canImport(UIKit)
import UIKit
typealias PlatformImage = UIImage
#else
import AppKit
typealias PlatformImage = NSImage
#endif

/// The map view: it has no size of its own (its style gives it one, or
/// grow), and lets its own gestures and the package's recognize together.
#if canImport(UIKit)
final class PlatformMap: MKMapView, UIGestureRecognizerDelegate {
    weak var owner: MapView?
    override func sizeThatFits(_ size: CGSize) -> CGSize { .zero }
    override func layoutSubviews() {
        super.layoutSubviews()
        owner?.laidOut()
    }
    func gestureRecognizer(_ g: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }
}
#else
final class PlatformMap: MKMapView {
    weak var owner: MapView?
    override var fittingSize: NSSize { .zero }
    override func layout() {
        super.layout()
        owner?.laidOut()
    }
}
#endif

/// A marker on the map: its coordinate and texts follow a new list of
/// markers in place (KVO), so it stays selected or dragged.
final class Pin: NSObject, MKAnnotation {
    private(set) var marker: Marker
    @objc dynamic var coordinate: CLLocationCoordinate2D
    @objc dynamic var title: String?
    @objc dynamic var subtitle: String?
    var image: PlatformImage?

    init(_ m: Marker) {
        marker = m
        coordinate = CLLocationCoordinate2D(latitude: m.at.lat, longitude: m.at.lng)
        title = m.title.isEmpty ? nil : m.title
        subtitle = m.subtitle.isEmpty ? nil : m.subtitle
    }

    /// What its view is made of: a change makes it again.
    func look(_ m: Marker, _ clustering: Bool) -> String {
        "\(m.image)|\(m.width)|\(m.height)|\(m.anchor.x)|\(m.anchor.y)|\(m.color ?? -1)|\(m.draggable)|\(m.zIndex)|\(clustering && m.clusters)|\(m.title.isEmpty && m.subtitle.isEmpty)"
    }

    func update(_ m: Marker) {
        marker = m
        let c = CLLocationCoordinate2D(latitude: m.at.lat, longitude: m.at.lng)
        if c.latitude != coordinate.latitude || c.longitude != coordinate.longitude { coordinate = c }
        let t = m.title.isEmpty ? nil : m.title, s = m.subtitle.isEmpty ? nil : m.subtitle
        if t != title { title = t }
        if s != subtitle { subtitle = s }
    }
}

/// Markers' images: an asset, a file, a URL (downloaded once, kept).
@MainActor enum MapImages {
    private static var cache: [String: PlatformImage] = [:]
    private static var waiting: [String: [(PlatformImage?) -> Void]] = [:]

    static func image(_ source: String, _ done: @escaping (PlatformImage?) -> Void) {
        if let i = cache[source] { return done(i) }
        guard let url = AsukaAssets.url(source) else { return done(nil) }
        if url.isFileURL {
            #if canImport(UIKit)
            let i = UIImage(contentsOfFile: url.path)
            #else
            let i = NSImage(contentsOf: url)
            #endif
            if let i { cache[source] = i }
            return done(i)
        }
        if waiting[source] != nil {
            waiting[source]?.append(done)
            return
        }
        waiting[source] = [done]
        URLSession.shared.dataTask(with: url) { data, _, _ in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    var i: PlatformImage?
                    if let data {
                        #if canImport(UIKit)
                        // A URL's pixels, as points at the screen's scale.
                        i = UIImage(data: data, scale: scale)
                        #else
                        i = NSImage(data: data)
                        if let i, let rep = i.representations.first, rep.pixelsWide > 0 {
                            i.size = NSSize(width: CGFloat(rep.pixelsWide) / scale, height: CGFloat(rep.pixelsHigh) / scale)
                        }
                        #endif
                    }
                    if let i { cache[source] = i }
                    for d in waiting.removeValue(forKey: source) ?? [] { d(i) }
                }
            }
        }.resume()
    }

    private static var scale: CGFloat {
        #if os(tvOS) || os(iOS)
        UIScreen.main.scale
        #elseif canImport(UIKit)
        2
        #else
        NSScreen.main?.backingScaleFactor ?? 2
        #endif
    }

    /// The size a marker's image shows at: width and height, one of them
    /// (its shape kept), or its own.
    static func size(_ i: PlatformImage, _ w: Double, _ h: Double) -> CGSize {
        let s = i.size
        guard s.width > 0, s.height > 0 else { return s }
        if w > 0 && h > 0 { return CGSize(width: w, height: h) }
        if w > 0 { return CGSize(width: w, height: w * s.height / s.width) }
        if h > 0 { return CGSize(width: h * s.width / s.height, height: h) }
        return s
    }

    static func resized(_ i: PlatformImage, _ size: CGSize) -> PlatformImage {
        if i.size == size { return i }
        #if canImport(UIKit)
        return UIGraphicsImageRenderer(size: size).image { _ in i.draw(in: CGRect(origin: .zero, size: size)) }
        #else
        let copy = i.copy() as! NSImage
        copy.size = size
        return copy
        #endif
    }
}

/// The camera's arithmetic: Web Mercator's zoom (the world 256 points
/// wide at 0) as MapKit's distance from the ground, and back.
enum MapMath {
    /// The vertical field of view MapKit's camera sees with.
    static let fov = 30.0 * .pi / 180

    static func distance(zoom: Double, lat: Double, height: Double) -> Double {
        let perPoint = 156543.03392 * cos(lat * .pi / 180) / pow(2, zoom)
        return perPoint * height / (2 * tan(fov / 2))
    }

    static func zoom(distance: Double, lat: Double, height: Double) -> Double {
        let perPoint = distance * 2 * tan(fov / 2) / height
        return max(log2(156543.03392 * cos(lat * .pi / 180) / perPoint), 0)
    }

    /// The distance from p to the segment a–b, in points.
    static func segmentDistance(_ p: CGPoint, _ a: CGPoint, _ b: CGPoint) -> Double {
        let dx = b.x - a.x, dy = b.y - a.y
        let len = dx * dx + dy * dy
        var t = len > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len : 0
        t = max(0, min(1, t))
        return Double(hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy)))
    }
}

/// The Maps the package's functions call: each finds its map by the Ref
/// it's in.
final class MapsControl: Maps {
    @MainActor private func find(_ ref: AsukaViewRef) -> MapView? { Self.find(ref.view) }

    @MainActor private static func find(_ v: AsukaPlatformView?) -> MapView? {
        guard let v else { return nil }
        if let m = v as? PlatformMap { return m.owner }
        for s in v.subviews {
            if let m = find(s) { return m }
        }
        return nil
    }

    @MainActor func moveTo(view: AsukaViewRef, region: Region, animated: Bool) throws {
        find(view)?.moveTo(region, animated: animated)
    }

    @MainActor func moveCamera(view: AsukaViewRef, camera: Camera, animated: Bool) throws {
        guard camera.zoom > 0 else { return }
        find(view)?.moveCamera(camera, animated: animated)
    }

    @MainActor func fit(view: AsukaViewRef, coordinates: [Coordinate], padding: Insets, animated: Bool) throws {
        find(view)?.fit(coordinates, padding: padding, animated: animated)
    }

    @MainActor func camera(view: AsukaViewRef) throws -> Camera {
        find(view)?.camera() ?? Camera()
    }
}

func makeMaps() throws -> any Maps { MapsControl() }
