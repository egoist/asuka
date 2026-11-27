// platform/car's navigation map on the iOS simulator (CarPlay is iOS's
// alone): the map controller CarPlay's window shows, given the app's
// car.Map, and the CPMapTemplate over it, given a .navigation's Shown —
// built and driven here without a car (the simulator can't show CarPlay's
// screen), their state read back: where the map looks, its lines, pins
// and puck, the driver's zoom and pan and recenter, day and night; the
// map's buttons, the bar, a preview's routes and the one started, a trip's
// maneuvers and estimates, an alert and its answer.
import CarPlay
import Foundation
import MapKit
import UIKit

@main struct NavigationViewTest {
    @MainActor static func main() {
        let home = Coordinate(lat: 35.6812, lng: 139.7671)
        let route = [home, Coordinate(lat: 35.6830, lng: 139.7700), Coordinate(lat: 35.6858, lng: 139.7745)]
        let m = CarMapController()
        m.size = CGSize(width: 800, height: 480)
        m.loadViewIfNeeded()
        m.map.frame = CGRect(x: 0, y: 0, width: 800, height: 480)
        var moved: [Bool] = []
        m.moved = { moved.append($0) }

        // Following the car: it's in the middle, at the zoom, with the
        // route, the pin and the puck.
        let given = Map(center: Coordinate(lat: 1, lng: 1), zoom: 15, position: home, follow: true, route: route,
                        markers: [Marker(at: route[2], title: "Office")])
        m.show(given)
        settle()
        check(near(m.map.centerCoordinate, home), "follows the car: \(m.map.centerCoordinate)")
        check(abs(m.zoom - 15) < 0.05, "zoom 15, got \(m.zoom)")
        check(m.drawn.count == 1 && m.drawn[0].points == 3 && m.drawn[0].route, "the route drawn: \(m.drawn)")
        check(m.markerTitles == ["Office"], "the markers: \(m.markerTitles)")
        check(m.puckShown, "the puck")
        check(m.map.overrideUserInterfaceStyle == .unspecified, "the car's day and night by default")

        // The driver zooms in: shown, and the recenter button with it; the
        // app's next position keeps the driver's zoom.
        m.zoom(by: 1)
        settle()
        check(abs(m.zoom - 16) < 0.05, "zoomed in to 16, got \(m.zoom)")
        check(moved.last == true, "moved said: \(moved)")
        var g2 = given
        g2.position = route[1]
        m.show(g2)
        settle()
        check(abs(m.zoom - 16) < 0.05 && near(m.map.centerCoordinate, route[1]), "still the driver's zoom, following: \(m.zoom) \(m.map.centerCoordinate)")
        // The driver pans east: the map stops following.
        m.pan(translation: CGPoint(x: -200, y: 0))
        settle()
        let panned = m.map.centerCoordinate
        check(panned.longitude > route[1].lng + 0.001, "panned east: \(panned)")
        var g3 = g2
        g3.position = route[2]
        m.show(g3)
        settle()
        check(near(m.map.centerCoordinate, CLLocationCoordinate2D(latitude: panned.latitude, longitude: panned.longitude)), "the driver's pan stays as the car moves: \(m.map.centerCoordinate)")
        // Recentered: the car again, at the app's zoom.
        m.recenter()
        settle()
        check(near(m.map.centerCoordinate, route[2]) && abs(m.zoom - 15) < 0.05, "recentered: \(m.map.centerCoordinate) \(m.zoom)")
        check(moved.last == false, "recentered said: \(moved)")

        // The app's own zoom takes the driver's away.
        m.zoom(by: -2)
        var g4 = g3
        g4.zoom = 14
        m.show(g4)
        settle()
        check(abs(m.zoom - 14) < 0.05, "the app's zoom 14, got \(m.zoom)")

        // Heading up while following it.
        var g5 = g4
        g5.heading = 90
        m.show(g5)
        settle()
        check(abs(m.map.camera.heading - 90) < 1, "heading up: \(m.map.camera.heading)")
        check(near(m.map.centerCoordinate, route[2]), "still on the car turned: \(m.map.centerCoordinate)")

        // Not following: the app's center; night.
        var g6 = g5
        g6.follow = false
        g6.center = route[0]
        g6.night = true
        m.show(g6)
        settle()
        check(near(m.map.centerCoordinate, route[0]) && abs(m.map.camera.heading) < 1, "the app's center, north up: \(m.map.centerCoordinate) \(m.map.camera.heading)")
        check(m.map.overrideUserInterfaceStyle == .dark, "night")

        // The map template: its buttons (recenter hidden), its bar.
        var chosen: [Int] = []
        var answers: [[Int]] = []
        let shown = Shown(id: 1, kind: .navigation, actions: [ShownAction(run: 3, title: "End", enabled: true)], map: given)
        let nav = CarNavigation(shown, map: CarMapController(), chose: { chosen.append($0) }, answered: { answers.append([$0, $1]) })
        nav.map.size = CGSize(width: 800, height: 480)
        nav.map.map.frame = CGRect(x: 0, y: 0, width: 800, height: 480)
        check(nav.template.mapButtons.count == 4 && nav.mapButtonsShown == 3, "the map's buttons: \(nav.template.mapButtons.count), \(nav.mapButtonsShown) shown")
        check(nav.template.trailingNavigationBarButtons.count == 1, "the bar: \(nav.template.trailingNavigationBarButtons.count)")
        nav.map.zoom(by: 1)
        check(nav.mapButtonsShown == 4, "recenter shows once the driver zoomed")
        nav.map.recenter()
        check(nav.mapButtonsShown == 3, "and goes again")

        // A preview: its routes as route choices, the first drawn
        // highlighted over the other; the second highlighted; started.
        let fast = ShownRoute(run: 7, title: "Fastest", distance: 1200, time: 300, path: route)
        let quiet = ShownRoute(run: 8, title: "Quiet", detail: "No tolls", distance: 1500, time: 420, path: [route[0], Coordinate(lat: 35.684, lng: 139.768), route[2]])
        var p = shown
        p.preview = ShownPreview(destination: "Office", at: route[2], routes: [fast, quiet])
        nav.update(p)
        guard let trip = nav.previewTrip else { fail("no preview shown") }
        check(trip.routeChoices.count == 2 && trip.routeChoices[1].summaryVariants == ["Quiet"], "the route choices: \(trip.routeChoices.map { $0.summaryVariants })")
        check(nav.map.drawn.map { $0.points } == [3, 3] && nav.map.drawn.last?.route == true, "the preview's lines: \(nav.map.drawn)")
        nav.mapTemplate(nav.template, selectedPreviewFor: trip, using: trip.routeChoices[1])
        check(nav.map.drawn.count == 2 && nav.map.drawn[0].route == false && nav.map.drawn[1].route, "the second highlighted: \(nav.map.drawn)")
        nav.mapTemplate(nav.template, startedTrip: trip, using: trip.routeChoices[1])
        check(chosen == [8], "starting it runs its run: \(chosen)")

        // The trip: a session, its maneuvers, the preview gone.
        var t = shown
        t.trip = ShownTrip(destination: "Office", maneuvers: [
            Maneuver(instruction: "Turn right onto Main St", turn: .right, road: "Main St", distance: 120, time: 30),
            Maneuver(instruction: "Take the 2nd exit", turn: .roundabout, distance: 800, time: 90, exit: 2),
        ], distance: 1500, time: 420, stop: 9)
        nav.update(t)
        check(nav.previewTrip == nil, "the preview goes as the trip starts")
        check(nav.session != nil && nav.trip === trip, "a session for the trip chosen")
        check(nav.maneuvers.count == 2 && nav.maneuvers[0].instructionVariants == ["Turn right onto Main St"], "the maneuvers: \(nav.maneuvers.map { $0.instructionVariants })")
        check(nav.maneuvers.allSatisfy { $0.symbolImage != nil }, "each maneuver's arrow")
        if #available(iOS 17.4, *) {
            check(nav.maneuvers[0].maneuverType == .rightTurn && nav.maneuvers[1].maneuverType == .roundaboutExit2, "the maneuvers' types: \(nav.maneuvers.map { $0.maneuverType.rawValue })")
        }
        check(nav.maneuvers[0].initialTravelEstimates?.distanceRemaining.value == 120, "the distance to it")
        check(nav.map.drawn.count == 1 && nav.map.drawn[0].points == 3, "the route drawn again: \(nav.map.drawn)")
        // The same maneuvers nearer: the same CPManeuvers, estimated again.
        let first = nav.maneuvers[0]
        var t2 = t
        t2.trip?.maneuvers[0].distance = 60
        nav.update(t2)
        check(nav.maneuvers[0] === first, "the maneuvers kept as they come nearer")
        // CarPlay ends it: the trip's stop.
        nav.mapTemplateDidCancelNavigation(nav.template)
        check(chosen == [8, 9] && nav.session == nil, "CarPlay's end runs stop: \(chosen)")
        // The app's trip goes: nothing more to end.
        nav.update(shown)
        check(nav.session == nil, "no session")

        // Every turn's arrow is a symbol iOS has.
        for i in 0...18 {
            check(Turns.image(Turn(rawValue: i)) != nil, "no symbol for turn \(i): \(Turns.symbol(Turn(rawValue: i)))")
        }

        // An alert: its title and buttons; a button answers it; the next
        // one dismissed answers -1.
        nav.present(ShownAlert(id: 4, title: "Traffic ahead", detail: "10 min slower", actions: [ShownAction(run: 2, title: "Reroute", enabled: true), ShownAction(run: 0, title: "Ignore", style: 1, enabled: true)], duration: 8))
        guard let a = nav.alert else { fail("no alert") }
        check(a.titleVariants == ["Traffic ahead"] && a.subtitleVariants == ["10 min slower"], "the alert's text: \(a.titleVariants) \(a.subtitleVariants)")
        check(a.primaryAction.title == "Reroute" && a.secondaryAction?.title == "Ignore" && a.duration == 8, "its buttons")
        nav.present(ShownAlert(id: 5, title: "Speed camera"))
        check(answers == [[4, -1]], "replaced: the first went without a button: \(answers)")
        check(nav.alert?.primaryAction.title.isEmpty == false, "a lone alert has an OK")
        nav.mapTemplate(nav.template, didDismiss: nav.alert!, dismissalContext: .timeout)
        check(answers == [[4, -1], [5, -1]], "timed out: \(answers)")

        print("ok")
        exit(0)
    }

    @MainActor static func settle() {
        for _ in 0..<5 { RunLoop.main.run(until: Date()) }
    }

    static func near(_ c: CLLocationCoordinate2D, _ want: Coordinate) -> Bool {
        abs(c.latitude - want.lat) < 2e-4 && abs(c.longitude - want.lng) < 2e-4
    }

    static func near(_ c: CLLocationCoordinate2D, _ want: CLLocationCoordinate2D) -> Bool {
        abs(c.latitude - want.latitude) < 2e-4 && abs(c.longitude - want.longitude) < 2e-4
    }

    static func check(_ ok: Bool, _ what: String) {
        if !ok { fail(what) }
    }

    static func fail(_ what: String) -> Never {
        print("FAIL: " + what)
        exit(1)
    }
}
