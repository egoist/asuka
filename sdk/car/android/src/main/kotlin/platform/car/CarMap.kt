// platform/car's navigation map on Android Auto. A navigation app draws
// its own map on the car's surface (the Car App Library's SurfaceCallback:
// there's no map view for a Surface), so the package draws one, as
// platform/maps does on Windows and the web: raster map tiles from a URL
// template (OpenStreetMap's by default, as its tile policy asks: a
// User-Agent naming the app, two downloads at a time, kept on disk for a
// week, the attribution on the map), placed in Web Mercator, the route as
// a line over them, the markers, the car as a puck, turned heading up
// while following it, centered in the part of the screen the templates
// leave (the visible area), darkened at night. The driver pans with the
// car's touch screen or knob (onScroll, onFling) and zooms (onScale, the
// map's buttons): the map stays where they took it until they recenter or
// the app moves it. On the main thread.
package platform.car

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorMatrix
import android.graphics.ColorMatrixColorFilter
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Rect
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.util.LruCache
import androidx.car.app.SurfaceCallback
import androidx.car.app.SurfaceContainer
import java.io.File
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executor
import java.util.concurrent.Executors
import kotlin.math.PI
import kotlin.math.atan
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.hypot
import kotlin.math.ln
import kotlin.math.log2
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin

/** Web Mercator: a place's world pixels at zoom z (the world 256 · 2^z wide). */
internal object Mercator {
    fun x(lng: Double, z: Double) = (lng + 180) / 360 * 256 * 2.0.pow(z)
    fun y(lat: Double, z: Double): Double {
        val s = sin(lat * PI / 180).coerceIn(-0.9999, 0.9999)
        return (0.5 - ln((1 + s) / (1 - s)) / (4 * PI)) * 256 * 2.0.pow(z)
    }
    fun lng(x: Double, z: Double) = x / (256 * 2.0.pow(z)) * 360 - 180
    fun lat(y: Double, z: Double): Double {
        val n = PI - 2 * PI * y / (256 * 2.0.pow(z))
        return 180 / PI * atan(0.5 * (exp(n) - exp(-n)))
    }
}

/**
 * Where the map looks: the app's [Map], and the driver's moves over it (a
 * center and a zoom of theirs, until they recenter or the app moves it),
 * and a preview's routes, framed.
 */
internal class MapState {
    var given = Map(zoom = 15.0, follow = true)
        private set
    private var givenOnce = false
    var driverCenter: Coordinate? = null
        private set
    var driverZoom: Double? = null
        private set
    var previews: List<List<Coordinate>> = emptyList()
        private set
    var highlighted = 0
        private set

    val moved get() = driverCenter != null || driverZoom != null

    /** Shows m; the app's own move (another zoom, another center, follow changed) takes it back from the driver. */
    fun show(m: Map) {
        val was = given
        given = m
        if (givenOnce) {
            if (m.zoom != was.zoom) driverZoom = null
            if (m.follow != was.follow || (!m.follow && m.center != was.center)) driverCenter = null
        }
        givenOnce = true
    }

    fun preview(paths: List<List<Coordinate>>, highlighted: Int) {
        previews = paths
        this.highlighted = highlighted.coerceIn(0, max(paths.size - 1, 0))
    }

    /** The zoom shown, or the preview's framing (fit, for a size). */
    val zoom get() = (driverZoom ?: given.zoom).coerceIn(1.0, 19.0)

    /** The place in the middle: the driver's, the car's while following it, else the app's center. */
    val target: Coordinate
        get() = driverCenter ?: given.position?.takeIf { given.follow } ?: given.center

    /** The heading up: the car's, while following it with one (else north). */
    val headingUp: Double
        get() = if (driverCenter == null && previews.isEmpty() && given.follow && given.position != null) given.heading ?: 0.0 else 0.0

    fun zoomBy(step: Double) {
        driverZoom = (zoom + step).coerceIn(1.0, 19.0)
    }

    fun recenter() {
        driverCenter = null
        driverZoom = null
    }

    /** The map dragged by (dx, dy) screen pixels (the content goes against them, as GestureDetector's scroll), at density. */
    fun pan(dx: Float, dy: Float, density: Float) {
        val z = zoom
        val h = headingUp * PI / 180
        // A screen vector in world pixels, the map turned h.
        val wx = (dx * cos(h) - dy * sin(h)) / density
        val wy = (dx * sin(h) + dy * cos(h)) / density
        val t = target
        driverCenter = Coordinate(lat = Mercator.lat(Mercator.y(t.lat, z) + wy, z), lng = Mercator.lng(Mercator.x(t.lng, z) + wx, z))
    }

    /** Zoomed by factor about the screen point focus, the middle at mid. */
    fun scale(focusX: Float, focusY: Float, factor: Float, midX: Float, midY: Float, density: Float) {
        val f = if (factor <= 0f) 2f else factor // a double tap
        val before = zoom
        val after = (before + log2(f.toDouble())).coerceIn(1.0, 19.0)
        if (after == before) return
        // The point under the focus stays: the middle moves toward it.
        val k = 1 - 2.0.pow(before - after)
        pan(((focusX - midX) * k).toFloat(), ((focusY - midY) * k).toFloat(), density)
        driverZoom = after
    }
}

/** The map tiles: kept in memory and on disk (a week), downloaded two at a time, the latest asked for first. */
internal class Tiles(
    private val context: Context,
    /** Called on the main thread as a tile comes. */
    private val loaded: () -> kotlin.Unit,
    private val workers: Executor = Executors.newFixedThreadPool(2),
    private val post: (() -> kotlin.Unit) -> kotlin.Unit = { r -> Handler(Looper.getMainLooper()).post(r) },
    private val fetch: (String) -> ByteArray? = { url -> downloader(context, url) },
) {
    var template = ""
    private val memory = object : LruCache<String, Bitmap>(48 * 1024 * 1024) {
        override fun sizeOf(key: String, value: Bitmap) = value.byteCount
    }
    private val loading = HashSet<String>()
    private val failed = HashMap<String, Long>()
    private val queue = ArrayDeque<String>()
    private var running = 0

    val url get() = template.ifEmpty { OSM }

    private fun key(z: Int, x: Int, y: Int) = url.replace("{z}", "$z").replace("{x}", "$x").replace("{y}", "$y")

    /** The tile if it's here; else asked for, and null. */
    fun get(z: Int, x: Int, y: Int): Bitmap? {
        val k = key(z, x, y)
        memory.get(k)?.let { return it }
        val f = failed[k]
        if (f != null && System.currentTimeMillis() - f < 30_000) return null
        if (loading.add(k)) {
            queue.addFirst(k)
            while (queue.size > 64) loading.remove(queue.removeLast())
            pump()
        }
        return null
    }

    /** The tile if it's in memory (for a coarser one drawn while a finer one comes). */
    fun peek(z: Int, x: Int, y: Int): Bitmap? = memory.get(key(z, x, y))

    private fun pump() {
        while (running < 2 && queue.isNotEmpty()) {
            val k = queue.removeFirst()
            running++
            workers.execute {
                val b = load(k)
                post {
                    running--
                    loading.remove(k)
                    if (b != null) {
                        memory.put(k, b)
                        failed.remove(k)
                        loaded()
                    } else {
                        failed[k] = System.currentTimeMillis()
                    }
                    pump()
                }
            }
        }
    }

    /** A tile: the disk's while it's fresh, else the server's, kept. Off the main thread. */
    private fun load(url: String): Bitmap? {
        val file = File(File(context.cacheDir, "asuka-car-tiles"), Integer.toHexString(url.hashCode()) + "-" + url.length)
        try {
            if (file.exists() && System.currentTimeMillis() - file.lastModified() < FRESH) {
                BitmapFactory.decodeFile(file.path)?.let { return it }
            }
            val data = fetch(url) ?: return if (file.exists()) BitmapFactory.decodeFile(file.path) else null
            val b = BitmapFactory.decodeByteArray(data, 0, data.size) ?: return null
            file.parentFile?.mkdirs()
            file.writeBytes(data)
            return b
        } catch (_: Exception) {
            return null
        }
    }

    companion object {
        const val OSM = "https://tile.openstreetmap.org/{z}/{x}/{y}.png"
        const val FRESH = 7L * 24 * 3600 * 1000

        /** How tiles are downloaded (a test's stands in for the network). */
        internal var downloader: (Context, String) -> ByteArray? = { c, url -> download(c, url) }

        /** A tile's bytes, with a User-Agent naming the app (the tile servers' policy). */
        fun download(context: Context, url: String): ByteArray? {
            val c = URL(url).openConnection() as HttpURLConnection
            return try {
                c.connectTimeout = 15_000
                c.readTimeout = 15_000
                c.setRequestProperty("User-Agent", agent(context))
                if (c.responseCode != 200) null else c.inputStream.use { it.readBytes() }
            } finally {
                c.disconnect()
            }
        }

        private fun agent(context: Context): String {
            val label = context.applicationInfo.loadLabel(context.packageManager).toString().replace(Regex("[^A-Za-z0-9._-]"), "")
            val version = try {
                context.packageManager.getPackageInfo(context.packageName, 0).versionName ?: "1"
            } catch (_: Exception) {
                "1"
            }
            return "${label.ifEmpty { "app" }}/$version (${context.packageName}; Android Auto; Asuka platform/car)"
        }
    }
}

/** Draws the map: what [MapState] says, on a canvas of w × h, centered in visible. */
internal class MapPainter(private val tiles: Tiles) {
    private val tilePaint = Paint(Paint.FILTER_BITMAP_FLAG)
    private val nightPaint = Paint(Paint.FILTER_BITMAP_FLAG).apply {
        // Night: the tiles inverted and turned back to their hues, dimmed.
        val invert = ColorMatrix(floatArrayOf(-1f, 0f, 0f, 0f, 255f, 0f, -1f, 0f, 0f, 255f, 0f, 0f, -1f, 0f, 255f, 0f, 0f, 0f, 1f, 0f))
        val hue = ColorMatrix(floatArrayOf(
            -0.574f, 1.43f, 0.144f, 0f, 0f,
            0.426f, 0.43f, 0.144f, 0f, 0f,
            0.426f, 1.43f, -0.856f, 0f, 0f,
            0f, 0f, 0f, 1f, 0f,
        ))
        val dim = ColorMatrix().apply { setScale(0.85f, 0.85f, 0.9f, 1f) }
        invert.postConcat(hue)
        invert.postConcat(dim)
        colorFilter = ColorMatrixColorFilter(invert)
    }
    private val line = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG)
    private val text = Paint(Paint.ANTI_ALIAS_FLAG)

    /** The route's color, a preview's others', the puck's, a marker's default. */
    companion object {
        const val ROUTE = 0xFF1A73E8.toInt()
        const val ROUTE_EDGE = 0xFF0B57D0.toInt()
        const val OTHER = 0xFF9AA0A6.toInt()
        const val PUCK = 0xFF1A73E8.toInt()
        const val PIN = 0xFFEA4335.toInt()
        const val DAY = 0xFFEDEAE2.toInt()
        const val NIGHT = 0xFF202124.toInt()
    }

    /** The screen point a place is drawn at, as the last draw placed it (before turning). */
    private var originX = 0.0
    private var originY = 0.0
    private var drawnZoom = 0.0
    private var drawnDensity = 1f

    fun draw(c: Canvas, w: Int, h: Int, visible: Rect, density: Float, s: MapState, night: Boolean) {
        val vis = if (visible.isEmpty) Rect(0, 0, w, h) else visible
        val midX = vis.exactCenterX()
        val midY = vis.exactCenterY()
        c.drawColor(if (night) NIGHT else DAY)
        var z = s.zoom
        var target = s.target
        if (s.previews.isNotEmpty()) {
            // A preview: its routes framed in what's visible.
            val all = s.previews.flatten()
            if (all.isNotEmpty()) {
                val minX = all.minOf { Mercator.x(it.lng, 0.0) }
                val maxX = all.maxOf { Mercator.x(it.lng, 0.0) }
                val minY = all.minOf { Mercator.y(it.lat, 0.0) }
                val maxY = all.maxOf { Mercator.y(it.lat, 0.0) }
                val pad = 48 * density
                val fx = (vis.width() - 2 * pad) / density / max(maxX - minX, 1e-9)
                val fy = (vis.height() - 2 * pad) / density / max(maxY - minY, 1e-9)
                z = log2(min(fx, fy)).coerceIn(1.0, 17.0)
                target = Coordinate(lat = Mercator.lat((minY + maxY) / 2, 0.0), lng = Mercator.lng((minX + maxX) / 2, 0.0))
            }
        }
        val heading = s.headingUp
        drawnZoom = z
        drawnDensity = density
        // World pixels (at z, times density) of the target, drawn at the middle.
        val tx = Mercator.x(target.lng, z) * density
        val ty = Mercator.y(target.lat, z) * density
        originX = midX - tx
        originY = midY - ty
        c.save()
        c.rotate(-heading.toFloat(), midX, midY)
        drawTiles(c, w, h, midX, midY, tx, ty, z, density, night)
        // The route, or the previews' lines (the highlighted one over the others).
        if (s.previews.isEmpty()) {
            route(c, s.given.route, ROUTE, density)
        } else {
            for ((i, p) in s.previews.withIndex()) if (i != s.highlighted) route(c, p, OTHER, density)
            s.previews.getOrNull(s.highlighted)?.let { route(c, it, ROUTE, density) }
        }
        for (m in s.given.markers) marker(c, m, density, heading, night)
        s.given.position?.let { puck(c, it, s.given.heading?.let { h -> h - heading }, density, heading) }
        c.restore()
        if (tiles.template.isEmpty()) attribution(c, vis, density)
    }

    /** Where a place is on the screen (unturned). */
    fun point(at: Coordinate): Pair<Float, Float> =
        Pair((Mercator.x(at.lng, drawnZoom) * drawnDensity + originX).toFloat(), (Mercator.y(at.lat, drawnZoom) * drawnDensity + originY).toFloat())

    private fun drawTiles(c: Canvas, w: Int, h: Int, midX: Float, midY: Float, tx: Double, ty: Double, z: Double, density: Float, night: Boolean) {
        val zi = floor(z).toInt().coerceIn(0, 19)
        val size = 256 * density * 2.0.pow(z - zi) // a tile's side on the screen
        // What the screen shows, turned any way: a circle around the middle.
        val r = hypot(max(midX, w - midX).toDouble(), max(midY, h - midY).toDouble())
        val n = 1 shl zi
        val x0 = floor((tx - r) / size).toInt()
        val x1 = floor((tx + r) / size).toInt()
        val y0 = max(floor((ty - r) / size).toInt(), 0)
        val y1 = min(floor((ty + r) / size).toInt(), n - 1)
        val paint = if (night) nightPaint else tilePaint
        val dst = RectF()
        for (j in y0..y1) for (i in x0..x1) {
            val left = (i * size - tx + midX).toFloat()
            val top = (j * size - ty + midY).toFloat()
            dst.set(left, top, (left + size).toFloat(), (top + size).toFloat())
            val wx = ((i % n) + n) % n
            val b = tiles.get(zi, wx, j)
            if (b != null) {
                c.drawBitmap(b, null, dst, paint)
                continue
            }
            // Coming: a coarser tile that's here, its part drawn bigger.
            for (up in 1..4) {
                val pz = zi - up
                if (pz < 0) break
                val pb = tiles.peek(pz, wx shr up, j shr up) ?: continue
                val part = 256 shr up
                val sx = (wx and ((1 shl up) - 1)) * part
                val sy = (j and ((1 shl up) - 1)) * part
                val scale = pb.width / 256f
                c.drawBitmap(pb, Rect((sx * scale).toInt(), (sy * scale).toInt(), ((sx + part) * scale).toInt(), ((sy + part) * scale).toInt()), dst, paint)
                break
            }
        }
    }

    private fun route(c: Canvas, points: List<Coordinate>, color: Int, density: Float) {
        if (points.size < 2) return
        val path = Path()
        for ((i, p) in points.withIndex()) {
            val (x, y) = point(p)
            if (i == 0) path.moveTo(x, y) else path.lineTo(x, y)
        }
        if (color == ROUTE) {
            line.color = ROUTE_EDGE
            line.strokeWidth = 10 * density
            c.drawPath(path, line)
        }
        line.color = color
        line.strokeWidth = (if (color == ROUTE) 7 else 6) * density
        c.drawPath(path, line)
    }

    private fun marker(c: Canvas, m: Marker, density: Float, heading: Double, night: Boolean) {
        val (x, y) = point(m.at)
        val color = m.color?.let { dev.asuka.AsukaModules.color(it, PIN) } ?: PIN
        fill.style = Paint.Style.FILL
        fill.color = Color.WHITE
        c.drawCircle(x, y, 10 * density, fill)
        fill.color = color
        c.drawCircle(x, y, 7.5f * density, fill)
        if (m.title.isNotEmpty()) {
            // Its label upright, beside it.
            c.save()
            c.rotate(heading.toFloat(), x, y)
            text.textSize = 14 * density
            text.style = Paint.Style.STROKE
            text.strokeWidth = 3 * density
            text.color = if (night) NIGHT else Color.WHITE
            c.drawText(m.title, x + 14 * density, y + 5 * density, text)
            text.style = Paint.Style.FILL
            text.color = if (night) Color.WHITE else 0xFF202124.toInt()
            c.drawText(m.title, x + 14 * density, y + 5 * density, text)
            c.restore()
        }
    }

    /** The car: a blue disc with a white rim, an arrow on it pointing its way (turn: from the screen's up). */
    private fun puck(c: Canvas, at: Coordinate, turn: Double?, density: Float, heading: Double) {
        val (x, y) = point(at)
        fill.style = Paint.Style.FILL
        fill.color = 0x40000000
        c.drawCircle(x, y, 15 * density, fill)
        fill.color = Color.WHITE
        c.drawCircle(x, y, 13 * density, fill)
        fill.color = PUCK
        c.drawCircle(x, y, 10 * density, fill)
        if (turn != null) {
            c.save()
            c.rotate((turn + heading).toFloat(), x, y)
            val a = Path().apply {
                moveTo(x, y - 7 * density)
                lineTo(x + 5.5f * density, y + 6 * density)
                lineTo(x, y + 3 * density)
                lineTo(x - 5.5f * density, y + 6 * density)
                close()
            }
            fill.color = Color.WHITE
            c.drawPath(a, fill)
            c.restore()
        }
    }

    private fun attribution(c: Canvas, vis: Rect, density: Float) {
        val s = "© OpenStreetMap contributors"
        text.textSize = 11 * density
        text.style = Paint.Style.FILL
        val tw = text.measureText(s)
        val pad = 4 * density
        val r = RectF(vis.right - tw - 3 * pad, vis.bottom - text.textSize - 3 * pad, vis.right - pad, vis.bottom - pad)
        fill.color = 0xB3FFFFFF.toInt()
        c.drawRect(r, fill)
        text.color = 0xFF3C4043.toInt()
        c.drawText(s, r.left + pad, r.bottom - pad - 1 * density, text)
    }
}

/**
 * The car's surface: the map drawn on it, its visible area kept, the
 * driver's scrolls and scales taken. moved says when the driver moves the
 * map, or comes back (the recenter button follows).
 */
internal class CarMapSurface(private val context: Context, private val night: () -> Boolean) : SurfaceCallback {
    val state = MapState()
    val tiles = Tiles(context, loaded = { draw() })
    private val painter = MapPainter(tiles)
    var surface: SurfaceContainer? = null
        private set
    var visible = Rect()
        private set
    var moved: () -> kotlin.Unit = {}
    private val density get() = (surface?.dpi ?: 160) / 160f

    fun show(m: Map) {
        tiles.template = m.tiles
        val was = state.moved
        state.show(m)
        draw()
        if (was != state.moved) moved()
    }

    fun preview(paths: List<List<Coordinate>>, highlighted: Int) {
        state.preview(paths, highlighted)
        draw()
    }

    fun zoomBy(step: Double) {
        state.zoomBy(step)
        draw()
        moved()
    }

    fun recenter() {
        state.recenter()
        draw()
        moved()
    }

    /** Draws the map on c, as on the car's surface (a test's canvas). */
    fun drawOn(c: Canvas, w: Int, h: Int) {
        painter.draw(c, w, h, visible, density, state, state.given.night ?: night())
    }

    fun draw() {
        val sc = surface ?: return
        val s = sc.surface ?: return
        if (!s.isValid) return
        val c = try {
            s.lockCanvas(null) // as the Car App Library's samples draw: in software
        } catch (_: Exception) {
            return
        }
        try {
            drawOn(c, sc.width, sc.height)
        } finally {
            s.unlockCanvasAndPost(c)
        }
    }

    override fun onSurfaceAvailable(c: SurfaceContainer) {
        surface = c
        draw()
    }

    override fun onSurfaceDestroyed(c: SurfaceContainer) {
        if (surface === c) surface = null
    }

    override fun onVisibleAreaChanged(r: Rect) {
        visible = Rect(r)
        draw()
    }

    override fun onStableAreaChanged(r: Rect) {
        if (visible.isEmpty) visible = Rect(r)
        draw()
    }

    override fun onScroll(dx: Float, dy: Float) {
        val was = state.moved
        state.pan(dx, dy, density)
        draw()
        if (!was) moved()
    }

    override fun onFling(vx: Float, vy: Float) {
        // A fling carries the map a little further, the way it went.
        onScroll(-vx / 8, -vy / 8)
    }

    override fun onScale(focusX: Float, focusY: Float, factor: Float) {
        val sc = surface
        val vis = if (visible.isEmpty) Rect(0, 0, sc?.width ?: 0, sc?.height ?: 0) else visible
        val was = state.moved
        state.scale(focusX, focusY, factor, vis.exactCenterX(), vis.exactCenterY(), density)
        draw()
        if (!was) moved()
    }

}
