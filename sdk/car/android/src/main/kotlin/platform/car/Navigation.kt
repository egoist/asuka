// platform/car's .navigation on Android Auto: a navigation app's root.
// Its map is drawn on the car's surface (CarMap.kt), under the Car App
// Library's NavigationTemplate: the map's actions (pan, zoom, recenter),
// the app's in its action strip, and while there's a trip, its routing
// info (the next step's maneuver, its instruction and distance, the step
// after) and the destination's travel estimate; the trip told to the car
// too (NavigationManager: started, each update, ended), which may end it
// itself (onStopNavigation: the trip's stop). A preview is a
// RoutePreviewNavigationTemplate: its routes as rows, the highlighted one
// drawn on the map, the navigate action starting it. Alerts are the
// AppManager's (Car App API level 5; before, a toast).
package platform.car

import android.text.SpannableString
import android.text.Spanned
import android.text.TextUtils
import androidx.car.app.AppManager
import androidx.car.app.CarContext
import androidx.car.app.CarToast
import androidx.car.app.model.Action
import androidx.car.app.model.ActionStrip
import androidx.car.app.model.Alert
import androidx.car.app.model.AlertCallback
import androidx.car.app.model.CarColor
import androidx.car.app.model.CarIcon
import androidx.car.app.model.CarText
import androidx.car.app.model.DateTimeWithZone
import androidx.car.app.model.Distance
import androidx.car.app.model.DistanceSpan
import androidx.car.app.model.DurationSpan
import androidx.car.app.model.ItemList
import androidx.car.app.model.Row
import androidx.car.app.model.Template
import androidx.car.app.navigation.NavigationManager
import androidx.car.app.navigation.NavigationManagerCallback
import androidx.car.app.navigation.model.Destination
import androidx.car.app.navigation.model.NavigationTemplate
import androidx.car.app.navigation.model.RoutePreviewNavigationTemplate
import androidx.car.app.navigation.model.RoutingInfo
import androidx.car.app.navigation.model.Step
import androidx.car.app.navigation.model.TravelEstimate
import androidx.car.app.navigation.model.Trip as CarTrip
import androidx.car.app.navigation.model.Maneuver as CarManeuver
import androidx.car.app.versioning.CarAppApiLevels
import androidx.core.graphics.drawable.IconCompat
import java.util.TimeZone

/** meters, in the units of the car's country: miles where roads are signed in them. */
internal fun carDistance(context: android.content.Context, meters: Double): Distance {
    val miles = context.resources.configuration.locales[0].country in setOf("US", "GB", "LR", "MM")
    val m = maxOf(meters, 0.0)
    return when {
        miles && m < 300 -> Distance.create(m / 0.3048, Distance.UNIT_FEET)
        miles -> Distance.create(m / 1609.344, Distance.UNIT_MILES)
        m < 1000 -> Distance.create(m, Distance.UNIT_METERS)
        else -> Distance.create(m / 1000, Distance.UNIT_KILOMETERS)
    }
}

/**
 * A navigation app's root on the car: its map on the car's surface, its
 * trip told to the car, its alerts.
 */
internal class CarNavigator(private val car: CarContext) {
    val map = CarMapSurface(car) { car.isDarkMode }
    private var started = false
    /** Whether the car has heard a trip started, and its stop's number. */
    var navigating = false
        private set
    private var stop = 0L
    /** The route of the preview highlighted. */
    var selected = 0
    private var shown: Shown? = null
    /** The alert showing, and the ids said gone. */
    var alert: Alert? = null
        private set
    private var alertID = 0L
    private val answeredIDs = HashSet<Long>()

    /** Shows the root: its map, its preview's routes, its trip. */
    fun follow(root: Shown) {
        if (!started) {
            started = true
            car.getCarService(AppManager::class.java).setSurfaceCallback(map)
            car.getCarService(NavigationManager::class.java).setNavigationManagerCallback(object : NavigationManagerCallback {
                override fun onStopNavigation() {
                    navigating = false
                    if (stop > 0) CarHub.chose(stop)
                }
            })
        }
        val was = shown
        shown = root
        if (was?.preview?.routes?.map { it.title } != root.preview?.routes?.map { it.title }) selected = 0
        map.show(root.map ?: Map(zoom = 15.0, follow = true))
        val routes = root.preview?.routes.orEmpty()
        map.preview(if (root.trip == null) routes.map { it.path } else emptyList(), selected)
        trip(root.trip)
    }

    /** The preview's route i highlighted: drawn over the others. */
    fun highlight(i: Int) {
        selected = i
        val routes = shown?.preview?.routes.orEmpty()
        if (shown?.trip == null) map.preview(routes.map { it.path }, i)
    }

    private fun trip(t: ShownTrip?) {
        val nm = car.getCarService(NavigationManager::class.java)
        if (t == null) {
            if (navigating) nm.navigationEnded()
            navigating = false
            return
        }
        stop = t.stop
        if (!navigating) {
            nm.navigationStarted()
            navigating = true
        }
        nm.updateTrip(carTrip(t))
    }

    /** The trip as the car's cluster and the host have it: its destination, its steps, their estimates. */
    fun carTrip(t: ShownTrip): CarTrip = CarTrip.Builder().apply {
        addDestination(Destination.Builder().setName(t.destination.ifEmpty { " " }).build(), estimate(t.distance, t.time.inWholeSeconds))
        if (!t.rerouting) {
            for (m in t.maneuvers.take(2)) addStep(step(m), estimate(m.distance, m.time.inWholeSeconds))
            t.maneuvers.firstOrNull()?.road?.takeIf { it.isNotEmpty() }?.let { setCurrentRoad(it) }
        }
        setLoading(t.rerouting)
    }.build()

    fun step(m: Maneuver): Step {
        val left = Maneuvers.leftHand(car)
        val man = CarManeuver.Builder(Maneuvers.type(m.turn, m.exit, left)).apply {
            setIcon(CarIcon.Builder(IconCompat.createWithBitmap(Maneuvers.icon(m.turn, left))).build())
            if (m.turn == Turn.ROUNDABOUT && m.exit > 0) setRoundaboutExitNumber(m.exit.toInt())
        }.build()
        return Step.Builder(m.instruction).setManeuver(man).apply {
            if (m.road.isNotEmpty()) setRoad(m.road)
        }.build()
    }

    fun estimate(meters: Double, seconds: Long): TravelEstimate {
        val arrival = System.currentTimeMillis() + maxOf(seconds, 0) * 1000
        return TravelEstimate.Builder(carDistance(car, meters), DateTimeWithZone.create(arrival, TimeZone.getDefault()))
            .setRemainingTimeSeconds(maxOf(seconds, 0)).build()
    }

    // ---- Alerts ----

    fun present(a: ShownAlert) {
        alert?.let {
            alert = null
            gone(alertID, -1)
            if (car.carAppApiLevel >= CarAppApiLevels.LEVEL_5) car.getCarService(AppManager::class.java).dismissAlert(alertID.toInt())
        }
        alertID = a.id
        if (car.carAppApiLevel < CarAppApiLevels.LEVEL_5) {
            // No alerts before level 5: the title, as a toast, and it's gone.
            CarToast.makeText(car, a.title, CarToast.LENGTH_LONG).show()
            gone(a.id, -1)
            return
        }
        val id = a.id
        val b = Alert.Builder(id.toInt(), CarText.create(a.title), a.duration.inWholeMilliseconds.coerceAtLeast(1)).apply {
            if (a.detail.isNotEmpty()) setSubtitle(CarText.create(a.detail))
            CarImages.symbol(car, a.symbol)?.let { setIcon(CarIcon.Builder(IconCompat.createWithBitmap(it)).build()) }
            for (x in a.actions.take(2)) {
                val run = x.run
                addAction(Action.Builder().setTitle(x.title.ifEmpty { " " }).apply {
                    if (x.style == 2L) setBackgroundColor(CarColor.RED)
                    setOnClickListener {
                        gone(id, run)
                        car.getCarService(AppManager::class.java).dismissAlert(id.toInt())
                    }
                }.build())
            }
            setCallback(object : AlertCallback {
                override fun onCancel(reason: Int) = gone(id, -1)
                override fun onDismiss() = gone(id, -1)
            })
        }.build()
        alert = b
        car.getCarService(AppManager::class.java).showAlert(b)
    }

    fun dismiss(id: Long) {
        if (id != alertID || alert == null) return
        answeredIDs.add(id)
        alert = null
        if (car.carAppApiLevel >= CarAppApiLevels.LEVEL_5) car.getCarService(AppManager::class.java).dismissAlert(id.toInt())
    }

    /** Alert id went, with button n (-1: without one): said once. */
    fun gone(id: Long, n: Long) {
        if (!answeredIDs.add(id)) return
        if (id == alertID) alert = null
        CarHub.answered(id, n)
    }

    /** The trip ended with the session: the car hears it. */
    fun end() {
        if (navigating) car.getCarService(NavigationManager::class.java).navigationEnded()
        navigating = false
    }
}

/** A .navigation's template: the routes to choose (a preview, no trip), else the map with the trip's guidance. */
@Suppress("DEPRECATION")
internal fun TemplateScreen.navigationTemplate(s: Shown, nav: CarNavigator?, root: Boolean): Template {
    val preview = s.preview
    val mapStrip = mapActions(nav)
    if (preview != null && preview.routes.isNotEmpty() && s.trip == null) {
        val routes = preview.routes
        val selected = (nav?.selected ?: 0).coerceIn(0, routes.size - 1)
        return RoutePreviewNavigationTemplate.Builder().apply {
            setTitle(preview.destination.ifEmpty { " " })
            setHeaderAction(if (root) Action.APP_ICON else Action.BACK)
            val list = ItemList.Builder()
            for (r in routes) {
                list.addItem(Row.Builder().setTitle(r.title).apply {
                    // How long and how far, in the car's units and language.
                    val span = SpannableString("  ·  ")
                    span.setSpan(DurationSpan.create(r.time.inWholeSeconds), 0, 1, Spanned.SPAN_INCLUSIVE_INCLUSIVE)
                    span.setSpan(DistanceSpan.create(carDistance(carContext, r.distance)), 4, 5, Spanned.SPAN_INCLUSIVE_INCLUSIVE)
                    addText(if (r.detail.isNotEmpty()) SpannableString(TextUtils.concat(span, "  ·  ", r.detail)) else span)
                }.build())
            }
            list.setSelectedIndex(selected)
            list.setOnSelectedListener { i ->
                nav?.highlight(i)
                invalidate()
            }
            setItemList(list.build())
            setNavigateAction(Action.Builder().apply {
                // It needs a title: the app's, else the route's, with an arrow.
                if (preview.start.isNotEmpty()) {
                    setTitle(preview.start)
                } else {
                    setTitle(routes[selected].title)
                    setIcon(CarIcon.Builder(IconCompat.createWithBitmap(Maneuvers.go())).build())
                }
                val run = routes[selected].run
                setOnClickListener { CarHub.chose(run) }
            }.build())
            appActions(s.actions)?.let { setActionStrip(it) }
            setMapActionStrip(mapStrip)
            setPanModeListener { }
        }.build()
    }
    return NavigationTemplate.Builder().apply {
        val trip = s.trip
        if (trip != null && nav != null) {
            if (trip.rerouting || trip.maneuvers.isEmpty()) {
                setNavigationInfo(RoutingInfo.Builder().setLoading(true).build())
            } else {
                val first = trip.maneuvers[0]
                setNavigationInfo(RoutingInfo.Builder().apply {
                    setCurrentStep(nav.step(first), carDistance(carContext, first.distance))
                    trip.maneuvers.getOrNull(1)?.let { setNextStep(nav.step(it)) }
                }.build())
            }
            setDestinationTravelEstimate(nav.estimate(trip.distance, trip.time.inWholeSeconds))
        }
        // The action strip is required: the app's, else the app's icon.
        setActionStrip(appActions(s.actions) ?: ActionStrip.Builder().addAction(Action.APP_ICON).build())
        setMapActionStrip(mapStrip)
        setPanModeListener { }
    }.build()
}

/** The app's actions, as an action strip (four at most; none: null). */
internal fun TemplateScreen.appActions(actions: List<ShownAction>): ActionStrip? {
    if (actions.isEmpty()) return null
    val b = ActionStrip.Builder()
    for (a in actions.take(4)) b.addAction(action(a))
    return b.build()
}

/** The map's own actions: pan, zoom in and out, and recenter while the driver has moved it. */
internal fun TemplateScreen.mapActions(nav: CarNavigator?): ActionStrip {
    fun icon(symbol: String, fallback: String): CarIcon {
        val b = CarImages.symbol(carContext, symbol) ?: CarImages.symbol(carContext, fallback)
        return if (b != null) CarIcon.Builder(IconCompat.createWithBitmap(b)).setTint(CarColor.DEFAULT).build() else CarIcon.APP_ICON
    }
    return ActionStrip.Builder().apply {
        addAction(Action.PAN)
        if (nav?.map?.state?.moved == true) {
            addAction(Action.Builder().setIcon(icon("location.fill", "location")).setOnClickListener { nav.map.recenter() }.build())
        }
        addAction(Action.Builder().setIcon(icon("plus", "plus")).setOnClickListener { nav?.map?.zoomBy(1.0) }.build())
        addAction(Action.Builder().setIcon(icon("minus", "minus")).setOnClickListener { nav?.map?.zoomBy(-1.0) }.build())
    }.build()
}
