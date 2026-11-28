// platform/maps on Android: a Google Maps MapView, its lifecycle following
// its window. It needs the app's key (asuka.toml's android.mapsKey, which
// the CLI puts in the manifest); without one it says so where the map
// would be, and reports error.
//
// Markers are Google's (a pin hued by color, or an image the package
// loads: an asset, a file, a URL), plain or, with clustering, in a
// ClusterManager of android-maps-utils, both in one MarkerManager so a
// tap, a drag and an info window reach the right one; they're kept by id
// as markers changes, so a selection and a drag survive it. Polylines,
// polygons and circles are Google's shapes, ordered by zIndex.
package platform.maps

import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.os.Handler
import android.os.Looper
import android.widget.FrameLayout
import android.widget.TextView
import com.google.android.gms.maps.CameraUpdateFactory
import com.google.android.gms.maps.GoogleMap
import com.google.android.gms.maps.MapView as GoogleMapView
import com.google.android.gms.maps.model.BitmapDescriptor
import com.google.android.gms.maps.model.BitmapDescriptorFactory
import com.google.android.gms.maps.model.ButtCap
import com.google.android.gms.maps.model.CameraPosition
import com.google.android.gms.maps.model.CircleOptions
import com.google.android.gms.maps.model.Dash
import com.google.android.gms.maps.model.Gap
import com.google.android.gms.maps.model.JointType
import com.google.android.gms.maps.model.LatLng
import com.google.android.gms.maps.model.LatLngBounds
import com.google.android.gms.maps.model.Marker as GoogleMarker
import com.google.android.gms.maps.model.MarkerOptions
import com.google.android.gms.maps.model.PolygonOptions
import com.google.android.gms.maps.model.PolylineOptions
import com.google.android.gms.maps.model.RoundCap
import com.google.maps.android.clustering.Cluster
import com.google.maps.android.clustering.ClusterItem
import com.google.maps.android.clustering.ClusterManager
import com.google.maps.android.clustering.view.DefaultClusterRenderer
import com.google.maps.android.collections.MarkerManager
import dev.asuka.AsukaModules
import dev.asuka.AsukaViewRef
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.WeakHashMap
import android.view.View as AndroidView

/** The maps showing, by their views: for the Maps interface, which finds one by its Ref. */
private val shown = WeakHashMap<AndroidView, MapsView>()

/** A marker in the ClusterManager: the app's, where a drag put it. */
private class Item(var m: Marker, var index: Int) : ClusterItem {
    /** Where the user dropped it, until markers changes it. */
    var dropped: LatLng? = null
    override fun getPosition(): LatLng = dropped ?: LatLng(m.at.lat, m.at.lng)
    override fun getTitle(): String? = m.title.ifEmpty { null }
    override fun getSnippet(): String? = m.subtitle.ifEmpty { null }
    override fun getZIndex(): Float = m.zIndex.toFloat()
}

/** The marker images loaded, by source and size; the loads under way and who waits for them. */
private object Images {
    val loaded = HashMap<String, Bitmap>()
    val failed = HashSet<String>()
    private val waiting = HashMap<String, MutableList<() -> Unit>>()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    fun key(m: Marker, density: Float) = "${m.image}|${m.width}|${m.height}|$density"

    /** Loads m's image at its size (once for every marker that has it), then calls done on the main thread. */
    fun load(m: Marker, density: Float, done: () -> Unit) {
        val k = key(m, density)
        waiting[k]?.let { it.add(done); return }
        waiting[k] = mutableListOf(done)
        scope.launch {
            val bmp = runCatching {
                withContext(Dispatchers.IO) {
                    val f = AsukaModules.localFile(m.image)
                    val src = BitmapFactory.decodeFile(f.path) ?: error("not an image")
                    scaled(src, m.width, m.height, density)
                }
            }.getOrNull()
            if (bmp != null) loaded[k] = bmp else failed.add(k)
            waiting.remove(k)?.forEach { it() }
        }
    }

    /** src at width×height dp (one 0: the other keeps its shape; both: its pixels as dp). */
    private fun scaled(src: Bitmap, width: Double, height: Double, density: Float): Bitmap {
        val iw = src.width.toDouble()
        val ih = src.height.toDouble()
        var w = width
        var h = height
        when {
            w <= 0 && h <= 0 -> { w = iw; h = ih }
            w <= 0 -> w = h * iw / ih
            h <= 0 -> h = w * ih / iw
        }
        val pw = maxOf(1, Math.round(w * density).toInt())
        val ph = maxOf(1, Math.round(h * density).toInt())
        if (pw == src.width && ph == src.height) return src
        return Bitmap.createScaledBitmap(src, pw, ph, true)
    }
}

private class MapsView(private val events: ViewEvents) : View {
    private val context = events.context
    private val density = context.resources.displayMetrics.density
    private val map = GoogleMapView(context)
    private val box = FrameLayout(context)
    private val main = Handler(Looper.getMainLooper())
    /** The Maps SDK's key, which it throws without (asuka.toml's android.mapsKey). */
    private val hasKey = runCatching {
        context.packageManager.getApplicationInfo(context.packageName, PackageManager.GET_META_DATA)
            .metaData?.getString("com.google.android.geo.API_KEY").isNullOrEmpty().not()
    }.getOrDefault(false)
    override val view: AndroidView get() = box

    private var google: GoogleMap? = null
    /** What waits for the map (a Maps call before it's ready). */
    private val queued = ArrayList<(GoogleMap) -> Unit>()
    private var region = Region()
    private var camera = Camera()
    private var cameraSet = false // camera came after region: it's what to show
    private var markers: List<Marker> = emptyList()
    private var polylines: List<Polyline> = emptyList()
    private var polygons: List<Polygon> = emptyList()
    private var circles: List<Circle> = emptyList()
    private var selected = ""
    private var clustering = false
    private var clusterColor: Long? = null
    private var type = MapType.STANDARD
    private var showsUser = false
    private var interactive = true
    private var userMoved = false
    private var placed = false // the map was put where the app said once: later moves animate

    private var manager: MarkerManager? = null
    private var plain: MarkerManager.Collection? = null
    private var clusters: ClusterManager<Item>? = null
    private var renderer: Renderer? = null
    /** The plain markers by id, with what they showed. */
    private val pins = LinkedHashMap<String, Pair<GoogleMarker, Marker>>()
    /** The clustered markers by id. */
    private val items = LinkedHashMap<String, Item>()
    private val shapes = ArrayList<Any>()

    init {
        shown[box] = this
        if (hasKey) start() else {
            // No key: say so where the map would be.
            box.addView(TextView(context).apply {
                text = "Google Maps needs an API key: set asuka.toml's android.mapsKey"
                gravity = android.view.Gravity.CENTER
                setPadding(32, 32, 32, 32)
            }, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
            box.post { events.error("no Google Maps API key (asuka.toml's android.mapsKey)") }
        }
    }

    private fun start() {
        map.onCreate(null)
        box.addView(map, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
        box.addOnAttachStateChangeListener(object : AndroidView.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: AndroidView) { map.onStart(); map.onResume() }
            override fun onViewDetachedFromWindow(v: AndroidView) { map.onPause(); map.onStop() }
        })
        map.getMapAsync { g -> ready(g) }
    }

    private fun ready(g: GoogleMap) {
        google = g
        val mm = MarkerManager(g)
        manager = mm
        val p = mm.newCollection("asuka.plain")
        plain = p
        val cm = ClusterManager<Item>(context, g, mm)
        val r = Renderer(g, cm)
        r.setMinClusterSize(2)
        cm.renderer = r
        clusters = cm
        renderer = r

        g.setOnMarkerClickListener(mm)
        g.setOnInfoWindowClickListener(mm)
        g.setOnMarkerDragListener(mm)
        p.setOnMarkerClickListener { m -> pinId(m)?.let { tapped(it, m) }; true }
        p.setOnInfoWindowClickListener { m -> pinId(m)?.let { events.calloutPress(it) } }
        p.setOnMarkerDragListener(dragging { m -> pinId(m) })
        cm.markerCollection.setOnMarkerDragListener(dragging { m -> renderer?.item(m)?.m?.id })
        cm.setOnClusterItemClickListener { item -> tapped(item.m.id, renderer?.getMarker(item)); true }
        cm.setOnClusterItemInfoWindowClickListener { item -> events.calloutPress(item.m.id) }
        cm.setOnClusterClickListener { c -> clusterTapped(c); true }

        g.setOnCameraMoveStartedListener { reason -> if (reason == GoogleMap.OnCameraMoveStartedListener.REASON_GESTURE) userMoved = true }
        g.setOnCameraMoveListener { events.cameraMove(cameraOf(g), regionOf(g)) }
        g.setOnCameraIdleListener {
            cm.onCameraIdle()
            val c = cameraOf(g)
            val rg = regionOf(g)
            if (userMoved) {
                userMoved = false
                events.regionChange(rg)
                events.cameraChange(c)
            }
            events.cameraIdle(c, rg)
        }
        g.setOnMapClickListener { at ->
            if (selected.isNotEmpty()) {
                selected = ""
                events.selectedChange("")
            }
            events.press(Coordinate(lat = at.latitude, lng = at.longitude))
        }
        g.setOnMapLongClickListener { at -> events.longPress(Coordinate(lat = at.latitude, lng = at.longitude)) }
        g.setOnPolylineClickListener { l -> (l.tag as? String)?.let { events.overlayPress(it) } }
        g.setOnPolygonClickListener { s -> (s.tag as? String)?.let { events.overlayPress(it) } }
        g.setOnCircleClickListener { s -> (s.tag as? String)?.let { events.overlayPress(it) } }

        applySettings(g)
        placeMarkers(g)
        placeShapes(g)
        if (cameraSet && camera.zoom > 0) moveCamera(g, camera, animated = false) else moveTo(g, region, animated = false)
        val q = ArrayList(queued)
        queued.clear()
        for (f in q) f(g)
    }

    /** Runs f with the map now, or once it's ready. */
    private fun withMap(f: (GoogleMap) -> Unit) {
        val g = google
        if (g != null) f(g) else queued.add(f)
    }

    // ---- Props ----

    override fun setRegion(value: Region) {
        region = value
        cameraSet = false
        google?.let { moveTo(it, value, animated = placed) }
    }

    override fun setCamera(value: Camera) {
        camera = value
        if (value.zoom <= 0) return
        cameraSet = true
        google?.let { moveCamera(it, value, animated = placed) }
    }

    override fun setMarkers(value: List<Marker>) { markers = value; google?.let { placeMarkers(it) } }
    override fun setPolylines(value: List<Polyline>) { polylines = value; google?.let { placeShapes(it) } }
    override fun setPolygons(value: List<Polygon>) { polygons = value; google?.let { placeShapes(it) } }
    override fun setCircles(value: List<Circle>) { circles = value; google?.let { placeShapes(it) } }

    override fun setSelected(value: String) {
        if (value == selected) return
        selected = value
        showSelected()
    }

    override fun setClustering(value: Boolean) {
        if (value == clustering) return
        clustering = value
        google?.let { placeMarkers(it) }
    }

    override fun setClusterColor(value: Long?) {
        if (value == clusterColor) return
        clusterColor = value
        // A new renderer: the old one keeps its icons, in the old color.
        val g = google ?: return
        val cm = clusters ?: return
        val r = Renderer(g, cm)
        r.setMinClusterSize(2)
        renderer = r
        cm.renderer = r
        cm.cluster()
    }

    override fun setMapType(value: MapType) { type = value; google?.let { applySettings(it) } }
    override fun setShowsUser(value: Boolean) { showsUser = value; google?.let { applySettings(it) } }
    override fun setInteractive(value: Boolean) { interactive = value; google?.let { applySettings(it) } }

    private fun applySettings(g: GoogleMap) {
        g.mapType = when (type) { MapType.SATELLITE -> GoogleMap.MAP_TYPE_SATELLITE; MapType.HYBRID -> GoogleMap.MAP_TYPE_HYBRID; else -> GoogleMap.MAP_TYPE_NORMAL }
        g.uiSettings.setAllGesturesEnabled(interactive)
        g.uiSettings.isMapToolbarEnabled = false
        // The user's location needs the permission (platform/location's); without it, none.
        try { g.isMyLocationEnabled = showsUser } catch (_: SecurityException) {}
    }

    // ---- The camera ----

    private fun cameraOf(g: GoogleMap): Camera {
        val p = g.cameraPosition
        return Camera(center = Coordinate(lat = p.target.latitude, lng = p.target.longitude), zoom = p.zoom.toDouble(), heading = p.bearing.toDouble(), pitch = p.tilt.toDouble())
    }

    private fun regionOf(g: GoogleMap): Region {
        val b = g.projection.visibleRegion.latLngBounds
        val c = g.cameraPosition.target
        var dl = b.northeast.longitude - b.southwest.longitude
        if (dl < 0) dl += 360
        return Region(center = Coordinate(lat = c.latitude, lng = c.longitude), latDelta = b.northeast.latitude - b.southwest.latitude, lngDelta = dl)
    }

    /** Moves to the camera position, animated or not, once the map has a size. */
    private fun go(g: GoogleMap, p: CameraPosition, animated: Boolean) {
        placed = true
        val u = CameraUpdateFactory.newCameraPosition(p)
        if (animated) g.animateCamera(u) else {
            g.moveCamera(u)
            // Clustered at the zoom it's at now, not waiting for the camera's idle.
            if (items.isNotEmpty()) clusters?.cluster()
        }
    }

    /** Runs f once the map is laid out (it needs its size). */
    private fun laidOut(f: () -> Unit) {
        if (map.width > 0 && map.height > 0) f() else map.post { if (map.width > 0 && map.height > 0) f() else map.viewTreeObserver.addOnGlobalLayoutListener(object : android.view.ViewTreeObserver.OnGlobalLayoutListener {
            override fun onGlobalLayout() {
                if (map.width <= 0 || map.height <= 0) return
                map.viewTreeObserver.removeOnGlobalLayoutListener(this)
                f()
            }
        }) }
    }

    fun moveTo(g: GoogleMap, r: Region, animated: Boolean) {
        if (r.latDelta <= 0 && r.lngDelta <= 0 && r.center.lat == 0.0 && r.center.lng == 0.0) return
        val lat = r.center.lat
        val lng = r.center.lng
        // The region the user moved to, given back: already there.
        val now = g.cameraPosition.target
        if (placed && Math.abs(now.latitude - lat) < 1e-6 && Math.abs(now.longitude - lng) < 1e-6) return
        val dLat = maxOf(if (r.latDelta > 0) r.latDelta else 0.05, 0.0005) / 2
        val dLng = maxOf(if (r.lngDelta > 0) r.lngDelta else 0.05, 0.0005) / 2
        laidOut {
            val w = map.width / density.toDouble()
            val h = map.height / density.toDouble()
            // The zoom that shows the span (as newLatLngBounds would), north up and flat.
            val pts = listOf(Coordinate(lat = lat - dLat, lng = lng - dLng), Coordinate(lat = lat + dLat, lng = lng + dLng))
            val (_, zoom) = Geo.fit(pts, w, h, Insets(), 0.0, g.maxZoomLevel.toDouble()) ?: return@laidOut
            go(g, CameraPosition(LatLng(lat, lng), zoom.toFloat(), 0f, 0f), animated)
        }
    }

    fun moveCamera(g: GoogleMap, c: Camera, animated: Boolean) {
        if (c.zoom <= 0) return
        val now = g.cameraPosition
        if (placed && Math.abs(now.target.latitude - c.center.lat) < 1e-6 && Math.abs(now.target.longitude - c.center.lng) < 1e-6 &&
            Math.abs(now.zoom - c.zoom) < 1e-3 && Math.abs(now.bearing - c.heading) < 1e-3 && Math.abs(now.tilt - c.pitch) < 1e-3) return
        val bearing = ((c.heading % 360) + 360) % 360
        val tilt = c.pitch.coerceIn(0.0, 90.0)
        laidOut { go(g, CameraPosition(LatLng(c.center.lat, c.center.lng), c.zoom.toFloat(), tilt.toFloat(), bearing.toFloat()), animated) }
    }

    fun fit(g: GoogleMap, points: List<Coordinate>, padding: Insets, animated: Boolean) {
        if (points.isEmpty()) return
        laidOut {
            val heading = g.cameraPosition.bearing.toDouble()
            val (center, zoom) = Geo.fit(points, map.width / density.toDouble(), map.height / density.toDouble(), padding, heading, g.maxZoomLevel.toDouble()) ?: return@laidOut
            go(g, CameraPosition(LatLng(center.lat, center.lng), zoom.toFloat(), 0f, heading.toFloat()), animated)
        }
    }

    // The Maps interface's calls, with the map once it's ready.
    fun callMoveTo(r: Region, animated: Boolean) = withMap { placed = true; moveTo(it, r, animated) }
    fun callMoveCamera(c: Camera, animated: Boolean) = withMap { placed = true; moveCamera(it, c, animated) }
    fun callFit(points: List<Coordinate>, padding: Insets, animated: Boolean) = withMap { fit(it, points, padding, animated) }
    fun currentCamera(): Camera = google?.let { cameraOf(it) } ?: Camera()

    // ---- Markers ----

    private fun pinId(m: GoogleMarker): String? = m.tag as? String

    private fun tapped(id: String, m: GoogleMarker?) {
        events.markerPress(id)
        if (selected != id) {
            selected = id
            events.selectedChange(id)
        }
        // The info window, without moving the map to the marker (Google's own tap does).
        if (m != null && (m.title != null || m.snippet != null)) m.showInfoWindow()
    }

    private fun clusterTapped(c: Cluster<Item>) {
        val members = c.items.sortedBy { it.index }
        events.clusterPress(members.map { it.m.id })
        val g = google ?: return
        val pts = members.map { Coordinate(lat = it.position.latitude, lng = it.position.longitude) }
        fit(g, pts, Insets(top = 48.0, left = 48.0, bottom = 48.0, right = 48.0), animated = true)
    }

    private fun dragging(id: (GoogleMarker) -> String?) = object : GoogleMap.OnMarkerDragListener {
        override fun onMarkerDragStart(m: GoogleMarker) { id(m)?.let { events.markerDragStart(it, at(m)) } }
        override fun onMarkerDrag(m: GoogleMarker) { id(m)?.let { events.markerDrag(it, at(m)) } }
        override fun onMarkerDragEnd(m: GoogleMarker) {
            val i = id(m) ?: return
            // Where it was dropped, until markers moves it.
            items[i]?.dropped = m.position
            events.markerDragEnd(i, at(m))
        }
        private fun at(m: GoogleMarker) = Coordinate(lat = m.position.latitude, lng = m.position.longitude)
    }

    /** The selected marker's info window, shown; the others' hidden. */
    private fun showSelected() {
        for ((_, p) in pins) if (p.second.id != selected && p.first.isInfoWindowShown) p.first.hideInfoWindow()
        renderer?.shown()?.forEach { (item, m) -> if (item.m.id != selected && m.isInfoWindowShown) m.hideInfoWindow() }
        if (selected.isEmpty()) return
        val m = pins[selected]?.first ?: items[selected]?.let { renderer?.getMarker(it) } ?: return
        if (m.title != null || m.snippet != null) m.showInfoWindow()
    }

    /** The icon a marker shows now: its image (null while it loads: hidden), or the pin hued by its color. */
    private fun icon(m: Marker, then: () -> Unit): BitmapDescriptor? {
        if (m.image.isEmpty()) {
            val c = m.color ?: return BitmapDescriptorFactory.defaultMarker()
            val hsv = FloatArray(3)
            Color.colorToHSV(AsukaModules.color(c), hsv)
            return BitmapDescriptorFactory.defaultMarker(hsv[0])
        }
        val k = Images.key(m, density)
        Images.loaded[k]?.let { return BitmapDescriptorFactory.fromBitmap(it) }
        if (k in Images.failed) return BitmapDescriptorFactory.defaultMarker()
        Images.load(m, density, then)
        return null
    }

    /** Sets a marker's options from m. */
    private fun options(o: MarkerOptions, m: Marker, at: LatLng, then: () -> Unit) {
        o.position(at).title(m.title.ifEmpty { null }).snippet(m.subtitle.ifEmpty { null }).draggable(m.draggable).zIndex(m.zIndex.toFloat())
        val ic = icon(m, then)
        if (ic != null) o.icon(ic).visible(true) else o.visible(false)
        if (m.image.isNotEmpty()) o.anchor(m.anchor.x.toFloat(), m.anchor.y.toFloat()) else o.anchor(0.5f, 1f)
    }

    /** Updates a Google marker to show m. */
    private fun update(g: GoogleMarker, old: Marker?, m: Marker, at: LatLng, then: () -> Unit) {
        if (old == null || old.at != m.at) g.position = at
        if (old == null || old.title != m.title) g.title = m.title.ifEmpty { null }
        if (old == null || old.subtitle != m.subtitle) g.snippet = m.subtitle.ifEmpty { null }
        g.isDraggable = m.draggable
        g.zIndex = m.zIndex.toFloat()
        if (old == null || old.image != m.image || old.color != m.color || old.width != m.width || old.height != m.height) {
            val ic = icon(m, then)
            if (ic != null) { g.setIcon(ic); g.isVisible = true } else g.isVisible = false
        }
        if (m.image.isNotEmpty()) g.setAnchor(m.anchor.x.toFloat(), m.anchor.y.toFloat()) else g.setAnchor(0.5f, 1f)
        if (g.isInfoWindowShown && old != null && (old.title != m.title || old.subtitle != m.subtitle)) g.showInfoWindow()
    }

    private fun placeMarkers(g: GoogleMap) {
        val p = plain ?: return
        val cm = clusters ?: return
        val r = renderer ?: return
        val want = LinkedHashMap<String, Pair<Marker, Int>>()
        markers.forEachIndexed { i, m -> want[m.id] = m to i }

        // Plain markers: kept by id, updated, added, removed.
        val plainIds = want.filter { (_, v) -> !(clustering && v.first.clusters) }
        for (id in pins.keys.toList()) if (id !in plainIds) pins.remove(id)?.let { p.remove(it.first) }
        for ((id, v) in plainIds) {
            val m = v.first
            val at = LatLng(m.at.lat, m.at.lng)
            val have = pins[id]
            if (have == null) {
                val o = MarkerOptions()
                options(o, m, at) { pins[id]?.let { (gm, cur) -> if (cur.image == m.image) update(gm, null, cur, gm.position, {}) } }
                val gm = p.addMarker(o)
                gm.tag = id
                pins[id] = gm to m
            } else if (have.second != m) {
                update(have.first, have.second, m, at) { pins[id]?.let { (gm, cur) -> update(gm, null, cur, gm.position, {}) } }
                pins[id] = have.first to m
            }
        }

        // Clustered ones: the ClusterManager's items, reclustered.
        val clusterIds = want.filter { (_, v) -> clustering && v.first.clusters }
        var changed = false
        for (id in items.keys.toList()) if (id !in clusterIds) { items.remove(id)?.let { cm.removeItem(it) }; changed = true }
        for ((id, v) in clusterIds) {
            val (m, i) = v
            val have = items[id]
            if (have == null) {
                val item = Item(m, i)
                items[id] = item
                cm.addItem(item)
                changed = true
            } else if (have.m != m || have.index != i) {
                val moved = have.m.at != m.at
                have.m = m
                have.index = i
                if (moved) have.dropped = null
                cm.updateItem(have)
                r.getMarker(have)?.let { gm -> update(gm, null, m, have.position) { r.refresh(have) } }
                changed = true
            }
        }
        if (changed) cm.cluster()
        showSelected()
    }

    /** The renderer of the clustered markers: their options as plain ones', the clusters in clusterColor. */
    private inner class Renderer(g: GoogleMap, cm: ClusterManager<Item>) : DefaultClusterRenderer<Item>(context, g, cm) {
        fun item(m: GoogleMarker): Item? = getClusterItem(m)

        fun shown(): List<Pair<Item, GoogleMarker>> = items.values.mapNotNull { i -> getMarker(i)?.let { i to it } }

        /** Shows an item's image once it's loaded. */
        fun refresh(item: Item) { getMarker(item)?.let { update(it, null, item.m, item.position) {} } }

        override fun onBeforeClusterItemRendered(item: Item, o: MarkerOptions) {
            options(o, item.m, item.position) { refresh(item) }
        }

        override fun onClusterItemUpdated(item: Item, m: GoogleMarker) {
            update(m, null, item.m, item.position) { refresh(item) }
        }

        override fun onClusterItemRendered(item: Item, m: GoogleMarker) {
            if (item.m.id == selected && (m.title != null || m.snippet != null)) m.showInfoWindow()
        }

        override fun getColor(clusterSize: Int): Int = clusterColor?.let { AsukaModules.color(it, super.getColor(clusterSize)) } ?: super.getColor(clusterSize)
    }

    // ---- Overlays ----

    private fun placeShapes(g: GoogleMap) {
        for (s in shapes) when (s) {
            is com.google.android.gms.maps.model.Polyline -> s.remove()
            is com.google.android.gms.maps.model.Polygon -> s.remove()
            is com.google.android.gms.maps.model.Circle -> s.remove()
        }
        shapes.clear()
        // Their order: by zIndex, then polygons, circles and lines, each in its list's order.
        data class Shape(val z: Double, val kind: Int, val index: Int, val add: (Float) -> Any)
        val all = ArrayList<Shape>()
        polygons.forEachIndexed { i, p -> all.add(Shape(p.zIndex, 0, i) { z -> addPolygon(g, p, z) }) }
        circles.forEachIndexed { i, c -> all.add(Shape(c.zIndex, 1, i) { z -> addCircle(g, c, z) }) }
        polylines.forEachIndexed { i, l -> all.add(Shape(l.zIndex, 2, i) { z -> addPolyline(g, l, z) }) }
        all.sortWith(compareBy<Shape>({ it.z }, { it.kind }, { it.index }))
        all.forEachIndexed { rank, s -> shapes.add(s.add(rank.toFloat())) }
    }

    private fun latLng(c: Coordinate) = LatLng(c.lat, c.lng)

    private fun addPolyline(g: GoogleMap, l: Polyline, z: Float): Any {
        val w = (l.width * density).toFloat()
        val o = PolylineOptions().addAll(l.points.map(::latLng)).color(AsukaModules.color(l.color, Color.BLUE)).width(w)
            .geodesic(l.geodesic).zIndex(z).clickable(l.id.isNotEmpty()).jointType(JointType.ROUND)
        if (l.dashed) o.pattern(listOf(Dash(4 * w), Gap(2 * w))).startCap(ButtCap()).endCap(ButtCap())
        else o.startCap(RoundCap()).endCap(RoundCap())
        return g.addPolyline(o).also { it.tag = l.id.ifEmpty { null } }
    }

    private fun addPolygon(g: GoogleMap, p: Polygon, z: Float): Any {
        val o = PolygonOptions().addAll(p.points.map(::latLng)).fillColor(AsukaModules.color(p.fill)).zIndex(z).clickable(p.id.isNotEmpty())
        if (p.strokeWidth > 0) o.strokeColor(AsukaModules.color(p.stroke)).strokeWidth((p.strokeWidth * density).toFloat()) else o.strokeWidth(0f)
        for (h in p.holes) if (h.size >= 3) o.addHole(h.map(::latLng))
        return g.addPolygon(o).also { it.tag = p.id.ifEmpty { null } }
    }

    private fun addCircle(g: GoogleMap, c: Circle, z: Float): Any {
        val o = CircleOptions().center(latLng(c.center)).radius(maxOf(c.radius, 0.0)).fillColor(AsukaModules.color(c.fill)).zIndex(z).clickable(c.id.isNotEmpty())
        if (c.strokeWidth > 0) o.strokeColor(AsukaModules.color(c.stroke)).strokeWidth((c.strokeWidth * density).toFloat()) else o.strokeWidth(0f)
        return g.addCircle(o).also { it.tag = c.id.ifEmpty { null } }
    }

    companion object {
        /** The map a view is, or holds. */
        fun of(v: AndroidView?): MapsView? {
            if (v == null) return null
            shown[v]?.let { return it }
            if (v is android.view.ViewGroup) for (i in 0 until v.childCount) of(v.getChildAt(i))?.let { return it }
            return null
        }
    }
}

private class GoogleMaps : Maps {
    override fun moveTo(view: AsukaViewRef, region: Region, animated: Boolean) { MapsView.of(view.view)?.callMoveTo(region, animated) }
    override fun moveCamera(view: AsukaViewRef, camera: Camera, animated: Boolean) { MapsView.of(view.view)?.callMoveCamera(camera, animated) }
    override fun fit(view: AsukaViewRef, coordinates: List<Coordinate>, padding: Insets, animated: Boolean) { MapsView.of(view.view)?.callFit(coordinates, padding, animated) }
    override fun camera(view: AsukaViewRef): Camera = MapsView.of(view.view)?.currentCamera() ?: Camera()
}

fun makeMaps(): Maps = GoogleMaps()

fun makeView(events: ViewEvents): View = MapsView(events)
