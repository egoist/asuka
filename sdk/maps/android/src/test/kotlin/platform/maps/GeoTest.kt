// platform/maps's fit on Android, on this computer's JVM: the camera that
// shows coordinates with padding at each edge, north up and turned.
package platform.maps

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin

class GeoTest {
    private val route = listOf(
        Coordinate(lat = 37.8080, lng = -122.4177),
        Coordinate(lat = 37.7955, lng = -122.3937),
        Coordinate(lat = 37.7694, lng = -122.4862),
    )

    /** Where c is on a w×h map with camera (center, zoom), turned to heading: its x and y in dp. */
    private fun screen(c: Coordinate, center: Coordinate, zoom: Double, heading: Double, w: Double, h: Double): Pair<Double, Double> {
        val k = 256 * 2.0.pow(zoom)
        var dl = c.lng - center.lng
        dl -= 360 * Math.round(dl / 360).toDouble()
        val dx = dl / 360 * k
        val dy = (Geo.mercY(c.lat) - Geo.mercY(center.lat)) * k
        val t = heading * PI / 180
        return (w / 2 + dx * cos(t) + dy * sin(t)) to (h / 2 - dx * sin(t) + dy * cos(t))
    }

    private fun checkFits(points: List<Coordinate>, w: Double, h: Double, pad: Insets, heading: Double) {
        val (center, zoom) = Geo.fit(points, w, h, pad, heading)!!
        var x0 = Double.MAX_VALUE; var x1 = -Double.MAX_VALUE; var y0 = Double.MAX_VALUE; var y1 = -Double.MAX_VALUE
        for (p in points) {
            val (x, y) = screen(p, center, zoom, heading, w, h)
            x0 = min(x0, x); x1 = max(x1, x); y0 = min(y0, y); y1 = max(y1, y)
        }
        val eps = 1e-6
        // Inside the room the padding leaves...
        assertTrue("left $x0", x0 >= pad.left - eps)
        assertTrue("right $x1", x1 <= w - pad.right + eps)
        assertTrue("top $y0", y0 >= pad.top - eps)
        assertTrue("bottom $y1", y1 <= h - pad.bottom + eps)
        // ...touching it on one axis, centered in it on both.
        val touches = Math.abs(x0 - pad.left) < 1e-3 || Math.abs(y0 - pad.top) < 1e-3
        assertTrue("fills the room", touches)
        assertEquals((pad.left + w - pad.right) / 2, (x0 + x1) / 2, 1e-3)
        assertEquals((pad.top + h - pad.bottom) / 2, (y0 + y1) / 2, 1e-3)
    }

    @Test fun fitsNorthUp() = checkFits(route, 390.0, 700.0, Insets(), 0.0)

    @Test fun fitsWithPaddingAtEachEdge() = checkFits(route, 390.0, 700.0, Insets(top = 20.0, left = 40.0, bottom = 300.0, right = 10.0), 0.0)

    @Test fun fitsTurned() = checkFits(route, 390.0, 700.0, Insets(top = 48.0, left = 48.0, bottom = 48.0, right = 48.0), 45.0)

    @Test fun fitsAcrossThe180thMeridian() {
        val pts = listOf(Coordinate(lat = -17.0, lng = 178.0), Coordinate(lat = -18.0, lng = -179.0))
        checkFits(pts, 400.0, 400.0, Insets(), 0.0)
        val (center, _) = Geo.fit(pts, 400.0, 400.0, Insets(), 0.0)!!
        assertEquals(179.5, center.lng, 1e-6)
    }

    @Test fun aPointIsCenteredAtStreetZoom() {
        val p = Coordinate(lat = 48.8566, lng = 2.3522)
        val (center, zoom) = Geo.fit(listOf(p, p), 390.0, 700.0, Insets(), 0.0)!!
        assertEquals(Geo.POINT_ZOOM, zoom, 0.0)
        assertEquals(p.lat, center.lat, 1e-9)
        assertEquals(p.lng, center.lng, 1e-9)
    }

    @Test fun theZoomStopsAtMax() {
        val pts = listOf(Coordinate(lat = 0.0, lng = 0.0), Coordinate(lat = 0.000001, lng = 0.000001))
        assertEquals(19.0, Geo.fit(pts, 390.0, 700.0, Insets(), 0.0, maxZoom = 19.0)!!.second, 0.0)
    }

    @Test fun noneIsNull() = assertNull(Geo.fit(emptyList(), 390.0, 700.0, Insets(), 0.0))

    @Test fun mercatorRoundTrips() {
        for (lat in listOf(-80.0, -33.9, 0.0, 37.77, 85.0)) assertEquals(lat, Geo.mercLat(Geo.mercY(lat)), 1e-9)
        assertEquals(-180.0, Geo.normal(180.0), 0.0)
        assertEquals(-170.0, Geo.normal(190.0), 1e-9)
    }
}
