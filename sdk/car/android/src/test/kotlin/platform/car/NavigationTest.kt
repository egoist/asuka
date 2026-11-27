// platform/car's navigation apps on Android Auto, on this computer's JVM
// (Robolectric with its native graphics, and the Car App Library's test
// library: a TestCarContext stands in for the car): a .navigation root's
// NavigationTemplate (the trip's routing info and estimate, the map's
// actions, the app's), the trip told to the NavigationManager and ended
// by the car, a preview's RoutePreviewNavigationTemplate, alerts; and the
// map drawn on the car's surface, read back as pixels: the tiles, the
// route's line, the puck, the driver's pan and zoom.
package platform.car

import android.content.Intent
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Rect
import android.graphics.drawable.ColorDrawable
import androidx.car.app.OnDoneCallback
import androidx.car.app.model.Action
import androidx.car.app.model.OnClickDelegate
import androidx.car.app.model.Row
import androidx.car.app.navigation.model.Maneuver as CarManeuver
import androidx.car.app.navigation.model.NavigationTemplate
import androidx.car.app.navigation.model.RoutePreviewNavigationTemplate
import androidx.car.app.navigation.model.RoutingInfo
import androidx.car.app.serialization.Bundleable
import androidx.car.app.testing.SessionController
import androidx.car.app.testing.TestAppManager
import androidx.car.app.testing.TestCarContext
import androidx.car.app.testing.TestScreenManager
import androidx.car.app.testing.navigation.TestNavigationManager
import androidx.lifecycle.Lifecycle
import androidx.test.core.app.ApplicationProvider
import java.io.ByteArrayOutputStream
import kotlin.time.Duration.Companion.seconds
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@GraphicsMode(GraphicsMode.Mode.NATIVE)
// Native graphics is API level 26's and later's; France: kilometers, and
// traffic on the right.
@Config(sdk = [35], qualifiers = "fr-rFR")
class NavigationTest {
    private lateinit var car: TestCarContext
    private val chosen = ArrayList<Long>()
    private val answered = ArrayList<Pair<Long, Long>>()
    private var stop: () -> Unit = {}
    private var controller: SessionController? = null

    private val home = Coordinate(lat = 35.6812, lng = 139.7671)
    private val route = listOf(home, Coordinate(lat = 35.6830, lng = 139.7700), Coordinate(lat = 35.6858, lng = 139.7745))
    private val map = Map(center = home, zoom = 15.0, position = home, follow = true, route = route,
        markers = listOf(Marker(at = route[2], title = "Office")))
    private val trip = ShownTrip(destination = "Office", maneuvers = listOf(
        Maneuver(instruction = "Turn right onto Main St", turn = Turn.RIGHT, road = "Main St", distance = 120.0, time = 30.seconds),
        Maneuver(instruction = "Take the 2nd exit", turn = Turn.ROUNDABOUT, distance = 800.0, time = 90.seconds, exit = 2),
    ), distance = 1500.0, time = 420.seconds, stop = 9)
    private val root = Shown(id = 1, kind = Kind.NAVIGATION, map = map, actions = listOf(ShownAction(run = 3, title = "End", enabled = true)))

    @Before fun setUp() {
        car = TestCarContext.createCarContext(ApplicationProvider.getApplicationContext())
        level(7)
        CarHub.starter = {}
        CarImages.symbols = { _, _ -> ColorDrawable(Color.WHITE) }
        Tiles.downloader = { _, _ -> null } // no network: the sessions' maps have no tiles
        CarHub.show(emptyList())
        stop = CarHub.listen({ chosen.add(it) }, { }, { id, n -> answered.add(id to n) })
    }

    @After fun tearDown() {
        stop()
        controller?.moveToState(Lifecycle.State.DESTROYED)
        CarHub.session = null
    }

    private fun level(n: Int) {
        car.updateHandshakeInfo(androidx.car.app.HandshakeInfo("com.google.android.projection.gearhead", n))
    }

    private val done = object : OnDoneCallback {
        override fun onSuccess(response: Bundleable?) {}
        override fun onFailure(response: Bundleable) {}
    }

    private fun click(d: OnClickDelegate?) = d!!.sendClick(done)

    /** A session on the car, its root shown, started. */
    private fun session(vararg stack: Shown): AsukaCarSession {
        val s = AsukaCarSession()
        controller = SessionController(s, car, Intent()).also { it.moveToState(Lifecycle.State.RESUMED) }
        CarHub.show(stack.toList())
        return s
    }

    private fun rootTemplate() = car.getCarService(TestScreenManager::class.java).screensPushed.first().let { (it as TemplateScreen).onGetTemplate() }

    @Test fun aTripsGuidance() {
        val s = session(root.copy(trip = trip))
        val nav = s.navigator!!
        // The map draws on the car's surface.
        assertTrue(car.getCarService(TestAppManager::class.java).surfaceCallback === nav.map)
        val t = rootTemplate() as NavigationTemplate
        val info = t.navigationInfo as RoutingInfo
        assertEquals("Turn right onto Main St", info.currentStep!!.cue.toString())
        assertEquals(CarManeuver.TYPE_TURN_NORMAL_RIGHT, info.currentStep!!.maneuver!!.type)
        assertNotNull(info.currentStep!!.maneuver!!.icon)
        assertEquals("Main St", info.currentStep!!.road.toString())
        assertEquals(120.0, info.currentDistance!!.displayDistance, 1e-9)
        assertEquals(CarManeuver.TYPE_ROUNDABOUT_ENTER_AND_EXIT_CCW, info.nextStep!!.maneuver!!.type)
        assertEquals(2, info.nextStep!!.maneuver!!.roundaboutExitNumber)
        assertEquals(1.5, t.destinationTravelEstimate!!.remainingDistance!!.displayDistance, 1e-9)
        assertEquals(420L, t.destinationTravelEstimate!!.remainingTimeSeconds)
        // The app's action, and the map's: pan, zoom in, zoom out.
        assertEquals("End", t.actionStrip!!.actions[0].title.toString())
        click(t.actionStrip!!.actions[0].onClickDelegate)
        assertEquals(listOf(3L), chosen)
        val mapActions = t.mapActionStrip!!.actions
        assertEquals(3, mapActions.size)
        assertEquals(Action.TYPE_PAN, mapActions[0].type)
        // The car heard the trip start, and has it.
        val nm = car.getCarService(TestNavigationManager::class.java)
        assertEquals(1, nm.navigationStartedCount)
        val sent = nm.tripsSent.last()
        assertEquals("Office", sent.destinations[0].name.toString())
        assertEquals(2, sent.steps.size)
        // Nearer: the same trip, one start.
        CarHub.show(listOf(root.copy(trip = trip.copy(maneuvers = listOf(trip.maneuvers[0].copy(distance = 60.0), trip.maneuvers[1])))))
        assertEquals(1, nm.navigationStartedCount)
        val nearer = (rootTemplate() as NavigationTemplate).navigationInfo as RoutingInfo
        assertEquals(60.0, nearer.currentDistance!!.displayDistance, 1e-9)
        // Rerouting: the car says it's loading.
        CarHub.show(listOf(root.copy(trip = trip.copy(rerouting = true))))
        assertTrue(((rootTemplate() as NavigationTemplate).navigationInfo as RoutingInfo).isLoading)
        assertTrue(nm.tripsSent.last().isLoading)
        // The car ends it (another app navigates): the trip's stop.
        nm.navigationManagerCallback!!.onStopNavigation()
        assertEquals(listOf(3L, 9L), chosen)
        // The app's trip goes: free driving, the car told.
        CarHub.show(listOf(root))
        assertNull((rootTemplate() as NavigationTemplate).navigationInfo)
        CarHub.show(listOf(root.copy(trip = trip)))
        assertEquals(2, nm.navigationStartedCount)
        CarHub.show(listOf(root))
        assertEquals(1, nm.navigationEndedCount)
    }

    @Test fun noActionsStillAStrip() {
        session(root.copy(actions = emptyList()))
        val t = rootTemplate() as NavigationTemplate
        assertEquals(Action.TYPE_APP_ICON, t.actionStrip!!.actions[0].type)
    }

    @Test fun aPreviewsRoutes() {
        val fast = ShownRoute(run = 7, title = "Fastest", distance = 1200.0, time = 300.seconds, path = route)
        val quiet = ShownRoute(run = 8, title = "Quiet", detail = "No tolls", distance = 1500.0, time = 420.seconds,
            path = listOf(route[0], Coordinate(lat = 35.684, lng = 139.768), route[2]))
        val s = session(root.copy(preview = ShownPreview(destination = "Office", at = route[2], routes = listOf(fast, quiet))))
        val t = rootTemplate() as RoutePreviewNavigationTemplate
        assertEquals("Office", t.title.toString())
        val rows = t.itemList!!.items.map { it as Row }
        assertEquals(listOf("Fastest", "Quiet"), rows.map { it.title.toString() })
        assertTrue(rows[1].texts[0].toString().endsWith("No tolls"))
        assertEquals(0, t.itemList!!.selectedIndex)
        // The map draws both, the first over the other.
        val st = s.navigator!!.map.state
        assertEquals(2, st.previews.size)
        assertEquals(0, st.highlighted)
        // The driver highlights the second, then starts it.
        t.itemList!!.onSelectedDelegate!!.sendSelected(1, done)
        assertEquals(1, st.highlighted)
        val again = rootTemplate() as RoutePreviewNavigationTemplate
        assertEquals(1, again.itemList!!.selectedIndex)
        // No title from the app: the route's, with an arrow.
        assertEquals("Quiet", again.navigateAction!!.title.toString())
        assertNotNull(again.navigateAction!!.icon)
        click(again.navigateAction!!.onClickDelegate)
        assertEquals(listOf(8L), chosen)
        // The trip starts: guidance, the preview's lines gone.
        CarHub.show(listOf(root.copy(trip = trip)))
        assertTrue(rootTemplate() is NavigationTemplate)
        assertTrue(st.previews.isEmpty())
    }

    @Test fun alerts() {
        val s = session(root)
        CarHub.alert(ShownAlert(id = 4, title = "Traffic ahead", detail = "10 min slower",
            actions = listOf(ShownAction(run = 2, title = "Reroute", enabled = true), ShownAction(run = 0, title = "Ignore", style = 1, enabled = true)), duration = 8.seconds))
        val a = s.navigator!!.alert!!
        assertEquals("Traffic ahead", a.title.toString())
        assertEquals("10 min slower", a.subtitle.toString())
        assertEquals(listOf("Reroute", "Ignore"), a.actions.map { it.title.toString() })
        assertEquals(8000L, a.durationMillis)
        click(a.actions[0].onClickDelegate)
        assertEquals(listOf(4L to 2L), answered)
        // Its time up: gone without a button, said once.
        CarHub.alert(ShownAlert(id = 5, title = "Speed camera", duration = 5.seconds))
        s.navigator!!.gone(5, -1)
        s.navigator!!.gone(5, -1)
        assertEquals(listOf(4L to 2L, 5L to -1L), answered)
        // Before Car App API level 5: a toast of its title.
        level(4)
        CarHub.alert(ShownAlert(id = 6, title = "Road closed"))
        assertTrue(car.getCarService(TestAppManager::class.java).toastsShown.map { it.toString() }.contains("Road closed"))
        assertEquals(6L to -1L, answered.last())
    }

    // ---- The map on the car's surface ----

    /** A tile: a 256-pixel PNG of one color. */
    private fun tile(color: Int): ByteArray {
        val b = Bitmap.createBitmap(256, 256, Bitmap.Config.ARGB_8888)
        b.eraseColor(color)
        return ByteArrayOutputStream().also { b.compress(Bitmap.CompressFormat.PNG, 100, it) }.toByteArray()
    }

    private val tileColor = 0xFF8899AA.toInt()

    /** The map's painter and state, its tiles served at once. */
    private fun painted(m: Map, w: Int = 400, h: Int = 240, visible: Rect = Rect(0, 0, w, h)): Triple<Bitmap, MapState, MapPainter> {
        val asked = ArrayList<String>()
        val tiles = Tiles(car, loaded = {}, workers = { it.run() }, post = { it() }, fetch = { url -> asked.add(url); tile(tileColor) })
        tiles.template = m.tiles
        val p = MapPainter(tiles)
        val s = MapState().apply { show(m) }
        val b = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        p.draw(Canvas(b), w, h, visible, 1f, s, false) // asks for the tiles
        p.draw(Canvas(b), w, h, visible, 1f, s, false) // draws them
        assertTrue(asked.isNotEmpty())
        assertTrue(asked.all { it.startsWith("https://tile.openstreetmap.org/15/") })
        return Triple(b, s, p)
    }

    private fun near(c: Int, want: Int, tolerance: Int = 24) =
        Math.abs(Color.red(c) - Color.red(want)) <= tolerance && Math.abs(Color.green(c) - Color.green(want)) <= tolerance && Math.abs(Color.blue(c) - Color.blue(want)) <= tolerance

    @Test fun theMapDrawsTilesTheRouteAndThePuck() {
        val (b, _, p) = painted(map, 600, 400) // the route's end on it too
        // The tiles: a corner far from what's drawn over them.
        assertTrue("tile pixel %08x".format(b.getPixel(4, 4)), near(b.getPixel(4, 4), tileColor))
        // The car in the middle: the puck's blue.
        assertTrue("puck %08x".format(b.getPixel(300, 200)), near(b.getPixel(300, 200), MapPainter.PUCK))
        // The route's line, halfway along its first leg.
        val (x0, y0) = p.point(route[0])
        val (x1, y1) = p.point(route[1])
        val mx = ((x0 + x1) / 2).toInt()
        val my = ((y0 + y1) / 2).toInt()
        assertTrue("route %08x at %d,%d".format(b.getPixel(mx, my), mx, my), near(b.getPixel(mx, my), MapPainter.ROUTE))
        // Off it, the tile.
        assertTrue(near(b.getPixel(mx, minOf(my + 40, 399)), tileColor) || near(b.getPixel(mx, maxOf(my - 40, 0)), tileColor))
        // The marker, at the route's end, red.
        val (ex, ey) = p.point(route[2])
        assertTrue("marker %08x".format(b.getPixel(ex.toInt(), ey.toInt())), near(b.getPixel(ex.toInt(), ey.toInt()), MapPainter.PIN))
    }

    @Test fun drawnOnTheCarsSurface() {
        // The session's SurfaceCallback, told of the car's surface and the
        // area the templates leave, as a car tells them: what it draws
        // (the frame it posts to the surface), read back.
        val sess = session(root.copy(map = map.copy(route = emptyList(), markers = emptyList())))
        val cb = car.getCarService(TestAppManager::class.java).surfaceCallback!!
        val container = androidx.car.app.SurfaceContainer(null, 320, 200, 320)
        cb.onSurfaceAvailable(container)
        cb.onVisibleAreaChanged(Rect(120, 0, 320, 200))
        val nav = sess.navigator!!
        assertTrue(nav.map.surface === container && nav.map.visible == Rect(120, 0, 320, 200))
        val b = Bitmap.createBitmap(320, 200, Bitmap.Config.ARGB_8888)
        nav.map.drawOn(Canvas(b), 320, 200)
        // The car in the middle of what's visible, at the car's density
        // (320 dpi: twice 160's), its disc as wide.
        assertTrue("puck %08x".format(b.getPixel(220, 100)), near(b.getPixel(220, 100), MapPainter.PUCK))
        assertTrue("puck's edge %08x".format(b.getPixel(237, 100)), near(b.getPixel(237, 100), MapPainter.PUCK))
        assertTrue("background %08x".format(b.getPixel(2, 2)), near(b.getPixel(2, 2), MapPainter.DAY, 4))
        cb.onSurfaceDestroyed(container)
        assertNull(nav.map.surface)
    }

    @Test fun theCarCenteredInTheVisibleArea() {
        // The templates cover the left: the car in the middle of the rest.
        val (b, _, _) = painted(map, visible = Rect(160, 0, 400, 240))
        assertTrue("puck %08x".format(b.getPixel(280, 120)), near(b.getPixel(280, 120), MapPainter.PUCK))
        assertFalse(near(b.getPixel(200, 120), MapPainter.PUCK))
    }

    @Test fun headingUpTurnsTheMap() {
        // Heading east, followed: east is up, so the route (going northeast) runs up and to the left.
        val (b, s, p) = painted(map.copy(heading = 90.0))
        assertEquals(90.0, s.headingUp, 1e-9)
        val (x1, y1) = p.point(route[1]) // unturned
        // Turned -90° about the middle: (x, y) → (cx + (y - cy), cy - (x - cx)).
        val tx = (200 + (y1 - 120)).toInt()
        val ty = (120 - (x1 - 200)).toInt()
        assertTrue("turned route %08x at %d,%d".format(b.getPixel(tx, ty), tx, ty), near(b.getPixel(tx, ty), MapPainter.ROUTE))
    }

    @Test fun theDriverPansAndZooms() {
        val surface = CarMapSurface(car) { false }
        var moves = 0
        surface.moved = { moves++ }
        surface.show(map)
        val s = surface.state
        assertFalse(s.moved)
        // A drag to the left: the map looks further east, and stops following.
        surface.onScroll(100f, 0f)
        assertTrue(s.moved && s.target.lng > home.lng)
        assertEquals(1, moves)
        // The car moves on: the driver's view stays.
        val panned = s.target
        surface.show(map.copy(position = route[1]))
        assertEquals(panned, s.target)
        // A pinch out about the middle: a zoom in.
        surface.onScale(0f, 0f, 2f)
        assertEquals(16.0, s.zoom, 1e-9)
        // Recentered: the car, the app's zoom.
        surface.recenter()
        assertFalse(s.moved)
        assertEquals(route[1], s.target)
        assertEquals(15.0, s.zoom, 1e-9)
        // The app's own zoom takes the driver's away.
        surface.zoomBy(2.0)
        surface.show(map.copy(position = route[1], zoom = 13.0))
        assertEquals(13.0, s.zoom, 1e-9)
    }

    @Test fun theRecenterActionFollowsTheDriver() {
        val sess = session(root)
        val nav = sess.navigator!!
        assertEquals(3, (rootTemplate() as NavigationTemplate).mapActionStrip!!.actions.size)
        nav.map.onScroll(50f, 0f)
        val actions = (rootTemplate() as NavigationTemplate).mapActionStrip!!.actions
        assertEquals(4, actions.size)
        click(actions[1].onClickDelegate) // recenter
        assertFalse(nav.map.state.moved)
        // Zoom in, from the car's button (pan, zoom in, zoom out again).
        click((rootTemplate() as NavigationTemplate).mapActionStrip!!.actions[1].onClickDelegate)
        assertEquals(16.0, nav.map.state.zoom, 1e-9)
    }

    @Test fun nightDarkensTheTiles() {
        val tiles = Tiles(car, loaded = {}, workers = { it.run() }, post = { it() }, fetch = { tile(Color.WHITE) })
        val p = MapPainter(tiles)
        val s = MapState().apply { show(map.copy(night = true, position = null, route = emptyList(), markers = emptyList())) }
        val b = Bitmap.createBitmap(200, 120, Bitmap.Config.ARGB_8888)
        p.draw(Canvas(b), 200, 120, Rect(), 1f, s, true)
        p.draw(Canvas(b), 200, 120, Rect(), 1f, s, true)
        val c = b.getPixel(100, 60)
        assertTrue("night tile %08x".format(c), Color.red(c) < 80 && Color.green(c) < 80 && Color.blue(c) < 80)
    }

    @Test fun maneuverArrowsPointTheirWay() {
        val right = Maneuvers.icon(Turn.RIGHT, false)
        val left = Maneuvers.icon(Turn.LEFT, false)
        fun ink(b: Bitmap, x0: Int, x1: Int) = (x0 until x1).sumOf { x -> (0 until 128).count { y -> Color.alpha(b.getPixel(x, y)) > 128 } }
        assertTrue(ink(right, 80, 128) > ink(right, 0, 48))
        assertTrue(ink(left, 0, 48) > ink(left, 80, 128))
        assertEquals(CarManeuver.TYPE_U_TURN_RIGHT, Maneuvers.type(Turn.U_TURN, 0, left = true))
        assertEquals(CarManeuver.TYPE_ROUNDABOUT_ENTER_CCW, Maneuvers.type(Turn.ROUNDABOUT, 0, left = false))
    }
}
