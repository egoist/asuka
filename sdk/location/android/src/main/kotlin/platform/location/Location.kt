// platform/location on Android: the location manager (no Play services):
// positions on screen from its fused (or GPS, or network) provider; the
// background updates and the regions (proximity alerts) delivered to
// PendingIntents for LocationReceiver, so they come with the app not
// running (the receiver starts the core, headless); with a notification,
// LocationService keeps the updates at their rate out of sight. Events
// are kept in a file until the app says each is done.
package platform.location

import android.Manifest
import android.annotation.SuppressLint
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.location.Geocoder
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.location.LocationRequest
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.annotation.RequiresApi
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.core.location.LocationManagerCompat
import dev.asuka.AsukaModules
import dev.asuka.AsukaRuntime
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.Locale
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.time.Duration.Companion.milliseconds

private val main = Handler(Looper.getMainLooper())

private fun granted(ctx: Context, p: String) = ctx.checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED
private fun foreground(ctx: Context) = granted(ctx, Manifest.permission.ACCESS_FINE_LOCATION) || granted(ctx, Manifest.permission.ACCESS_COARSE_LOCATION)
// Before Android 10 background location comes with the foreground permission.
private fun background(ctx: Context) = foreground(ctx) &&
    (Build.VERSION.SDK_INT < 29 || granted(ctx, Manifest.permission.ACCESS_BACKGROUND_LOCATION))

private fun lm(ctx: Context) = ctx.getSystemService(Context.LOCATION_SERVICE) as LocationManager

fun position(l: Location) = Fix(
    latitude = l.latitude, longitude = l.longitude, accuracy = l.accuracy.toDouble(),
    altitude = if (l.hasAltitude()) l.altitude else 0.0,
    speed = if (l.hasSpeed()) l.speed.toDouble() else -1.0,
    course = if (l.hasBearing()) l.bearing.toDouble() else -1.0,
    time = l.time * 1_000_000,
)

private fun json(p: Fix) = JSONObject().put("lat", p.latitude).put("lon", p.longitude).put("acc", p.accuracy)
    .put("alt", p.altitude).put("spd", p.speed).put("crs", p.course).put("t", p.time)

private fun unjson(o: JSONObject?) = if (o == null) Fix() else Fix(
    latitude = o.optDouble("lat"), longitude = o.optDouble("lon"), accuracy = o.optDouble("acc"), altitude = o.optDouble("alt"),
    speed = o.optDouble("spd", -1.0), course = o.optDouble("crs", -1.0), time = o.optLong("t"),
)

@SuppressLint("MissingPermission") // checked first
class AndroidLocator(private val ctx: Context) : Locator {
    override fun authorization(): Authorization {
        val level = when {
            background(ctx) -> Level.ALWAYS
            foreground(ctx) -> Level.WHEN_IN_USE
            Store.asked(ctx) -> Level.DENIED
            else -> Level.NOT_ASKED
        }
        return Authorization(level = level, precise = granted(ctx, Manifest.permission.ACCESS_FINE_LOCATION), off = !LocationManagerCompat.isLocationEnabled(lm(ctx)))
    }

    override suspend fun watchAuthorization(changed: (Authorization) -> Unit) {
        // Android says nothing as a permission changes in Settings: asked
        // again as the app comes back, and every few seconds while it runs.
        var last: Authorization? = null
        while (true) {
            val a = authorization()
            if (a != last) {
                last = a
                withContext(Dispatchers.Main) { changed(a) }
            }
            kotlinx.coroutines.delay(2000)
        }
    }

    override suspend fun request(always: Boolean): Authorization {
        if (!foreground(ctx)) {
            Store.setAsked(ctx)
            ask(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION))
        }
        if (always && foreground(ctx) && !background(ctx)) {
            // Android 11 and later take the user to the app's settings
            // (before Android 10 there's nothing more to ask: background()).
            ask(arrayOf(Manifest.permission.ACCESS_BACKGROUND_LOCATION))
        }
        return authorization()
    }

    override suspend fun requestPrecise(purpose: String): Authorization {
        if (foreground(ctx) && !granted(ctx, Manifest.permission.ACCESS_FINE_LOCATION)) {
            ask(arrayOf(Manifest.permission.ACCESS_FINE_LOCATION, Manifest.permission.ACCESS_COARSE_LOCATION))
        }
        return authorization()
    }

    private fun provider(): String {
        val m = lm(ctx)
        // Before Android 12 there's no fused provider: GPS when it's on and
        // allowed, the network's otherwise.
        return when {
            Build.VERSION.SDK_INT >= 31 && m.hasProvider(LocationManager.FUSED_PROVIDER) -> LocationManager.FUSED_PROVIDER
            granted(ctx, Manifest.permission.ACCESS_FINE_LOCATION) && m.isProviderEnabled(LocationManager.GPS_PROVIDER) -> LocationManager.GPS_PROVIDER
            else -> LocationManager.NETWORK_PROVIDER
        }
    }

    override suspend fun current(): Fix {
        if (!foreground(ctx)) request(false)
        if (!foreground(ctx)) throw LocationException(LocationError.DENIED)
        return withContext(Dispatchers.Main) {
            suspendCancellableCoroutine { c ->
                val cancel = android.os.CancellationSignal()
                c.invokeOnCancellation { cancel.cancel() }
                // Before Android 11, the first update the provider gives (or none in 30 seconds).
                LocationManagerCompat.getCurrentLocation(lm(ctx), provider(), cancel, ContextCompat.getMainExecutor(ctx)) { l ->
                    if (l != null) c.resume(position(l)) else c.resumeWithException(LocationException(LocationError.UNAVAILABLE))
                }
            }
        }
    }

    override suspend fun watch(moved: (Fix) -> Unit) {
        if (!foreground(ctx)) request(false)
        if (!foreground(ctx)) throw LocationException(LocationError.DENIED)
        val l = LocationListener { moved(position(it)) }
        withContext(Dispatchers.Main) {
            lm(ctx).getLastKnownLocation(provider())?.let { moved(position(it)) }
            lm(ctx).requestLocationUpdates(provider(), 1000L, 0f, l, Looper.getMainLooper())
        }
        try {
            awaitCancellation()
        } finally {
            main.post { lm(ctx).removeUpdates(l) }
        }
    }

    override suspend fun placeName(latitude: Double, longitude: Double): String {
        if (!Geocoder.isPresent()) throw LocationException(LocationError.UNAVAILABLE)
        val g = Geocoder(ctx, Locale.getDefault())
        val list = if (Build.VERSION.SDK_INT >= 33) geocode(g, latitude, longitude) else withContext(Dispatchers.IO) {
            // Before Android 13 the geocoder answers only as it blocks: on a worker.
            @Suppress("DEPRECATION")
            try { g.getFromLocation(latitude, longitude, 1) ?: emptyList() } catch (e: java.io.IOException) { throw Exception(e.message ?: "location: no name for this place") }
        }
        val a = list.firstOrNull()
        val parts = listOfNotNull(a?.subLocality ?: a?.featureName, a?.locality ?: a?.adminArea ?: a?.countryName).distinct()
        if (parts.isEmpty()) throw Exception("location: no name for this place")
        return parts.joinToString(", ")
    }

    @RequiresApi(33)
    private suspend fun geocode(g: Geocoder, latitude: Double, longitude: Double): List<android.location.Address> = suspendCancellableCoroutine { c ->
        g.getFromLocation(latitude, longitude, 1, object : Geocoder.GeocodeListener {
            override fun onGeocode(list: MutableList<android.location.Address>) { c.resume(list) }
            override fun onError(message: String?) { c.resumeWithException(Exception(message ?: "location: no name for this place")) }
        })
    }

    override fun openSettings() {
        val i = Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.fromParts("package", ctx.packageName, null)).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        (AsukaModules.activity ?: ctx).startActivity(i)
    }

    override fun support() = Support(updates = true, regions = true, maxRegions = 0, maxRadius = 0.0, dwell = false, relaunch = true)

    override suspend fun startUpdates(options: Updates) {
        request(false)
        if (!foreground(ctx)) throw LocationException(LocationError.DENIED)
        withContext(Dispatchers.Main) {
            Store.setUpdates(ctx, options)
            Background.requestUpdates(ctx, options)
            if (options.notification.title.isNotEmpty()) {
                // Its notification shows (the system asks first); the updates go on either way.
                if (Build.VERSION.SDK_INT >= 33) ask(arrayOf(Manifest.permission.POST_NOTIFICATIONS))
                Background.startService(ctx)
            } else {
                LocationService.running?.stop()
            }
        }
    }

    override fun stopUpdates() {
        main.post {
            Store.setUpdates(ctx, null)
            runCatching { lm(ctx).removeUpdates(Background.updatesIntent(ctx)) }
            LocationService.running?.stop()
        }
    }

    override suspend fun updating() = Store.updates(ctx) != null

    override suspend fun monitor(region: Region) {
        if (!background(ctx)) throw LocationException(LocationError.NEEDS_ALWAYS)
        withContext(Dispatchers.Main) {
            val rs = Store.regions(ctx).filter { it.id != region.id }
            Store.setInside(ctx, Store.inside(ctx).apply { remove(region.id) })
            Store.setRegions(ctx, rs + region)
            lm(ctx).addProximityAlert(region.latitude, region.longitude, region.radius.toFloat(), -1, Background.regionIntent(ctx, region.id))
        }
    }

    override suspend fun stopMonitoring(id: String) = withContext(Dispatchers.Main) {
        runCatching { lm(ctx).removeProximityAlert(Background.regionIntent(ctx, id)) }
        Store.setRegions(ctx, Store.regions(ctx).filter { it.id != id })
        Store.setInside(ctx, Store.inside(ctx).apply { remove(id) })
    }

    override suspend fun stopMonitoringAll() = withContext(Dispatchers.Main) {
        for (r in Store.regions(ctx)) runCatching { lm(ctx).removeProximityAlert(Background.regionIntent(ctx, r.id)) }
        Store.setRegions(ctx, emptyList())
        Store.setInside(ctx, JSONObject())
    }

    override suspend fun monitored(): List<Region> = Store.regions(ctx)

    override suspend fun events(event: (Event) -> Unit) {
        val id = withContext(Dispatchers.Main) { Events.listen(ctx, event) }
        try {
            awaitCancellation()
        } finally {
            main.post { Events.unlisten(id) }
        }
    }

    override fun hold(id: Long) = Events.hold(id)

    override fun done(id: Long) { main.post { Events.done(ctx, id) } }

    /** Asks for permissions not granted yet. */
    private suspend fun ask(perms: Array<String>) = withContext(Dispatchers.Main) {
        if (perms.all { granted(ctx, it) }) return@withContext
        val act = AsukaModules.activity as? ComponentActivity ?: return@withContext
        suspendCancellableCoroutine { c ->
            var launcher: ActivityResultLauncher<Array<String>>? = null
            launcher = act.activityResultRegistry.register("asuka-location-${asks++}", ActivityResultContracts.RequestMultiplePermissions()) {
                launcher?.unregister()
                if (c.isActive) c.resume(Unit)
            }
            launcher.launch(perms)
        }
    }

    private companion object {
        var asks = 0
    }
}

/** What runs, kept across launches (SharedPreferences). */
object Store {
    private fun prefs(ctx: Context) = ctx.getSharedPreferences("asuka.location", Context.MODE_PRIVATE)

    fun asked(ctx: Context) = prefs(ctx).getBoolean("asked", false)
    fun setAsked(ctx: Context) = prefs(ctx).edit().putBoolean("asked", true).apply()

    fun updates(ctx: Context): JSONObject? = prefs(ctx).getString("updates", null)?.let { runCatching { JSONObject(it) }.getOrNull() }

    fun setUpdates(ctx: Context, u: Updates?) {
        val e = prefs(ctx).edit()
        if (u == null) e.remove("updates")
        else e.putString("updates", JSONObject().put("accuracy", u.accuracy.value).put("distance", u.distance)
            .put("interval", u.interval.inWholeMilliseconds).put("delay", u.delay.inWholeMilliseconds).put("significant", u.significant)
            .put("title", u.notification.title).put("text", u.notification.text).toString())
        e.apply()
    }

    fun regions(ctx: Context): List<Region> = runCatching {
        val a = JSONArray(prefs(ctx).getString("regions", "[]"))
        (0 until a.length()).map { i ->
            val o = a.getJSONObject(i)
            val ev = o.optJSONArray("events")
            Region(id = o.getString("id"), latitude = o.getDouble("lat"), longitude = o.getDouble("lon"), radius = o.getDouble("radius"),
                events = if (ev == null) listOf(Transition.ENTER, Transition.EXIT) else (0 until ev.length()).map { Transition(ev.getLong(it)) },
                dwell = o.optLong("dwell", 300_000).milliseconds)
        }
    }.getOrDefault(emptyList())

    fun setRegions(ctx: Context, rs: List<Region>) = prefs(ctx).edit().putString("regions", JSONArray(rs.map { r ->
        JSONObject().put("id", r.id).put("lat", r.latitude).put("lon", r.longitude).put("radius", r.radius)
            .put("events", JSONArray(r.events.map { it.value })).put("dwell", r.dwell.inWholeMilliseconds)
    }).toString()).apply()

    fun inside(ctx: Context): JSONObject = runCatching { JSONObject(prefs(ctx).getString("inside", "{}")!!) }.getOrDefault(JSONObject())
    fun setInside(ctx: Context, o: JSONObject) = prefs(ctx).edit().putString("inside", o.toString()).apply()
}

@SuppressLint("MissingPermission")
object Background {
    const val UPDATES = "asuka.location.UPDATES"
    const val REGIONS = "asuka.location.REGIONS"
    private const val FLAGS = PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_MUTABLE

    fun updatesIntent(ctx: Context): PendingIntent =
        PendingIntent.getBroadcast(ctx, 1, Intent(ctx, LocationReceiver::class.java).setAction(UPDATES), FLAGS)

    fun regionIntent(ctx: Context, id: String): PendingIntent =
        PendingIntent.getBroadcast(ctx, 3, Intent(ctx, LocationReceiver::class.java).setAction(REGIONS).setData(Uri.fromParts("asuka-region", id, null)), FLAGS)

    fun requestUpdates(ctx: Context, u: Updates) {
        val m = lm(ctx)
        val accuracy = if (u.significant) 2L else u.accuracy.value
        val interval = u.interval.inWholeMilliseconds.takeIf { it > 0 } ?: if (u.significant) 5 * 60_000L else 10_000L
        // Before Android 12 there's no fused provider: GPS for the best
        // accuracy (when it's allowed), the network's for the rest.
        val provider = when {
            accuracy == 4L -> LocationManager.PASSIVE_PROVIDER
            Build.VERSION.SDK_INT >= 31 && m.hasProvider(LocationManager.FUSED_PROVIDER) -> LocationManager.FUSED_PROVIDER
            accuracy <= 1 && granted(ctx, Manifest.permission.ACCESS_FINE_LOCATION) && LocationManagerCompat.hasProvider(m, LocationManager.GPS_PROVIDER) -> LocationManager.GPS_PROVIDER
            else -> LocationManager.NETWORK_PROVIDER
        }
        val distance = if (u.significant) maxOf(u.distance, 500.0) else u.distance
        if (Build.VERSION.SDK_INT >= 31) {
            val quality = when (accuracy) {
                0L, 1L -> LocationRequest.QUALITY_HIGH_ACCURACY
                2L -> LocationRequest.QUALITY_BALANCED_POWER_ACCURACY
                else -> LocationRequest.QUALITY_LOW_POWER
            }
            val req = LocationRequest.Builder(interval).setQuality(quality).setMinUpdateDistanceMeters(distance.toFloat())
                .setMaxUpdateDelayMillis(u.delay.inWholeMilliseconds).build()
            m.requestLocationUpdates(provider, req, updatesIntent(ctx))
        } else {
            // Before Android 12 the provider stands for the accuracy, and
            // each update comes as it's found (none held back to batch).
            m.requestLocationUpdates(provider, interval, distance.toFloat(), updatesIntent(ctx))
        }
    }

    fun startService(ctx: Context) {
        runCatching { ContextCompat.startForegroundService(ctx, Intent(ctx, LocationService::class.java)) }
    }

    /** Asks again for what ran, after a reboot or an update (which clear it). */
    fun restore(ctx: Context) {
        val u = Store.updates(ctx)
        if (u != null && foreground(ctx)) {
            runCatching {
                requestUpdates(ctx, Updates(accuracy = Accuracy(u.optLong("accuracy")), distance = u.optDouble("distance"),
                    interval = u.optLong("interval").milliseconds, delay = u.optLong("delay").milliseconds, significant = u.optBoolean("significant")))
            }
            if (u.optString("title").isNotEmpty()) startService(ctx)
        }
        if (background(ctx)) {
            for (r in Store.regions(ctx)) {
                runCatching { lm(ctx).addProximityAlert(r.latitude, r.longitude, r.radius.toFloat(), -1, regionIntent(ctx, r.id)) }
            }
        }
    }
}

/**
 * The events, kept in a file until the app says each is done: one that
 * came as the process ended is handed over at the next start. A
 * broadcast's (goAsync) is held while its events wait, or for 9 seconds.
 * Main thread.
 */
object Events {
    private var file: File? = null
    private val events = ArrayList<JSONObject>()
    private val readers = HashMap<Int, (Event) -> Unit>()
    private var nextReader = 0
    private val holds = ArrayList<BroadcastReceiver.PendingResult>()

    private fun load(ctx: Context) {
        if (file != null) return
        val f = File(ctx.filesDir, "asuka-location-events.json")
        file = f
        runCatching { val a = JSONArray(f.readText()); for (i in 0 until a.length()) events.add(a.getJSONObject(i)) }
    }

    private fun save() {
        val f = file ?: return
        val tmp = File(f.path + ".tmp")
        tmp.writeText(JSONArray(events).toString())
        tmp.renameTo(f)
    }

    private fun event(o: JSONObject): Event {
        val ps = o.optJSONArray("positions")
        return Event(id = o.getLong("id"), positions = if (ps == null) emptyList() else (0 until ps.length()).map { unjson(ps.getJSONObject(it)) },
            crossing = o.optBoolean("crossing"), region = o.optString("region"), kind = Transition(o.optLong("kind")),
            at = unjson(o.optJSONObject("at")), time = o.optLong("time"))
    }

    fun listen(ctx: Context, f: (Event) -> Unit): Int {
        load(ctx)
        readers[++nextReader] = f
        for (e in events.toList()) f(event(e))
        return nextReader
    }

    fun unlisten(id: Int) { readers.remove(id) }

    fun received(ctx: Context, positions: List<Fix>, crossings: List<Pair<String, Transition>>, hold: BroadcastReceiver.PendingResult?) {
        load(ctx)
        val out = ArrayList<JSONObject>()
        if (positions.isNotEmpty() && Store.updates(ctx) != null) out.add(JSONObject().put("positions", JSONArray(positions.map { json(it) })))
        for ((id, kind) in crossings) crossing(ctx, id, kind)?.let { out.add(it) }
        if (out.isEmpty()) { hold?.finish(); return }
        val prefs = ctx.getSharedPreferences("asuka.location", Context.MODE_PRIVATE)
        for (e in out) {
            val id = prefs.getLong("nextEvent", 0) + 1
            prefs.edit().putLong("nextEvent", id).apply()
            events.add(e.put("id", id))
        }
        save()
        if (hold != null) {
            holds.add(hold)
            main.postDelayed({ if (holds.remove(hold)) hold.finish() }, 9000L)
        }
        // The app's Asuka, started without a screen if it isn't running.
        AsukaRuntime.get(ctx).startHeadless()
        for (e in out) { val ev = event(e); for (r in readers.values.toList()) r(ev) }
    }

    /** A crossing as an event, if the region reports it: from not known to outside is none (just added, the device out of it). */
    private fun crossing(ctx: Context, id: String, kind: Transition): JSONObject? {
        val r = Store.regions(ctx).firstOrNull { it.id == id } ?: return null
        val inside = Store.inside(ctx)
        val was = if (inside.has(id)) inside.getBoolean(id) else null
        Store.setInside(ctx, inside.put(id, kind == Transition.ENTER))
        if (was == null && kind == Transition.EXIT || was == (kind == Transition.ENTER)) return null
        if (kind !in r.events) return null
        val e = JSONObject().put("crossing", true).put("region", id).put("kind", kind.value).put("time", System.currentTimeMillis() * 1_000_000)
        lastKnown(ctx)?.let { e.put("at", json(position(it))) }
        return e
    }

    /** The fused provider's last position; before Android 12, the newer of GPS's and the network's. */
    @SuppressLint("MissingPermission") // a region's crossing comes with the permission granted (runCatching: revoked since)
    private fun lastKnown(ctx: Context): Location? {
        val ps = if (Build.VERSION.SDK_INT >= 31) listOf(LocationManager.FUSED_PROVIDER) else listOf(LocationManager.GPS_PROVIDER, LocationManager.NETWORK_PROVIDER)
        return ps.mapNotNull { runCatching { lm(ctx).getLastKnownLocation(it) }.getOrNull() }.maxByOrNull { it.time }
    }

    fun hold(@Suppress("UNUSED_PARAMETER") id: Long) {
        // The broadcast that brought it is held already (received).
    }

    fun done(ctx: Context, id: Long) {
        load(ctx)
        events.removeAll { it.optLong("id") == id }
        save()
        if (events.isEmpty()) {
            val hs = ArrayList(holds)
            holds.clear()
            for (h in hs) h.finish()
        }
    }
}

/**
 * The updates' and the regions' PendingIntents (with the app not running,
 * they start its process), and a reboot or an update, after which what
 * ran is asked for again.
 */
class LocationReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED -> main.post { Background.restore(ctx) }
            else -> {
                val x = intent.extras ?: return
                val hold = goAsync()
                if (x.containsKey(LocationManager.KEY_PROXIMITY_ENTERING)) {
                    val id = intent.data?.schemeSpecificPart ?: run { hold.finish(); return }
                    val kind = if (x.getBoolean(LocationManager.KEY_PROXIMITY_ENTERING)) Transition.ENTER else Transition.EXIT
                    main.post { Events.received(ctx, emptyList(), listOf(id to kind), hold) }
                    return
                }
                // Before Android 12 a broadcast brings one position.
                @Suppress("DEPRECATION")
                val ls: List<Location> = (if (Build.VERSION.SDK_INT >= 31) x.getParcelableArrayList<Location>(LocationManager.KEY_LOCATIONS) else null)
                    ?: listOfNotNull(x.getParcelable<Location>(LocationManager.KEY_LOCATION_CHANGED))
                main.post { Events.received(ctx, ls.map { position(it) }, emptyList(), hold) }
            }
        }
    }
}

/**
 * The foreground service of the location type that keeps the background
 * updates at their rate with the app out of sight, its notification the
 * updates' title and text (tapping it opens the app).
 */
class LocationService : Service() {
    companion object {
        const val CHANNEL = "asuka.location"
        const val ID = 0x6c6f_6361
        var running: LocationService? = null
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        running = this
        if (!show()) { stop(); return START_NOT_STICKY }
        return START_STICKY
    }

    private fun show(): Boolean {
        val u = Store.updates(this) ?: return false
        if (u.optString("title").isEmpty()) return false
        val nm = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26 && nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Location", NotificationManager.IMPORTANCE_LOW))
        }
        val open = packageManager.getLaunchIntentForPackage(packageName)
        // Before Android 8 there are no channels: the notification's own priority, low as the channel's importance.
        val b = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(applicationInfo.icon)
            .setContentTitle(u.optString("title"))
            .setOngoing(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .setCategory(NotificationCompat.CATEGORY_NAVIGATION)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
        u.optString("text").takeIf { it.isNotEmpty() }?.let { b.setContentText(it) }
        if (open != null) b.setContentIntent(PendingIntent.getActivity(this, ID, open, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        return try {
            ServiceCompat.startForeground(this, ID, b.build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_LOCATION)
            true
        } catch (e: Exception) {
            // Not allowed now (started from the background without always):
            // the updates go on as Android lets them.
            android.util.Log.w("asuka", "location: the foreground service can't start: $e")
            false
        }
    }

    fun stop() {
        stopForeground(STOP_FOREGROUND_REMOVE)
        stopSelf()
        running = null
    }

    override fun onDestroy() {
        if (running === this) running = null
        super.onDestroy()
    }
}

fun makeLocator(): Locator = AndroidLocator(AsukaModules.context)
