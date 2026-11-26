// platform/car on Android: Android Auto. The app's category (asuka.toml's
// [car], which the CLI declares with the package's service) decides how:
// a templated app's car screen is the Car App Library's (AsukaCarService,
// its Session and a Screen per template on the stack, Templates.kt); an
// audio app's is Android Auto's own media browser and player
// (AsukaCarMediaService, Media.kt), whose tree is the templates' rows.
// Either starts the app's code when the car does, and CarHub keeps the
// stack Asuka sends, and hands back what the driver chooses, by its
// handler's number. On the main thread.
package platform.car

import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.drawable.Drawable
import android.os.Handler
import android.os.Looper
import dev.asuka.AsukaImages
import dev.asuka.AsukaModules
import dev.asuka.AsukaRuntime
import dev.asuka.AsukaUnsupportedException
import dev.asuka.symbol
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.withContext

internal object CarHub : Head {
    private val main = Handler(Looper.getMainLooper())

    /** What Asuka shows, the root first. */
    var stack: List<Shown> = emptyList()
        private set

    private val connectedWatchers = ArrayList<(Boolean) -> kotlin.Unit>()
    private val choseWatchers = ArrayList<(Long) -> kotlin.Unit>()
    private val backWatchers = ArrayList<(Long) -> kotlin.Unit>()
    private val shownListeners = ArrayList<() -> kotlin.Unit>()

    /** The templated app's session, while a car shows it. */
    var session: AsukaCarSession? = null
        set(v) {
            field = v
            connectedChanged()
        }

    /** Android Auto's media browser is connected (an audio app). */
    var media = false
        set(v) {
            if (field == v) return
            field = v
            connectedChanged()
        }

    val connected get() = session != null || media

    private fun connectedChanged() {
        val c = connected
        for (w in connectedWatchers.toList()) w(c)
    }

    /** The driver chose what n names. */
    fun chose(n: Long) {
        if (n > 0) for (w in choseWatchers.toList()) w(n)
    }

    /** The car took templates away: left are left. */
    fun back(left: Int) {
        for (w in backWatchers.toList()) w(left.toLong())
    }

    /** Calls l each time Asuka shows a stack, until removed. */
    fun onShown(l: () -> kotlin.Unit) = shownListeners.add(l)
    fun offShown(l: () -> kotlin.Unit) = shownListeners.remove(l)

    override fun show(stack: List<Shown>) {
        this.stack = stack
        session?.render()
        for (l in shownListeners.toList()) l()
    }

    override suspend fun connections(connected: (Boolean) -> kotlin.Unit) {
        withContext(Dispatchers.Main) {
            connectedWatchers.add(connected)
            connected(this@CarHub.connected)
        }
        try {
            awaitCancellation()
        } finally {
            main.post { connectedWatchers.remove(connected) }
        }
    }

    override suspend fun events(chose: (Long) -> kotlin.Unit, back: (Long) -> kotlin.Unit) {
        val stop = withContext(Dispatchers.Main) { listen(chose, back) }
        try {
            awaitCancellation()
        } finally {
            main.post { stop() }
        }
    }

    /** Hears what the driver chooses and the car's back button, until the function it returns is called. Main thread. */
    internal fun listen(chose: (Long) -> kotlin.Unit, back: (Long) -> kotlin.Unit): () -> kotlin.Unit {
        choseWatchers.add(chose)
        backWatchers.add(back)
        return {
            choseWatchers.remove(chose)
            backWatchers.remove(back)
        }
    }

    /** Starts the app's code (a test's stands in for the core). */
    internal var starter: (Context) -> kotlin.Unit = { AsukaRuntime.get(it).startHeadless() }

    /** The app's code runs: a car started the process, before any Activity. */
    fun start(context: Context) = starter(context)
}

/** Images for the car: an asset, a file or an http(s) URL, loaded once and kept; a symbol drawn. */
internal object CarImages {
    /** A symbol's drawable, the host's (a test's stands in for it). */
    internal var symbols: (Context, String) -> Drawable? = { c, name -> AsukaRuntime.get(c).symbol(name) }

    private val loaded = HashMap<String, Bitmap>()
    private val loading = HashSet<String>()
    private val waiting = ArrayList<() -> kotlin.Unit>()

    /** The image if it's in; else it's loaded, and changed runs once it is. Main thread. */
    fun get(context: Context, source: String, changed: () -> kotlin.Unit): Bitmap? {
        if (source.isEmpty()) return null
        loaded[source]?.let { return it }
        waiting.add(changed)
        if (loading.add(source)) {
            AsukaImages.load(context, source) { r ->
                loading.remove(source)
                r.getOrNull()?.bitmap?.let { loaded[source] = scaled(it, 256) }
                val ws = ArrayList(waiting)
                waiting.clear()
                for (w in ws) w()
            }
        }
        return null
    }

    /** The image now, waiting for nothing (null: not in). */
    fun cached(source: String): Bitmap? = loaded[source]

    /** A symbol (an SF Symbol's name, Material's) drawn at size pixels. */
    fun symbol(context: Context, name: String, size: Int = 96): Bitmap? {
        if (name.isEmpty()) return null
        val key = "symbol:$name"
        loaded[key]?.let { return it }
        val d = symbols(context, name) ?: return null
        val b = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        d.setBounds(0, 0, size, size)
        d.setTint(android.graphics.Color.WHITE)
        d.draw(Canvas(b))
        loaded[key] = b
        return b
    }

    private fun scaled(b: Bitmap, max: Int): Bitmap {
        val side = maxOf(b.width, b.height)
        if (side <= max) return b
        val s = max.toFloat() / side
        return Bitmap.createScaledBitmap(b, maxOf(1, (b.width * s).toInt()), maxOf(1, (b.height * s).toInt()), true)
    }
}

/** Whether the app's manifest declares the service, as the CLI does for asuka.toml's [car]. */
private fun declares(context: Context, service: Class<*>): Boolean = try {
    context.packageManager.getServiceInfo(ComponentName(context, service), 0)
    true
} catch (_: PackageManager.NameNotFoundException) {
    false
}

fun makeHead(): Head {
    val ctx = AsukaModules.context
    if (!declares(ctx, AsukaCarService::class.java) && !declares(ctx, AsukaCarMediaService::class.java)) {
        throw AsukaUnsupportedException("no car screen: asuka.toml's [car] says the app's category")
    }
    return CarHub
}
