// platform/maps on Android: the geometry Google's SDK leaves to us, pure
// Kotlin (unit tested on the JVM): the camera that fits coordinates with
// padding at each edge, the map turned to a heading.
package platform.maps

import kotlin.math.PI
import kotlin.math.atan
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.ln
import kotlin.math.log2
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sinh
import kotlin.math.tan

internal object Geo {
    /** The zoom a fit of a single place (or of places all at one point) shows. */
    const val POINT_ZOOM = 15.0

    /** A latitude's Web Mercator y: 0 at the top of the world, 1 at its bottom. */
    fun mercY(lat: Double): Double {
        val l = max(min(lat, 85.05112878), -85.05112878) * PI / 180
        return (1 - ln(tan(l) + 1 / cos(l)) / PI) / 2
    }

    /** The latitude at a Web Mercator y. */
    fun mercLat(y: Double): Double = atan(sinh(PI * (1 - 2 * y))) * 180 / PI

    /** A longitude in [-180, 180). */
    fun normal(lng: Double): Double {
        val l = (lng + 180) - 360 * floor((lng + 180) / 360)
        return l - 180
    }

    /**
     * The camera that shows every coordinate as near as it can in a map
     * width×height (in dp), with padding's room at its edges (dp), turned
     * to heading (degrees clockwise from north) and flat: its center and
     * zoom, the zoom at most maxZoom. Places at one point: centered at
     * POINT_ZOOM (or maxZoom, if less). Null for none.
     */
    fun fit(points: List<Coordinate>, width: Double, height: Double, padding: Insets, heading: Double, maxZoom: Double = 21.0): Pair<Coordinate, Double>? {
        if (points.isEmpty()) return null
        // The places in world units (the world 1 wide at zoom 0), each
        // longitude the short way round from the first's.
        val lng0 = points[0].lng
        val ox = (lng0 + 180) / 360
        val oy = mercY(points[0].lat)
        val t = heading * PI / 180
        val c = cos(t)
        val s = sin(t)
        var x0 = Double.MAX_VALUE
        var x1 = -Double.MAX_VALUE
        var y0 = Double.MAX_VALUE
        var y1 = -Double.MAX_VALUE
        for (p in points) {
            var dl = p.lng - lng0
            dl -= 360 * Math.round(dl / 360).toDouble()
            val dx = dl / 360
            val dy = mercY(p.lat) - oy
            // Where it is on the screen of a map turned to heading.
            val sx = dx * c + dy * s
            val sy = -dx * s + dy * c
            x0 = min(x0, sx); x1 = max(x1, sx)
            y0 = min(y0, sy); y1 = max(y1, sy)
        }
        val w = max(width - padding.left - padding.right, 1.0)
        val h = max(height - padding.top - padding.bottom, 1.0)
        val sw = x1 - x0
        val sh = y1 - y0
        var zoom = if (sw <= 1e-12 && sh <= 1e-12) min(POINT_ZOOM, maxZoom) else {
            var z = maxZoom
            if (sw > 1e-12) z = min(z, log2(w / (256 * sw)))
            if (sh > 1e-12) z = min(z, log2(h / (256 * sh)))
            z
        }
        zoom = max(zoom, 0.0)
        val scale = 256 * Math.pow(2.0, zoom) // dp a world unit
        // The middle of what's fitted, moved so it's at the middle of the
        // room the padding leaves.
        val mx = (x0 + x1) / 2 - (padding.left - padding.right) / 2 / scale
        val my = (y0 + y1) / 2 - (padding.top - padding.bottom) / 2 / scale
        val dx = mx * c - my * s
        val dy = mx * s + my * c
        val lng = normal((ox + dx) * 360 - 180)
        val lat = mercLat(oy + dy)
        return Coordinate(lat = lat, lng = lng) to zoom
    }
}
