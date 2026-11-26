// platform/maps on iOS and macOS: an MKMapView. The region set moves it
// (animated once it's on screen), unless it's already there (the user's
// move, bound and given back); regionChange is the user's moves only.
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
    private var setting = false // moving the map for the region prop: not the user's move
    private var userMoving = false
    private var location: CLLocationManager?

    init(_ events: ViewEvents) {
        self.events = events
        super.init()
        map.delegate = self
        #if canImport(UIKit)
        let tap = UITapGestureRecognizer(target: self, action: #selector(tapped(_:)))
        tap.delegate = map
        map.addGestureRecognizer(tap)
        #else
        let click = NSClickGestureRecognizer(target: self, action: #selector(clicked(_:)))
        click.delaysPrimaryMouseButtonEvents = false
        map.addGestureRecognizer(click)
        #endif
    }

    func setRegion(_ r: Region) {
        guard r.latDelta > 0 || r.lngDelta > 0 || r.center.lat != 0 || r.center.lng != 0 else { return }
        let center = CLLocationCoordinate2D(latitude: r.center.lat, longitude: r.center.lng)
        let span = MKCoordinateSpan(latitudeDelta: max(r.latDelta > 0 ? r.latDelta : 0.05, 0.0005), longitudeDelta: max(r.lngDelta > 0 ? r.lngDelta : 0.05, 0.0005))
        // The region the user moved to, given back: already there.
        let cur = map.region
        if abs(cur.center.latitude - center.latitude) < 1e-6 && abs(cur.center.longitude - center.longitude) < 1e-6
            && abs(cur.span.latitudeDelta - span.latitudeDelta) < 1e-6 { return }
        setting = true
        map.setRegion(MKCoordinateRegion(center: center, span: span), animated: map.window != nil)
    }

    func setMarkers(_ list: [Marker]) {
        map.removeAnnotations(map.annotations.filter { $0 is Pin })
        map.addAnnotations(list.map(Pin.init))
    }

    func setMapType(_ t: MapType) {
        if #available(iOS 16, macOS 13, *) {
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

    /// A tap on the map, not on a marker: press.
    private func pressed(_ p: CGPoint) {
        if map.annotations.contains(where: { a in map.view(for: a)?.frame.contains(p) ?? false }) { return }
        let c = map.convert(p, toCoordinateFrom: map)
        events.press(at: Coordinate(lat: c.latitude, lng: c.longitude))
    }

    #if canImport(UIKit)
    @objc private func tapped(_ g: UITapGestureRecognizer) { pressed(g.location(in: map)) }
    #else
    @objc private func clicked(_ g: NSClickGestureRecognizer) { pressed(g.location(in: map)) }
    #endif

    nonisolated func mapView(_ m: MKMapView, regionWillChangeAnimated animated: Bool) {
        MainActor.assumeIsolated {
            #if canImport(UIKit)
            // A gesture moves it: the user's.
            userMoving = map.subviews.first?.gestureRecognizers?.contains { $0.state == .began || $0.state == .changed } ?? false
            #else
            userMoving = !setting
            #endif
        }
    }

    nonisolated func mapView(_ m: MKMapView, regionDidChangeAnimated animated: Bool) {
        MainActor.assumeIsolated {
            defer { setting = false; userMoving = false }
            guard userMoving && !setting else { return }
            let r = map.region
            events.regionChange(region: Region(center: Coordinate(lat: r.center.latitude, lng: r.center.longitude),
                                               latDelta: r.span.latitudeDelta, lngDelta: r.span.longitudeDelta))
        }
    }

    nonisolated func mapView(_ m: MKMapView, viewFor annotation: MKAnnotation) -> MKAnnotationView? {
        MainActor.assumeIsolated {
            guard let a = annotation as? Pin else { return nil }
            let v = map.dequeueReusableAnnotationView(withIdentifier: "asuka") as? MKMarkerAnnotationView
                ?? MKMarkerAnnotationView(annotation: a, reuseIdentifier: "asuka")
            v.annotation = a
            v.markerTintColor = a.color
            v.canShowCallout = a.title != nil
            return v
        }
    }

    nonisolated func mapView(_ m: MKMapView, didSelect v: MKAnnotationView) {
        MainActor.assumeIsolated {
            if let a = v.annotation as? Pin { events.markerPress(id: a.id) }
        }
    }
}

/// The map view: it has no size of its own (its style gives it one, or
/// grow), and lets its own gestures and the tap recognize together.
#if canImport(UIKit)
final class PlatformMap: MKMapView, UIGestureRecognizerDelegate {
    override func sizeThatFits(_ size: CGSize) -> CGSize { .zero }
    func gestureRecognizer(_ g: UIGestureRecognizer, shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool { true }
}
#else
final class PlatformMap: MKMapView {
    override var fittingSize: NSSize { .zero }
}
#endif

final class Pin: NSObject, MKAnnotation {
    let id: String
    let coordinate: CLLocationCoordinate2D
    let title: String?
    let subtitle: String?
    let color: AsukaPlatformColor?

    init(_ m: Marker) {
        id = m.id
        coordinate = CLLocationCoordinate2D(latitude: m.at.lat, longitude: m.at.lng)
        title = m.title.isEmpty ? nil : m.title
        subtitle = m.subtitle.isEmpty ? nil : m.subtitle
        color = m.color.flatMap(AsukaColors.color)
    }
}

@MainActor func makeView(_ events: ViewEvents) -> any View { MapView(events) }
