// platform/car's navigation map on CarPlay: the CarPlay window's root view
// controller, an MKMapView under the CPMapTemplate, drawn from the app's
// car.Map (as platform/maps' MapView draws maps.View: its pins, its
// region): where it looks (the car's position, followed and turned
// heading up, or a center), at a zoom, the route as a line, the markers,
// the car as a puck, day or night. What CarPlay's chrome covers (the
// guidance card, the buttons) is the window's safe area: the car is
// centered in what's left. The driver moves and zooms it with the map
// template's buttons and pan; it stays where they took it until they
// recenter or the app moves it.
import Asuka
import CarPlay
import Foundation
import MapKit
import UIKit

@MainActor final class CarMapController: UIViewController, MKMapViewDelegate {
    let map = MKMapView()
    /// What the app gave, last.
    private(set) var given = Map(zoom: 15, follow: true)
    private var givenOnce = false
    /// Where the driver took it (none: where the app says), and the zoom
    /// they chose (none: the app's).
    private(set) var driverCenter: CLLocationCoordinate2D?
    private(set) var driverZoom: Double?
    /// The routes previewed, and the one highlighted (none: no preview).
    private var previews: [[Coordinate]] = []
    private var highlighted = 0
    private let puck = Puck()
    private var pins: [CarPin] = []
    private var tiles: MKTileOverlay?
    private var lines: [RouteLine] = []
    /// Said when the driver's moves change: the recenter button follows.
    var moved: (Bool) -> Void = { _ in }
    /// A size for the map before it's in a window (a test's).
    var size: CGSize?

    override func loadView() {
        map.delegate = self
        map.showsCompass = false
        map.showsScale = false
        map.pointOfInterestFilter = .excludingAll
        map.isRotateEnabled = false
        map.isPitchEnabled = false
        view = map
    }

    override func viewSafeAreaInsetsDidChange() {
        super.viewSafeAreaInsetsDidChange()
        place(animated: true)
    }

    override func viewDidLayoutSubviews() {
        super.viewDidLayoutSubviews()
        place(animated: false)
    }

    // MARK: The app's map

    /// Shows m: its content, and where it looks, unless the driver took
    /// it elsewhere and the app hasn't moved it since.
    func show(_ m: Map) {
        loadViewIfNeeded()
        let was = given
        given = m
        if givenOnce {
            // The app's own move takes it back from the driver.
            if m.zoom != was.zoom { driverZoom = nil }
            if m.follow != was.follow || (!m.follow && !same(m.center, was.center)) { driverCenter = nil }
        }
        givenOnce = true
        content()
        place(animated: true)
        moved(driverCenter != nil || driverZoom != nil)
    }

    /// The routes of a preview (the highlighted one drawn over the others),
    /// framed; [] ends it.
    func preview(_ paths: [[Coordinate]], highlighted i: Int) {
        loadViewIfNeeded()
        previews = paths
        highlighted = i
        content()
        place(animated: true)
    }

    private func same(_ a: Coordinate, _ b: Coordinate) -> Bool { a.lat == b.lat && a.lng == b.lng }

    /// The overlays and annotations: tiles, the route or the previews'
    /// lines, the markers, the puck.
    private func content() {
        map.overrideUserInterfaceStyle = given.night.map { $0 ? .dark : .light } ?? .unspecified
        if (tiles?.urlTemplate ?? "") != given.tiles {
            if let t = tiles { map.removeOverlay(t) }
            tiles = nil
            if !given.tiles.isEmpty {
                let t = MKTileOverlay(urlTemplate: given.tiles)
                t.canReplaceMapContent = true
                map.insertOverlay(t, at: 0, level: .aboveLabels)
                tiles = t
            }
        }
        map.removeOverlays(lines)
        lines = []
        if previews.isEmpty {
            if given.route.count > 1 { lines.append(RouteLine(given.route, .route)) }
        } else {
            for (i, p) in previews.enumerated() where i != highlighted && p.count > 1 { lines.append(RouteLine(p, .other)) }
            if highlighted < previews.count, previews[highlighted].count > 1 { lines.append(RouteLine(previews[highlighted], .route)) }
        }
        map.addOverlays(lines, level: .aboveRoads)
        map.removeAnnotations(pins)
        pins = given.markers.map(CarPin.init)
        map.addAnnotations(pins)
        if let p = given.position {
            puck.coordinate = CLLocationCoordinate2D(latitude: p.lat, longitude: p.lng)
            if !map.annotations.contains(where: { $0 === puck }) { map.addAnnotation(puck) }
            turnPuck()
        } else {
            map.removeAnnotation(puck)
        }
    }

    // MARK: Where it looks

    private var bounds: CGRect {
        if let s = size { return CGRect(origin: .zero, size: s) }
        return map.bounds
    }

    /// The zoom shown (web maps' levels).
    var zoom: Double {
        let w = bounds.width
        guard w > 0, map.visibleMapRect.width > 0 else { return driverZoom ?? given.zoom }
        return 20 - log2(map.visibleMapRect.width / Double(w))
    }

    /// The point the map centers on: the driver's, the car's while
    /// following it, else the app's center.
    var target: CLLocationCoordinate2D {
        if let c = driverCenter { return c }
        if given.follow, let p = given.position { return CLLocationCoordinate2D(latitude: p.lat, longitude: p.lng) }
        return CLLocationCoordinate2D(latitude: given.center.lat, longitude: given.center.lng)
    }

    /// The heading up: the car's, while following it with one.
    var headingUp: Double {
        if driverCenter == nil, previews.isEmpty, given.follow, given.position != nil, let h = given.heading { return h }
        return 0
    }

    /// Moves the map to what it shows now: the previews framed, or the
    /// target in the middle of the safe area, at the zoom, heading up.
    func place(animated: Bool) {
        guard isViewLoaded, bounds.width > 0, bounds.height > 0, givenOnce else { return }
        let insets = view.safeAreaInsets
        if !previews.isEmpty {
            var r = MKMapRect.null
            for p in previews {
                for c in p { r = r.union(MKMapRect(origin: MKMapPoint(CLLocationCoordinate2D(latitude: c.lat, longitude: c.lng)), size: MKMapSize(width: 0, height: 0))) }
            }
            if !r.isNull {
                let pad = UIEdgeInsets(top: insets.top + 24, left: insets.left + 24, bottom: insets.bottom + 24, right: insets.right + 24)
                map.setVisibleMapRect(r, edgePadding: pad, animated: animated)
            }
            return
        }
        let z = min(max(driverZoom ?? given.zoom, 1), 20)
        let scale = pow(2, 20 - z) // map points a screen point
        let b = bounds
        // The safe area's middle, from the view's.
        let dx = Double((insets.left - insets.right) / 2), dy = Double((insets.top - insets.bottom) / 2)
        let h = headingUp * .pi / 180
        // A screen vector in map points, the map turned h: right is (cos, sin), down (-sin, cos).
        let mx = (dx * cos(h) - dy * sin(h)) * scale, my = (dx * sin(h) + dy * cos(h)) * scale
        let t = MKMapPoint(target)
        let mid = MKMapPoint(x: t.x - mx, y: t.y - my)
        let size = MKMapSize(width: Double(b.width) * scale, height: Double(b.height) * scale)
        let rect = MKMapRect(origin: MKMapPoint(x: mid.x - size.width / 2, y: mid.y - size.height / 2), size: size)
        if headingUp == 0 {
            if abs(map.camera.heading) > 0.01 {
                let c = map.camera.copy() as! MKMapCamera
                c.heading = 0
                map.setCamera(c, animated: false)
            }
            map.setVisibleMapRect(rect, animated: animated && map.window != nil)
        } else {
            // The camera's distance for that rect, found north up and put
            // back before anything's drawn; then the camera turned.
            let saved = map.camera.copy() as! MKMapCamera
            map.setVisibleMapRect(rect, animated: false)
            let d = map.camera.centerCoordinateDistance
            map.camera = saved
            map.setCamera(MKMapCamera(lookingAtCenter: mid.coordinate, fromDistance: d, pitch: 0, heading: headingUp), animated: animated && map.window != nil)
        }
        turnPuck()
    }

    // MARK: The driver's

    func zoom(by step: Double) {
        driverZoom = min(max((driverZoom ?? given.zoom) + step, 1), 20)
        place(animated: true)
        moved(true)
    }

    /// Back to where the app says.
    func recenter() {
        driverCenter = nil
        driverZoom = nil
        place(animated: true)
        moved(false)
    }

    private var panFrom: MKMapPoint?

    func panBegan() { panFrom = MKMapPoint(target) }

    /// The map moved by translation screen points since the pan began
    /// (the content goes with the finger).
    func pan(translation: CGPoint) {
        let from = panFrom ?? MKMapPoint(target)
        let scale = pow(2, 20 - (driverZoom ?? given.zoom))
        let h = map.camera.heading * .pi / 180
        let tx = Double(translation.x), ty = Double(translation.y)
        let mx = (tx * cos(h) - ty * sin(h)) * scale, my = (tx * sin(h) + ty * cos(h)) * scale
        driverCenter = MKMapPoint(x: from.x - mx, y: from.y - my).coordinate
        place(animated: false)
        moved(true)
    }

    func panEnded() { panFrom = nil }

    /// A pan button's step: a quarter of the map that way.
    func pan(_ d: CPMapTemplate.PanDirection) {
        let b = bounds
        var t = CGPoint.zero
        if d.contains(.left) { t.x += b.width / 4 }
        if d.contains(.right) { t.x -= b.width / 4 }
        if d.contains(.up) { t.y += b.height / 4 }
        if d.contains(.down) { t.y -= b.height / 4 }
        panBegan()
        pan(translation: t)
        panEnded()
    }

    // MARK: MKMapViewDelegate

    private func turnPuck() {
        guard let v = map.view(for: puck) else { return }
        let h = (given.heading ?? 0) - map.camera.heading
        v.transform = CGAffineTransform(rotationAngle: CGFloat(h * .pi / 180))
        v.image = Puck.image(heading: given.heading != nil)
    }

    nonisolated func mapView(_ m: MKMapView, regionDidChangeAnimated animated: Bool) {
        MainActor.assumeIsolated { turnPuck() }
    }

    nonisolated func mapView(_ m: MKMapView, rendererFor overlay: MKOverlay) -> MKOverlayRenderer {
        MainActor.assumeIsolated {
            if let t = overlay as? MKTileOverlay { return MKTileOverlayRenderer(tileOverlay: t) }
            guard let l = overlay as? RouteLine else { return MKOverlayRenderer(overlay: overlay) }
            let r = MKPolylineRenderer(polyline: l)
            r.lineCap = .round
            r.lineJoin = .round
            switch l.kind {
            case .route:
                r.strokeColor = .systemBlue
                r.lineWidth = 8
            case .other:
                r.strokeColor = UIColor.systemGray.withAlphaComponent(0.8)
                r.lineWidth = 6
            }
            return r
        }
    }

    nonisolated func mapView(_ m: MKMapView, viewFor annotation: MKAnnotation) -> MKAnnotationView? {
        MainActor.assumeIsolated {
            if annotation === puck {
                let v = map.dequeueReusableAnnotationView(withIdentifier: "asuka.puck") ?? MKAnnotationView(annotation: annotation, reuseIdentifier: "asuka.puck")
                v.annotation = annotation
                v.image = Puck.image(heading: given.heading != nil)
                v.zPriority = .max
                let h = (given.heading ?? 0) - map.camera.heading
                v.transform = CGAffineTransform(rotationAngle: CGFloat(h * .pi / 180))
                return v
            }
            guard let a = annotation as? CarPin else { return nil }
            let v = map.dequeueReusableAnnotationView(withIdentifier: "asuka.pin") as? MKMarkerAnnotationView
                ?? MKMarkerAnnotationView(annotation: a, reuseIdentifier: "asuka.pin")
            v.annotation = a
            v.markerTintColor = a.color
            v.titleVisibility = .visible
            return v
        }
    }

    // MARK: What a test reads

    /// The lines drawn: each one's points, and whether it's the route (or
    /// a preview's highlighted one).
    var drawn: [(points: Int, route: Bool)] { lines.map { ($0.pointCount, $0.kind == .route) } }
    var markerTitles: [String] { pins.compactMap { $0.title } }
    var puckShown: Bool { map.annotations.contains { $0 === puck } }
}

/// A line on the map: the route (or the preview highlighted), or another
/// route of a preview.
final class RouteLine: MKPolyline {
    enum Kind { case route, other }
    private(set) var kind: Kind = .route

    convenience init(_ points: [Coordinate], _ kind: Kind) {
        let cs = points.map { CLLocationCoordinate2D(latitude: $0.lat, longitude: $0.lng) }
        self.init(coordinates: cs, count: cs.count)
        self.kind = kind
    }
}

/// A marker of the app's.
final class CarPin: NSObject, MKAnnotation {
    let coordinate: CLLocationCoordinate2D
    let title: String?
    let color: UIColor?

    init(_ m: Marker) {
        coordinate = CLLocationCoordinate2D(latitude: m.at.lat, longitude: m.at.lng)
        title = m.title.isEmpty ? nil : m.title
        color = m.color.flatMap(AsukaColors.color)
    }
}

/// The car on the map: a blue disc with a white rim, an arrow on it when
/// the heading's known (the view turns it).
final class Puck: NSObject, MKAnnotation {
    @objc dynamic var coordinate = CLLocationCoordinate2D()

    @MainActor private static var images: [Bool: UIImage] = [:]

    @MainActor static func image(heading: Bool) -> UIImage {
        if let i = images[heading] { return i }
        let side: CGFloat = 30
        let img = UIGraphicsImageRenderer(size: CGSize(width: side, height: side)).image { ctx in
            let c = ctx.cgContext
            let r = CGRect(x: 2, y: 2, width: side - 4, height: side - 4)
            c.setShadow(offset: .zero, blur: 3, color: UIColor.black.withAlphaComponent(0.35).cgColor)
            c.setFillColor(UIColor.white.cgColor)
            c.fillEllipse(in: r)
            c.setShadow(offset: .zero, blur: 0, color: nil)
            c.setFillColor(UIColor.systemBlue.cgColor)
            c.fillEllipse(in: r.insetBy(dx: 3, dy: 3))
            if heading {
                c.setFillColor(UIColor.white.cgColor)
                c.move(to: CGPoint(x: side / 2, y: 7))
                c.addLine(to: CGPoint(x: side - 9, y: side - 8))
                c.addLine(to: CGPoint(x: side / 2, y: side - 11))
                c.addLine(to: CGPoint(x: 9, y: side - 8))
                c.closePath()
                c.fillPath()
            }
        }
        images[heading] = img
        return img
    }
}
