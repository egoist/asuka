// platform/maps on iOS, macOS and tvOS: an MKMapView. The region or
// camera set moves it (animated once it's on screen), unless it's
// already there (the user's move, bound and given back); regionChange
// and cameraChange are the user's moves only, cameraMove and cameraIdle
// every move. Markers are kept by id, so a selection or a drag survives
// a new list; MapKit clusters the markers (clusteringIdentifier) and
// draws the overlays (MKPolyline, MKGeodesicPolyline, MKPolygon,
// MKCircle), which the package hit-tests for overlayPress.
import Asuka
import Foundation
import MapKit
#if canImport(UIKit)
import UIKit
#else
import AppKit
#endif

@MainActor final class MapView: NSObject, View, MKMapViewDelegate {
    let events: ViewEvents
    let map = PlatformMap()
    var view: AsukaPlatformView { map }
    private var setting = false // moving the map for a prop or a call: not the user's move
    private var userMoving = false
    private var location: CLLocationManager?

    // Markers, by id, and their order in markers (clusterPress's ids).
    private var pins: [String: Pin] = [:]
    private var order: [String: Int] = [:]
    private var clustering = false
    private var clusterColor: AsukaPlatformColor?
    private var selected = ""
    private var selecting: String? // selected by the app: no events
    private var dragTimer: Timer?

    // Overlays, in the order they're drawn, with how.
    private var polylines: [Polyline] = []
    private var polygons: [Polygon] = []
    private var circles: [Circle] = []
    private var drawn: [(overlay: MKOverlay, look: Look)] = []
    private var looks: [ObjectIdentifier: Look] = [:]

    private var pendingCamera: (Camera, Bool)?

    init(_ events: ViewEvents) {
        self.events = events
        super.init()
        map.owner = self
        map.delegate = self
        #if canImport(UIKit)
        let tap = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        tap.delegate = map
        map.addGestureRecognizer(tap)
        #if !os(tvOS)
        let long = UILongPressGestureRecognizer(target: self, action: #selector(longPressed(_:)))
        long.delegate = map
        map.addGestureRecognizer(long)
        // A long press isn't a tap too.
        tap.require(toFail: long)
        #endif
        #else
        let click = NSClickGestureRecognizer(target: self, action: #selector(clicked(_:)))
        click.delaysPrimaryMouseButtonEvents = false
        map.addGestureRecognizer(click)
        let right = NSClickGestureRecognizer(target: self, action: #selector(rightClicked(_:)))
        right.buttonMask = 0x2
        map.addGestureRecognizer(right)
        #endif
    }

    // MARK: - Where it shows

    func setRegion(_ r: Region) {
        guard r.latDelta > 0 || r.lngDelta > 0 || r.center.lat != 0 || r.center.lng != 0 else { return }
        moveTo(r, animated: map.window != nil)
    }

    func moveTo(_ r: Region, animated: Bool) {
        let center = CLLocationCoordinate2D(latitude: r.center.lat, longitude: r.center.lng)
        let span = MKCoordinateSpan(latitudeDelta: max(r.latDelta > 0 ? r.latDelta : 0.05, 0.0005), longitudeDelta: max(r.lngDelta > 0 ? r.lngDelta : 0.05, 0.0005))
        // The region the user moved to, given back: already there.
        let cur = map.region
        if abs(cur.center.latitude - center.latitude) < 1e-6 && abs(cur.center.longitude - center.longitude) < 1e-6
            && abs(cur.span.latitudeDelta - span.latitudeDelta) < 1e-6 { return }
        pendingCamera = nil
        setting = true
        map.setRegion(MKCoordinateRegion(center: center, span: span), animated: animated)
    }

    func setCamera(_ c: Camera) {
        guard c.zoom > 0 else { return }
        moveCamera(c, animated: map.window != nil)
    }

    func moveCamera(_ c: Camera, animated: Bool) {
        let h = map.bounds.height
        guard h > 0 else {
            // Its distance needs the map's size: once it's laid out.
            pendingCamera = (c, false)
            return
        }
        pendingCamera = nil
        let now = camera()
        if abs(now.center.lat - c.center.lat) < 1e-6 && abs(now.center.lng - c.center.lng) < 1e-6 && abs(now.zoom - c.zoom) < 0.01
            && abs(angle(now.heading - c.heading)) < 0.5 && abs(now.pitch - c.pitch) < 0.5 { return }
        let center = CLLocationCoordinate2D(latitude: c.center.lat, longitude: c.center.lng)
        let cam = MKMapCamera(lookingAtCenter: center, fromDistance: MapMath.distance(zoom: c.zoom, lat: c.center.lat, height: Double(h)),
                              pitch: CGFloat(max(c.pitch, 0)), heading: c.heading)
        setting = true
        map.setCamera(cam, animated: animated)
    }

    /// Shows every coordinate as near as it can with padding's room, the
    /// heading kept, flat.
    func fit(_ cs: [Coordinate], padding p: Insets, animated: Bool) {
        guard !cs.isEmpty else { return }
        let w = Double(map.bounds.width), h = Double(map.bounds.height)
        let heading = map.camera.heading
        let pts = cs.map { MKMapPoint(CLLocationCoordinate2D(latitude: $0.lat, longitude: $0.lng)) }
        // The points as the screen turns them (heading), around the first.
        let t = heading * .pi / 180, cosT = cos(t), sinT = sin(t)
        let o = pts[0]
        var minX = 0.0, maxX = 0.0, minY = 0.0, maxY = 0.0
        for m in pts {
            var dx = m.x - o.x
            let world = MKMapSize.world.width
            if dx > world / 2 { dx -= world } else if dx < -world / 2 { dx += world }
            let dy = m.y - o.y
            let x = dx * cosT + dy * sinT, y = -dx * sinT + dy * cosT
            minX = min(minX, x); maxX = max(maxX, x); minY = min(minY, y); maxY = max(maxY, y)
        }
        let spanX = maxX - minX, spanY = maxY - minY
        // Map points per point at zoom z: world / (256 · 2^z).
        let availW = max(w - p.left - p.right, 1), availH = max(h - p.top - p.bottom, 1)
        var zoom = 15.0
        if spanX > 0 || spanY > 0 {
            let world = MKMapSize.world.width
            var z = 20.0
            if spanX > 0 { z = min(z, log2(availW * world / (256 * spanX))) }
            if spanY > 0 { z = min(z, log2(availH * world / (256 * spanY))) }
            zoom = max(z, 0)
        }
        let perPoint = MKMapSize.world.width / (256 * pow(2, zoom))
        // The middle of what's fitted, moved so it's the padded box's middle.
        let cx = (minX + maxX) / 2 - (p.left - p.right) / 2 * perPoint
        let cy = (minY + maxY) / 2 - (p.top - p.bottom) / 2 * perPoint
        let mx = o.x + cx * cosT - cy * sinT, my = o.y + cx * sinT + cy * cosT
        let c = MKMapPoint(x: mx, y: my).coordinate
        moveCamera(Camera(center: Coordinate(lat: c.latitude, lng: c.longitude), zoom: zoom, heading: heading, pitch: 0), animated: animated)
    }

    func camera() -> Camera {
        let c = map.camera
        let h = Double(map.bounds.height)
        var zoom = 0.0
        if h > 0 && c.centerCoordinateDistance > 0 {
            zoom = MapMath.zoom(distance: c.centerCoordinateDistance, lat: c.centerCoordinate.latitude, height: h)
        }
        // The heading in [0, 360), without a -0.
        var heading = c.heading.truncatingRemainder(dividingBy: 360)
        if heading < 0 { heading += 360 }
        if abs(heading) < 1e-9 || abs(heading - 360) < 1e-9 { heading = 0 }
        return Camera(center: Coordinate(lat: c.centerCoordinate.latitude, lng: c.centerCoordinate.longitude),
                      zoom: zoom, heading: heading, pitch: Double(c.pitch))
    }

    private func regionNow() -> Region {
        let r = map.region
        return Region(center: Coordinate(lat: r.center.latitude, lng: r.center.longitude), latDelta: r.span.latitudeDelta, lngDelta: r.span.longitudeDelta)
    }

    /// The map was laid out: a camera waiting for its size moves it.
    func laidOut() {
        if let (c, _) = pendingCamera, map.bounds.height > 0 { moveCamera(c, animated: false) }
    }

    private func angle(_ d: Double) -> Double {
        var a = d.truncatingRemainder(dividingBy: 360)
        if a > 180 { a -= 360 } else if a < -180 { a += 360 }
        return a
    }

    // MARK: - Markers

    func setMarkers(_ list: [Marker]) {
        var keep = Set<String>()
        var add: [Pin] = []
        var remove: [Pin] = []
        order = [:]
        for (i, m) in list.enumerated() where order[m.id] == nil {
            order[m.id] = i
            keep.insert(m.id)
            if let p = pins[m.id] {
                if p.look(m, clustering) != p.look(p.marker, clustering) {
                    // How it's drawn changed: a view made again.
                    remove.append(p)
                    let n = Pin(m)
                    pins[m.id] = n
                    add.append(n)
                } else {
                    p.update(m)
                }
            } else {
                let n = Pin(m)
                pins[m.id] = n
                add.append(n)
            }
        }
        for (id, p) in pins where !keep.contains(id) {
            remove.append(p)
            pins[id] = nil
        }
        if !remove.isEmpty { map.removeAnnotations(remove) }
        if !add.isEmpty { map.addAnnotations(add) }
        for p in add { load(p) }
        reselect()
    }

    /// Loads a pin's image: at once from a file, an asset; a URL's, as
    /// it comes.
    private func load(_ p: Pin) {
        guard !p.marker.image.isEmpty else { return }
        MapImages.image(p.marker.image) { [weak self, weak p] img in
            guard let self, let p, self.pins[p.marker.id] === p else { return }
            p.image = img
            if let v = self.map.view(for: p) { self.configure(v, p) }
        }
    }

    func setSelected(_ id: String) {
        selected = id
        reselect()
    }

    /// Shows the selection the app set.
    private func reselect() {
        let cur = map.selectedAnnotations.compactMap { $0 as? Pin }.first
        if selected.isEmpty {
            if let cur {
                selecting = ""
                map.deselectAnnotation(cur, animated: true)
            }
            return
        }
        guard let p = pins[selected], cur !== p else { return }
        selecting = selected
        map.selectAnnotation(p, animated: true)
    }

    func setClustering(_ on: Bool) {
        guard on != clustering else { return }
        clustering = on
        // Their views take the identifier again.
        let all = Array(pins.values)
        map.removeAnnotations(all)
        map.addAnnotations(all)
        reselect()
    }

    func setClusterColor(_ v: Int?) {
        clusterColor = v.flatMap(AsukaColors.color)
        for a in map.annotations {
            if let c = a as? MKClusterAnnotation, let v = map.view(for: c) as? MKMarkerAnnotationView { v.markerTintColor = clusterColor }
        }
    }

    // MARK: - Overlays

    func setPolylines(_ v: [Polyline]) { polylines = v; drawOverlays() }
    func setPolygons(_ v: [Polygon]) { polygons = v; drawOverlays() }
    func setCircles(_ v: [Circle]) { circles = v; drawOverlays() }

    /// Draws the overlays again, by zIndex: polygons, then circles, then
    /// lines where it's the same.
    private func drawOverlays() {
        var all: [(z: Double, kind: Int, i: Int, overlay: MKOverlay, look: Look)] = []
        for (i, p) in polygons.enumerated() where p.points.count >= 3 {
            let holes = p.holes.filter { $0.count >= 3 }.map { h in coords(h).withUnsafeBufferPointer { MKPolygon(coordinates: $0.baseAddress!, count: h.count) } }
            let o = coords(p.points).withUnsafeBufferPointer { MKPolygon(coordinates: $0.baseAddress!, count: p.points.count, interiorPolygons: holes) }
            all.append((p.zIndex, 0, i, o, Look(id: p.id, kind: .polygon, fill: AsukaColors.color(p.fill), stroke: AsukaColors.color(p.stroke), width: p.strokeWidth, dashed: false)))
        }
        for (i, c) in circles.enumerated() where c.radius > 0 {
            let o = MKCircle(center: CLLocationCoordinate2D(latitude: c.center.lat, longitude: c.center.lng), radius: c.radius)
            all.append((c.zIndex, 1, i, o, Look(id: c.id, kind: .circle, fill: AsukaColors.color(c.fill), stroke: AsukaColors.color(c.stroke), width: c.strokeWidth, dashed: false)))
        }
        for (i, l) in polylines.enumerated() where l.points.count >= 2 {
            let cs = coords(l.points)
            let o: MKPolyline = cs.withUnsafeBufferPointer { b in
                l.geodesic ? MKGeodesicPolyline(coordinates: b.baseAddress!, count: cs.count) : MKPolyline(coordinates: b.baseAddress!, count: cs.count)
            }
            all.append((l.zIndex, 2, i, o, Look(id: l.id, kind: .line, fill: nil, stroke: AsukaColors.color(l.color), width: l.width, dashed: l.dashed)))
        }
        all.sort { a, b in a.z != b.z ? a.z < b.z : a.kind != b.kind ? a.kind < b.kind : a.i < b.i }
        map.removeOverlays(drawn.map(\.overlay))
        drawn = all.map { ($0.overlay, $0.look) }
        looks = Dictionary(uniqueKeysWithValues: drawn.map { (ObjectIdentifier($0.overlay), $0.look) })
        // Each above the one before (the order added), all above the roads.
        map.addOverlays(drawn.map(\.overlay), level: .aboveRoads)
    }

    private func coords(_ cs: [Coordinate]) -> [CLLocationCoordinate2D] {
        cs.map { CLLocationCoordinate2D(latitude: $0.lat, longitude: $0.lng) }
    }

    /// The topmost overlay with an id at a point of the map, if any.
    private func overlay(at p: CGPoint) -> String? {
        for (o, look) in drawn.reversed() where !look.id.isEmpty {
            switch look.kind {
            case .circle:
                guard let c = o as? MKCircle else { continue }
                let at = MKMapPoint(map.convert(p, toCoordinateFrom: map))
                if at.distance(to: MKMapPoint(c.coordinate)) <= c.radius { return look.id }
            case .polygon:
                guard let g = o as? MKPolygon else { continue }
                if inside(p, g) && !(g.interiorPolygons ?? []).contains(where: { inside(p, $0) }) { return look.id }
            case .line:
                guard let l = o as? MKPolyline else { continue }
                let pts = points(l)
                let reach = look.width / 2 + 6
                for i in 1..<max(pts.count, 1) where MapMath.segmentDistance(p, pts[i - 1], pts[i]) <= reach { return look.id }
            }
        }
        return nil
    }

    private func points(_ s: MKMultiPoint) -> [CGPoint] {
        let mp = s.points()
        return (0..<s.pointCount).map { map.convert(mp[$0].coordinate, toPointTo: map) }
    }

    private func inside(_ p: CGPoint, _ g: MKPolygon) -> Bool {
        let pts = points(g)
        var inside = false
        var j = pts.count - 1
        for i in 0..<pts.count {
            let a = pts[i], b = pts[j]
            if (a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x { inside.toggle() }
            j = i
        }
        return inside
    }

    // MARK: - The rest

    func setMapType(_ t: MapType) {
        if #available(iOS 16, macOS 13, tvOS 16, *) {
            switch t {
            case .satellite: map.preferredConfiguration = MKImageryMapConfiguration()
            case .hybrid: map.preferredConfiguration = MKHybridMapConfiguration()
            case .muted: map.preferredConfiguration = MKStandardMapConfiguration(emphasisStyle: .muted)
            default: map.preferredConfiguration = MKStandardMapConfiguration()
            }
        } else {
            map.mapType = t == .satellite ? .satellite : t == .hybrid ? .hybrid : .standard
        }
    }

    func setShowsUser(_ on: Bool) {
        if on && CLLocationManager().authorizationStatus == .notDetermined {
            // Asks for it, if the app has the purpose string (platform/location's).
            if Bundle.main.object(forInfoDictionaryKey: "NSLocationWhenInUseUsageDescription") != nil {
                location = CLLocationManager()
                location?.requestWhenInUseAuthorization()
            }
        }
        map.showsUserLocation = on
    }

    func setInteractive(_ on: Bool) {
        map.isScrollEnabled = on
        map.isZoomEnabled = on
        #if !os(tvOS) // tvOS's map neither rotates nor tilts
        map.isRotateEnabled = on
        map.isPitchEnabled = on
        #endif
    }

    // MARK: - Taps

    /// Whether a point of the map is on a marker's (or a cluster's) view,
    /// or on the callout of one.
    private func onMarker(_ p: CGPoint) -> Bool {
        #if canImport(UIKit)
        var v: AsukaPlatformView? = map.hitTest(p, with: nil)
        #else
        var v: AsukaPlatformView? = nil
        if let s = map.superview { v = map.hitTest(map.convert(p, to: s)) }
        #endif
        while let x = v, x !== map {
            if x is MKAnnotationView || NSStringFromClass(type(of: x)).contains("Callout") { return true }
            v = x.superview
        }
        return false
    }

    /// A tap on the map, not on a marker or a callout: the selection goes,
    /// then an overlay's press, else press.
    private func pressed(_ p: CGPoint) {
        if onMarker(p) { return }
        let at = map.convert(p, toCoordinateFrom: map)
        // MapKit deselects the marker: it's said first.
        if !selected.isEmpty {
            selected = ""
            events.selectedChange(selected: "")
        }
        if let hit = overlay(at: p) { events.overlayPress(id: hit) } else { events.press(at: Coordinate(lat: at.latitude, lng: at.longitude)) }
    }

    /// No marker's selected now: selectedChange, if one was.
    private func deselected() {
        guard !selected.isEmpty, !map.selectedAnnotations.contains(where: { $0 is Pin }) else { return }
        selected = ""
        events.selectedChange(selected: "")
    }

    #if canImport(UIKit)
    @objc private func tapped(_ g: UITapGestureRecognizer) { pressed(g.location(in: map)) }
    #if !os(tvOS)
    @objc private func longPressed(_ g: UILongPressGestureRecognizer) {
        guard g.state == .began else { return }
        let p = g.location(in: map)
        if onMarker(p) { return } // a draggable marker's
        let c = map.convert(p, toCoordinateFrom: map)
        events.longPress(at: Coordinate(lat: c.latitude, lng: c.longitude))
    }
    #endif
    #else
    @objc private func clicked(_ g: NSClickGestureRecognizer) { pressed(g.location(in: map)) }
    @objc private func rightClicked(_ g: NSClickGestureRecognizer) {
        let p = g.location(in: map)
        if onMarker(p) { return }
        let c = map.convert(p, toCoordinateFrom: map)
        events.longPress(at: Coordinate(lat: c.latitude, lng: c.longitude))
    }
    #endif

    // MARK: - MKMapViewDelegate

    nonisolated func mapView(_ m: MKMapView, regionWillChangeAnimated animated: Bool) {
        MainActor.assumeIsolated {
            #if canImport(UIKit)
            // A gesture moves it: the user's.
            userMoving = map.subviews.first?.gestureRecognizers?.contains { $0.state == .began || $0.state == .changed } ?? false
            if userMoving { setting = false }
            #else
            userMoving = !setting
            #endif
        }
    }

    nonisolated func mapViewDidChangeVisibleRegion(_ m: MKMapView) {
        MainActor.assumeIsolated {
            guard map.bounds.height > 0 else { return }
            events.cameraMove(camera: camera(), region: regionNow())
        }
    }

    nonisolated func mapView(_ m: MKMapView, regionDidChangeAnimated animated: Bool) {
        MainActor.assumeIsolated {
            defer { setting = false; userMoving = false }
            guard map.bounds.height > 0 else { return }
            let c = camera(), r = regionNow()
            if userMoving && !setting {
                events.regionChange(region: r)
                events.cameraChange(camera: c)
            }
            events.cameraIdle(camera: c, region: r)
        }
    }

    nonisolated func mapView(_ m: MKMapView, viewFor annotation: MKAnnotation) -> MKAnnotationView? {
        MainActor.assumeIsolated {
            if let c = annotation as? MKClusterAnnotation {
                let v = map.dequeueReusableAnnotationView(withIdentifier: "asuka.cluster") as? MKMarkerAnnotationView
                    ?? MKMarkerAnnotationView(annotation: c, reuseIdentifier: "asuka.cluster")
                v.annotation = c
                v.markerTintColor = clusterColor
                v.canShowCallout = false
                v.displayPriority = .required
                return v
            }
            guard let p = annotation as? Pin else { return nil }
            let id = p.marker.image.isEmpty ? "asuka.pin" : "asuka.image"
            let v = map.dequeueReusableAnnotationView(withIdentifier: id)
                ?? (p.marker.image.isEmpty ? MKMarkerAnnotationView(annotation: p, reuseIdentifier: id) : MKAnnotationView(annotation: p, reuseIdentifier: id))
            v.annotation = p
            configure(v, p)
            return v
        }
    }

    /// Sets a marker's view up as its marker says.
    private func configure(_ v: MKAnnotationView, _ p: Pin) {
        let m = p.marker
        v.clusteringIdentifier = clustering && m.clusters ? "asuka" : nil
        v.displayPriority = .required
        v.zPriority = MKAnnotationViewZPriority(rawValue: Float(500 + max(min(m.zIndex, 400), -400)))
        v.canShowCallout = !m.title.isEmpty || !m.subtitle.isEmpty
        #if !os(tvOS)
        v.isDraggable = m.draggable
        #endif
        if v.canShowCallout {
            #if canImport(UIKit)
            if v.rightCalloutAccessoryView == nil { v.rightCalloutAccessoryView = UIButton(type: .detailDisclosure) }
            #else
            if v.rightCalloutAccessoryView == nil {
                let b = NSButton(image: NSImage(systemSymbolName: "info.circle", accessibilityDescription: "Details") ?? NSImage(), target: nil, action: nil)
                b.isBordered = false
                v.rightCalloutAccessoryView = b
            }
            #endif
        }
        if let mv = v as? MKMarkerAnnotationView {
            mv.markerTintColor = m.color.flatMap(AsukaColors.color)
            return
        }
        guard let img = p.image else {
            v.image = nil
            return
        }
        let size = MapImages.size(img, m.width, m.height)
        v.image = MapImages.resized(img, size)
        v.centerOffset = CGPoint(x: (0.5 - m.anchor.x) * size.width, y: (0.5 - m.anchor.y) * size.height)
    }

    nonisolated func mapView(_ m: MKMapView, didSelect v: MKAnnotationView) {
        MainActor.assumeIsolated {
            if let c = v.annotation as? MKClusterAnnotation {
                map.deselectAnnotation(c, animated: false)
                let members = c.memberAnnotations.compactMap { $0 as? Pin }.sorted { (order[$0.marker.id] ?? 0) < (order[$1.marker.id] ?? 0) }
                events.clusterPress(ids: members.map(\.marker.id))
                // Shows its markers, if they can split.
                let rect = members.reduce(MKMapRect.null) { $0.union(MKMapRect(origin: MKMapPoint($1.coordinate), size: MKMapSize(width: 0, height: 0))) }
                if rect.size.width > 0 || rect.size.height > 0 {
                    #if canImport(UIKit)
                    let pad = UIEdgeInsets(top: 48, left: 48, bottom: 48, right: 48)
                    #else
                    let pad = NSEdgeInsets(top: 48, left: 48, bottom: 48, right: 48)
                    #endif
                    setting = true
                    map.setVisibleMapRect(rect, edgePadding: pad, animated: true)
                }
                return
            }
            guard let p = v.annotation as? Pin else { return }
            let id = p.marker.id
            if let s = selecting, s == id {
                selecting = nil
                return
            }
            selecting = nil
            events.markerPress(id: id)
            if selected != id {
                selected = id
                events.selectedChange(selected: id)
            }
        }
    }

    nonisolated func mapView(_ m: MKMapView, didDeselect v: MKAnnotationView) {
        MainActor.assumeIsolated {
            guard v.annotation is Pin else { return }
            if selecting == "" {
                selecting = nil
                return
            }
            // Another marker's selected next, or none.
            DispatchQueue.main.async { [weak self] in self?.deselected() }
        }
    }

    #if !os(tvOS)
    nonisolated func mapView(_ m: MKMapView, annotationView v: MKAnnotationView, calloutAccessoryControlTapped control: AsukaControl) {
        MainActor.assumeIsolated {
            if let p = v.annotation as? Pin { events.calloutPress(id: p.marker.id) }
        }
    }

    nonisolated func mapView(_ m: MKMapView, annotationView v: MKAnnotationView, didChange new: MKAnnotationView.DragState, fromOldState old: MKAnnotationView.DragState) {
        MainActor.assumeIsolated {
            guard let p = v.annotation as? Pin else { return }
            switch new {
            case .starting:
                events.markerDragStart(id: p.marker.id, at: Coordinate(lat: p.coordinate.latitude, lng: p.coordinate.longitude))
                var last = p.coordinate
                dragTimer?.invalidate()
                weak var dragged: MKAnnotationView? = v
                dragTimer = Timer.scheduledTimer(withTimeInterval: 1.0 / 60, repeats: true) { [weak self] _ in
                    MainActor.assumeIsolated {
                        guard let self, let dv = dragged, let sup: AsukaPlatformView = dv.superview else { return }
                        // Where its view is now: the point its offset is from.
                        let f = dv.frame
                        let pt = CGPoint(x: f.midX - dv.centerOffset.x, y: f.midY - dv.centerOffset.y)
                        let c = self.map.convert(pt, toCoordinateFrom: sup)
                        if abs(c.latitude - last.latitude) > 1e-9 || abs(c.longitude - last.longitude) > 1e-9 {
                            last = c
                            self.events.markerDrag(id: p.marker.id, at: Coordinate(lat: c.latitude, lng: c.longitude))
                        }
                    }
                }
            case .ending, .canceling:
                dragTimer?.invalidate()
                dragTimer = nil
                v.setDragState(.none, animated: true)
                events.markerDragEnd(id: p.marker.id, at: Coordinate(lat: p.coordinate.latitude, lng: p.coordinate.longitude))
            default: break
            }
        }
    }
    #endif

    nonisolated func mapView(_ m: MKMapView, rendererFor overlay: MKOverlay) -> MKOverlayRenderer {
        MainActor.assumeIsolated {
            guard let look = looks[ObjectIdentifier(overlay)] else { return MKOverlayRenderer(overlay: overlay) }
            let r: MKOverlayPathRenderer
            switch overlay {
            case let c as MKCircle: r = MKCircleRenderer(circle: c)
            case let g as MKPolygon: r = MKPolygonRenderer(polygon: g)
            case let l as MKPolyline: r = MKPolylineRenderer(polyline: l)
            default: return MKOverlayRenderer(overlay: overlay)
            }
            r.fillColor = look.fill
            r.strokeColor = look.width > 0 ? look.stroke : nil
            r.lineWidth = CGFloat(look.width)
            r.lineJoin = .round
            if look.dashed {
                r.lineCap = .butt
                r.lineDashPattern = [NSNumber(value: 4 * look.width), NSNumber(value: 2 * look.width)]
            } else {
                r.lineCap = .round
            }
            return r
        }
    }
}

#if canImport(UIKit)
typealias AsukaControl = UIControl
#else
typealias AsukaControl = NSControl
#endif

/// How an overlay's drawn, and its id for overlayPress.
struct Look {
    enum Kind { case polygon, circle, line }
    let id: String
    let kind: Kind
    let fill: AsukaPlatformColor?
    let stroke: AsukaPlatformColor?
    let width: Double
    let dashed: Bool
}

@MainActor func makeView(_ events: ViewEvents) -> any View { MapView(events) }
