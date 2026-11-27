// platform/car's .navigation on CarPlay: a CPMapTemplate over the map
// (CarMapController, the CarPlay window's root view controller). Its map
// buttons zoom, pan and recenter the map; its bar has the app's actions.
// A preview shows its routes as CPTrip's route choices (the highlighted
// one drawn on the map), and the one started runs its route's run. A
// trip is a CPNavigationSession: its maneuvers (CPManeuver: the
// instruction, an arrow, the distance and time to it), its estimates
// (CPTravelEstimates, to the destination), paused while it reroutes, and
// finished when the trip goes; CarPlay's own end of it (another app
// navigating) runs the trip's stop. Alerts are CPNavigationAlerts.
import CarPlay
import Foundation
import MapKit
import UIKit

@MainActor final class CarNavigation: NSObject, CPMapTemplateDelegate {
    let template = CPMapTemplate()
    let map: CarMapController
    private(set) var shown: Shown
    private let chose: (Int) -> Void
    private let answered: (Int, Int) -> Void

    /// The map's buttons: recenter (while the driver has moved it), pan,
    /// zoom in and out.
    private(set) var recenterButton: CPMapButton!

    /// The preview shown: its trip, and what it was made from.
    private(set) var previewTrip: CPTrip?
    private var previewMade = ""
    /// The trip guided: its session, its CarPlay trip, its maneuvers and
    /// what they were made from.
    private(set) var session: CPNavigationSession?
    private(set) var trip: CPTrip?
    private(set) var maneuvers: [CPManeuver] = []
    private var maneuversMade = ""
    private var paused = false
    /// The alert showing, and its id; ids answered already.
    private(set) var alert: CPNavigationAlert?
    private var alertID = 0
    private var answeredIDs: Set<Int> = []

    init(_ s: Shown, map: CarMapController, chose: @escaping (Int) -> Void, answered: @escaping (Int, Int) -> Void) {
        shown = s
        self.map = map
        self.chose = chose
        self.answered = answered
        super.init()
        template.mapDelegate = self
        func button(_ symbol: String, _ run: @escaping @MainActor (CarNavigation) -> Void) -> CPMapButton {
            let b = CPMapButton { [weak self] _ in
                MainActor.assumeIsolated {
                    if let self { run(self) }
                }
            }
            b.image = UIImage(systemName: symbol)
            return b
        }
        recenterButton = button("location.fill") { $0.map.recenter() }
        recenterButton.isHidden = true
        let pan = button("arrow.up.and.down.and.arrow.left.and.right") { n in
            if n.template.isPanningInterfaceVisible { n.template.dismissPanningInterface(animated: true) } else { n.template.showPanningInterface(animated: true) }
        }
        template.mapButtons = [recenterButton, pan, button("plus.magnifyingglass") { $0.map.zoom(by: 1) }, button("minus.magnifyingglass") { $0.map.zoom(by: -1) }]
        map.moved = { [weak self] moved in self?.recenterButton.isHidden = !moved }
        apply(s, first: true)
    }

    /// Shows s: its map, its bar, its preview, its trip.
    func update(_ s: Shown) {
        let was = shown
        shown = s
        if String(describing: was) == String(describing: s) { return }
        apply(s, first: false)
    }

    private func apply(_ s: Shown, first: Bool) {
        let m = s.map ?? Map(zoom: 15, follow: true)
        map.show(m)
        bar(s.actions)
        // The trip first: one started from the preview takes its CPTrip.
        guide(s.trip)
        preview(s.preview)
    }

    // MARK: The bar

    var barActions: [ShownAction] = []

    private func bar(_ actions: [ShownAction]) {
        barActions = actions
        let buttons = actions.prefix(4).map { a -> CPBarButton in
            let run = a.run
            let handler: (CPBarButton) -> Void = { [weak self] _ in MainActor.assumeIsolated { self?.chose(run) } }
            let b: CPBarButton
            if !a.symbol.isEmpty, let img = UIImage(systemName: a.symbol) {
                b = CPBarButton(image: img, handler: handler)
            } else {
                b = CPBarButton(title: a.title, handler: handler)
            }
            b.isEnabled = a.enabled && run > 0
            return b
        }
        template.trailingNavigationBarButtons = Array(buttons.prefix(2))
        template.leadingNavigationBarButtons = Array(buttons.dropFirst(2))
    }

    // MARK: Previews

    private func preview(_ p: ShownPreview?) {
        guard let p, !p.routes.isEmpty, session == nil else {
            if previewTrip != nil {
                template.hideTripPreviews()
                previewTrip = nil
                previewMade = ""
                map.preview([], highlighted: 0)
            }
            return
        }
        let made = String(describing: p)
        if made == previewMade { return }
        previewMade = made
        let choices = p.routes.map { r -> CPRouteChoice in
            let summary = [r.title, estimate(r.distance, r.time)].filter { !$0.isEmpty }.joined(separator: " · ")
            let c = CPRouteChoice(summaryVariants: [r.title], additionalInformationVariants: r.detail.isEmpty ? [] : [r.detail], selectionSummaryVariants: [summary])
            c.userInfo = r.run
            return c
        }
        let t = CPTrip(origin: origin(), destination: place(p.destination, p.at), routeChoices: choices)
        previewTrip = t
        let text = CPTripPreviewTextConfiguration(startButtonTitle: p.start.isEmpty ? nil : p.start, additionalRoutesButtonTitle: nil, overviewButtonTitle: nil)
        template.showTripPreviews([t], textConfiguration: text)
        highlight(0)
    }

    /// Route i of the preview is the one highlighted: drawn over the
    /// others, its estimates shown.
    private func highlight(_ i: Int) {
        guard let p = shown.preview, let t = previewTrip, i < p.routes.count else { return }
        map.preview(p.routes.map { $0.path }, highlighted: i)
        let r = p.routes[i]
        template.updateEstimates(estimates(r.distance, r.time), for: t)
    }

    // MARK: Guidance

    private func guide(_ g: ShownTrip?) {
        guard let g else {
            if let s = session {
                s.finishTrip()
                session = nil
                trip = nil
                maneuvers = []
                maneuversMade = ""
            }
            return
        }
        if session == nil {
            let t = previewTrip ?? CPTrip(origin: origin(), destination: place(g.destination, shown.map?.route.last ?? Coordinate()),
                                          routeChoices: [CPRouteChoice(summaryVariants: [g.destination], additionalInformationVariants: [], selectionSummaryVariants: [])])
            if previewTrip != nil {
                template.hideTripPreviews()
                previewTrip = nil
                previewMade = ""
                map.preview([], highlighted: 0)
            }
            trip = t
            session = template.startNavigationSession(for: t)
            paused = false
        }
        guard let s = session, let t = trip else { return }
        if g.rerouting {
            if !paused {
                s.pauseTrip(for: .rerouting, description: nil)
                paused = true
                maneuversMade = ""
            }
        } else {
            let made = String(describing: g.maneuvers.map { ($0.instruction, $0.turn.rawValue, $0.road, $0.exit) })
            if made != maneuversMade || paused {
                maneuversMade = made
                paused = false
                maneuvers = g.maneuvers.prefix(2).map { maneuver($0) }
                s.upcomingManeuvers = maneuvers
            }
            for (m, cp) in zip(g.maneuvers, maneuvers) {
                s.updateEstimates(estimates(m.distance, m.time), for: cp)
            }
        }
        template.updateEstimates(estimates(g.distance, g.time), for: t)
    }

    func maneuver(_ m: Maneuver) -> CPManeuver {
        let c = CPManeuver()
        c.instructionVariants = [m.instruction]
        c.symbolImage = Turns.image(m.turn)
        c.initialTravelEstimates = estimates(m.distance, m.time)
        if #available(iOS 17.4, *) {
            c.maneuverType = Turns.type(m.turn, exit: m.exit)
            if !m.road.isEmpty { c.roadFollowingManeuverVariants = [m.road] }
            if m.turn == .roundabout { c.junctionType = .roundabout }
        }
        return c
    }

    private func estimates(_ meters: Double, _ time: TimeInterval) -> CPTravelEstimates {
        CPTravelEstimates(distanceRemaining: Measurement(value: max(meters, 0), unit: UnitLength.meters), timeRemaining: max(time, 0))
    }

    private func estimate(_ meters: Double, _ time: TimeInterval) -> String {
        var parts: [String] = []
        if time > 0 {
            let f = DateComponentsFormatter()
            f.allowedUnits = time >= 3600 ? [.hour, .minute] : [.minute]
            f.unitsStyle = .abbreviated
            if let s = f.string(from: time) { parts.append(s) }
        }
        if meters > 0 { parts.append(MKDistanceFormatter().string(fromDistance: meters)) }
        return parts.joined(separator: " · ")
    }

    /// Where the trip starts: the car's position, else here.
    private func origin() -> MKMapItem {
        if let p = shown.map?.position {
            return MKMapItem(placemark: MKPlacemark(coordinate: CLLocationCoordinate2D(latitude: p.lat, longitude: p.lng)))
        }
        return MKMapItem.forCurrentLocation()
    }

    private func place(_ name: String, _ at: Coordinate) -> MKMapItem {
        let item = MKMapItem(placemark: MKPlacemark(coordinate: CLLocationCoordinate2D(latitude: at.lat, longitude: at.lng)))
        item.name = name
        return item
    }

    // MARK: Alerts

    func present(_ a: ShownAlert) {
        if let was = alert {
            alert = nil
            if !answeredIDs.contains(alertID) { gone(alertID, -1) }
            _ = was
            template.dismissNavigationAlert(animated: false) { _ in }
        }
        alertID = a.id
        let id = a.id
        func action(_ b: ShownAction) -> CPAlertAction {
            let style: CPAlertAction.Style = b.style == 1 ? .cancel : b.style == 2 ? .destructive : .default
            let run = b.run
            return CPAlertAction(title: b.title, style: style) { [weak self] _ in
                MainActor.assumeIsolated { self?.gone(id, run) }
            }
        }
        let ok = Bundle(identifier: "com.apple.UIKitCore")?.localizedString(forKey: "OK", value: "OK", table: nil) ?? "OK"
        let primary = a.actions.first.map(action) ?? CPAlertAction(title: ok, style: .cancel) { [weak self] _ in
            MainActor.assumeIsolated { self?.gone(id, -1) }
        }
        let n = CPNavigationAlert(titleVariants: [a.title], subtitleVariants: a.detail.isEmpty ? nil : [a.detail],
                                  image: a.symbol.isEmpty ? nil : UIImage(systemName: a.symbol), primaryAction: primary,
                                  secondaryAction: a.actions.count > 1 ? action(a.actions[1]) : nil, duration: a.duration)
        alert = n
        template.present(navigationAlert: n, animated: true)
    }

    func dismissAlert(_ id: Int) {
        guard id == alertID, alert != nil else { return }
        answeredIDs.insert(id)
        alert = nil
        template.dismissNavigationAlert(animated: true) { _ in }
    }

    /// Alert id went, with button n (-1: without one): said once.
    private func gone(_ id: Int, _ n: Int) {
        guard !answeredIDs.contains(id) else { return }
        answeredIDs.insert(id)
        if id == alertID { alert = nil }
        answered(id, n)
    }

    // MARK: CPMapTemplateDelegate

    nonisolated func mapTemplate(_ t: CPMapTemplate, selectedPreviewFor trip: CPTrip, using routeChoice: CPRouteChoice) {
        MainActor.assumeIsolated {
            if let i = trip.routeChoices.firstIndex(where: { $0 === routeChoice }) { highlight(i) }
        }
    }

    nonisolated func mapTemplate(_ t: CPMapTemplate, startedTrip trip: CPTrip, using routeChoice: CPRouteChoice) {
        MainActor.assumeIsolated {
            if let run = routeChoice.userInfo as? Int, run > 0 { chose(run) }
        }
    }

    nonisolated func mapTemplateDidCancelNavigation(_ t: CPMapTemplate) {
        MainActor.assumeIsolated {
            session = nil
            trip = nil
            maneuversMade = ""
            if let stop = shown.trip?.stop, stop > 0 { chose(stop) }
        }
    }

    nonisolated func mapTemplateDidBeginPanGesture(_ t: CPMapTemplate) {
        MainActor.assumeIsolated { map.panBegan() }
    }

    nonisolated func mapTemplate(_ t: CPMapTemplate, didUpdatePanGestureWithTranslation translation: CGPoint, velocity: CGPoint) {
        MainActor.assumeIsolated { map.pan(translation: translation) }
    }

    nonisolated func mapTemplate(_ t: CPMapTemplate, didEndPanGestureWithVelocity velocity: CGPoint) {
        MainActor.assumeIsolated { map.panEnded() }
    }

    nonisolated func mapTemplate(_ t: CPMapTemplate, panWith direction: CPMapTemplate.PanDirection) {
        MainActor.assumeIsolated { map.pan(direction) }
    }

    nonisolated func mapTemplate(_ t: CPMapTemplate, didDismiss navigationAlert: CPNavigationAlert, dismissalContext: CPNavigationAlert.DismissalContext) {
        MainActor.assumeIsolated {
            if navigationAlert === alert { gone(alertID, -1) }
        }
    }

    // MARK: What a test reads

    var mapButtonsShown: Int { template.mapButtons.filter { !$0.isHidden }.count }
}

/// The maneuvers' arrows: an SF Symbol each, and CarPlay's maneuver type.
enum Turns {
    static func symbol(_ t: Turn) -> String {
        switch t {
        case .depart: return "location.north.fill"
        case .slightLeft, .keepLeft, .exitLeft: return "arrow.up.left"
        case .left: return "arrow.turn.up.left"
        case .sharpLeft: return "arrow.down.left"
        case .slightRight, .keepRight, .exitRight: return "arrow.up.right"
        case .right: return "arrow.turn.up.right"
        case .sharpRight: return "arrow.down.right"
        case .uTurn: return "arrow.uturn.down"
        case .merge: return "arrow.triangle.merge"
        case .roundabout: return "arrow.triangle.turn.up.right.circle"
        case .ferry: return "ferry"
        case .arrive, .arriveLeft, .arriveRight: return "flag.checkered"
        default: return "arrow.up"
        }
    }

    static func image(_ t: Turn) -> UIImage? {
        UIImage(systemName: symbol(t), withConfiguration: UIImage.SymbolConfiguration(pointSize: 40, weight: .bold))
    }

    @available(iOS 17.4, *)
    static func type(_ t: Turn, exit: Int) -> CPManeuverType {
        switch t {
        case .depart: return .startRoute
        case .slightLeft: return .slightLeftTurn
        case .left: return .leftTurn
        case .sharpLeft: return .sharpLeftTurn
        case .slightRight: return .slightRightTurn
        case .right: return .rightTurn
        case .sharpRight: return .sharpRightTurn
        case .uTurn: return .uTurn
        case .keepLeft: return .keepLeft
        case .keepRight: return .keepRight
        case .merge: return .changeHighway
        case .exitLeft: return .highwayOffRampLeft
        case .exitRight: return .highwayOffRampRight
        case .roundabout:
            if exit >= 1 && exit <= 19, let r = CPManeuverType(rawValue: CPManeuverType.roundaboutExit1.rawValue + UInt(exit - 1)) { return r }
            return .enterRoundabout
        case .ferry: return .enter_Ferry
        case .arrive: return .arriveAtDestination
        case .arriveLeft: return .arriveAtDestinationLeft
        case .arriveRight: return .arriveAtDestinationRight
        default: return .straightAhead
        }
    }
}
