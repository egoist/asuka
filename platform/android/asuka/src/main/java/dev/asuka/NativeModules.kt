// Typed native modules (//asuka:native) and views (//asuka:view): what
// the Kotlin that asuka native generates for a package calls, and what
// the core's calls reach through JNI (core/src/abi/jni_android.c), from
// any thread.
//
// The wire format is core/src/native's: values in declaration order,
// little-endian.
package dev.asuka

import android.os.Handler
import android.os.Looper
import java.time.Instant
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.atomic.AtomicLong
import kotlin.time.Duration
import kotlin.time.Duration.Companion.nanoseconds
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob

/** Encodes values (core/src/native's writer). */
class AsukaNativeWriter {
    private var b = ByteArray(64)
    private var n = 0

    private fun room(k: Int) {
        if (n + k > b.size) b = b.copyOf(maxOf(b.size * 2, n + k))
    }

    private fun put64(v: Long) {
        room(8)
        for (i in 0 until 8) b[n + i] = (v ushr (8 * i)).toByte()
        n += 8
    }

    private fun put32(v: Int) {
        room(4)
        for (i in 0 until 4) b[n + i] = (v ushr (8 * i)).toByte()
        n += 4
    }

    fun bool(v: Boolean) { room(1); b[n++] = if (v) 1 else 0 }
    fun int8(v: Byte) { room(1); b[n++] = v }
    fun int16(v: Short) { room(2); b[n++] = v.toByte(); b[n++] = (v.toInt() ushr 8).toByte() }
    fun int32(v: Int) = put32(v)
    fun int64(v: Long) = put64(v)
    fun int(v: Long) = put64(v)
    fun uint8(v: UByte) = int8(v.toByte())
    fun uint16(v: UShort) = int16(v.toShort())
    fun uint32(v: UInt) = put32(v.toInt())
    fun uint64(v: ULong) = put64(v.toLong())
    fun uint(v: ULong) = put64(v.toLong())
    fun float32(v: Float) = put32(java.lang.Float.floatToRawIntBits(v))
    fun float64(v: Double) = put64(java.lang.Double.doubleToRawLongBits(v))
    fun string(v: String) = data(v.toByteArray(Charsets.UTF_8))

    fun data(v: ByteArray) {
        put32(v.size)
        room(v.size)
        System.arraycopy(v, 0, b, n, v.size)
        n += v.size
    }

    /** A time: seconds and nanoseconds since 1970. */
    fun time(v: Instant) { put64(v.epochSecond); put32(v.nano) }
    fun duration(v: Duration) = put64(v.inWholeNanoseconds)
    /** A Decimal (std/decimal's): its coefficient, without trailing zeros,
     *  and its exponent. One of more digits than a Long holds is rounded
     *  half to even to 18 significant digits. */
    fun decimal(v: java.math.BigDecimal) {
        var d = v
        if (d.signum() != 0 && d.precision() > 18) d = d.round(java.math.MathContext(18, java.math.RoundingMode.HALF_EVEN))
        if (d.signum() == 0) {
            put64(0)
            put64(0)
            return
        }
        d = d.stripTrailingZeros()
        put64(d.unscaledValue().toLong())
        put64(-d.scale().toLong())
    }
    fun len(k: Int) = put32(k)
    fun present(v: Boolean) = bool(v)

    fun toByteArray(): ByteArray = b.copyOf(n)

    companion object {
        /** An answer's writer: its status, ok, first. */
        fun ok() = AsukaNativeWriter().apply { int8(0) }
    }
}

/**
 * One of the app's views a method was passed (an Asuka Ref): the id its
 * host knows it by.
 */
data class AsukaViewRef(val id: Long) {
    /**
     * The view while the app shows it, else null: a WebView's is its
     * android.webkit.WebView. Read it on the main thread.
     */
    val view: android.view.View?
        get() = if (id <= 0 || id > Int.MAX_VALUE) null else AsukaRuntime.current?.nodes?.get(id.toInt())?.view
}

/** Decodes values (core/src/native's reader); past what's there, every read is its zero value and failed is true. */
class AsukaNativeReader(private val b: ByteArray) {
    private var p = 0
    var failed = false
        private set

    private fun take(k: Int): Int {
        if (failed || k < 0 || b.size - p < k) {
            failed = true
            return -1
        }
        val at = p
        p += k
        return at
    }

    private fun get64(): Long {
        val at = take(8)
        if (at < 0) return 0
        var v = 0L
        for (i in 7 downTo 0) v = (v shl 8) or (b[at + i].toLong() and 0xff)
        return v
    }

    private fun get32(): Int {
        val at = take(4)
        if (at < 0) return 0
        var v = 0
        for (i in 3 downTo 0) v = (v shl 8) or (b[at + i].toInt() and 0xff)
        return v
    }

    fun bool(): Boolean = int8().toInt() != 0
    fun int8(): Byte { val at = take(1); return if (at < 0) 0 else b[at] }
    fun int16(): Short {
        val at = take(2)
        return if (at < 0) 0 else ((b[at].toInt() and 0xff) or (b[at + 1].toInt() shl 8)).toShort()
    }
    fun int32(): Int = get32()
    fun int64(): Long = get64()
    fun int(): Long = get64()
    fun uint8(): UByte = int8().toUByte()
    fun uint16(): UShort = int16().toUShort()
    fun uint32(): UInt = get32().toUInt()
    fun uint64(): ULong = get64().toULong()
    fun viewRef(): AsukaViewRef = AsukaViewRef(get64())
    fun uint(): ULong = get64().toULong()
    fun float32(): Float = java.lang.Float.intBitsToFloat(get32())
    fun float64(): Double = java.lang.Double.longBitsToDouble(get64())
    fun string(): String = String(data(), Charsets.UTF_8)

    fun data(): ByteArray {
        val k = get32()
        val at = take(k)
        return if (at < 0) ByteArray(0) else b.copyOfRange(at, at + k)
    }

    fun time(): Instant {
        val s = get64()
        val ns = get32()
        return Instant.ofEpochSecond(s, ns.toLong())
    }

    fun duration(): Duration = get64().nanoseconds
    /** A Decimal (std/decimal's): coefficient × 10^exponent. */
    fun decimal(): java.math.BigDecimal {
        val c = get64()
        val e = get64()
        if (c == 0L || e < Int.MIN_VALUE + 1 || e > Int.MAX_VALUE) return java.math.BigDecimal.ZERO
        return java.math.BigDecimal.valueOf(c, (-e).toInt())
    }

    /** A slice's or a map's length, bounded by what's left. */
    fun len(): Int {
        val k = get32()
        if (k < 0 || k > b.size - p) {
            failed = true
            return 0
        }
        return k
    }

    fun present(): Boolean = bool()
    /** A callback's id (0: none). */
    fun callback(): Long = get64()

    /** Throws when a read went past what's there. */
    fun check() {
        if (failed) throw AsukaMalformedException()
    }
}

/** An error a package declares (its enum ...: Error): the app gets that value. */
/**
 * An object of a package's that hears Asuka let it go (on the main
 * thread): to free what it holds, as a Swift object's deinit does.
 */
interface AsukaReleasing {
    fun released()
}

interface AsukaDeclaredError {
    val asukaIndex: Int
}

/** What the platform doesn't have: Go gets errors.ErrUnsupported. */
class AsukaUnsupportedException(message: String) : Exception(message)

class AsukaMalformedException : Exception("asuka: a malformed call")

class AsukaClosedException : Exception("asuka: the object is closed")

/** A Go package's native code, as asuka native generates it: the host finds it by its class's name (the module's Kotlin, in Go). */
interface AsukaModuleProvider {
    fun make(iface: Int): Any
    fun call(obj: Any, iface: Int, method: Int, r: AsukaNativeReader, w: AsukaNativeWriter)
    fun start(obj: Any, iface: Int, method: Int, r: AsukaNativeReader, done: (Result<AsukaNativeWriter>) -> Unit): Job?
    fun makeView(name: String, ctx: AsukaNativeContext): AsukaNativeView?
}

/**
 * The typed modules' runtime: the objects Go holds, by handle; the calls
 * it started; and the way back to Go (JNI).
 */
object AsukaModules {
    private class Entry(val obj: Any, val provider: AsukaModuleProvider, val iface: Int)

    private val objects = ConcurrentHashMap<Long, Entry>()
    private val nextHandle = AtomicLong()
    private val jobs = ConcurrentHashMap<Long, Job>()
    private val finished = ConcurrentHashMap.newKeySet<Long>()
    private val providers = ConcurrentHashMap<String, AsukaModuleProvider>()
    private val main = Handler(Looper.getMainLooper())

    /**
     * The application's context, for a module's Android APIs (any thread).
     * Throws before the host started, when no module can be called.
     */
    val context: android.content.Context
        get() = AsukaRuntime.current?.app ?: throw IllegalStateException("asuka: the host hasn't started")

    /**
     * The activity showing the app, if one is: for what needs one (a
     * dialog, a permission request, a result). Read it on the main thread.
     */
    val activity: android.app.Activity? get() = AsukaRuntime.current?.activity

    /**
     * A Color that crossed to a package's Kotlin (a Long, the core's
     * encoding) as an Android color: a semantic one from the app's theme.
     * Read it on the main thread.
     */
    fun color(v: Long, fallback: Int = android.graphics.Color.TRANSPARENT): Int =
        AsukaRuntime.current?.color(v, fallback) ?: when ((v ushr 32).toInt()) {
            2 -> (v and 0xffffffffL).toInt().let { (it ushr 8) or (it shl 24) }
            else -> fallback
        }

    /**
     * A source as the hosts take one (an http(s) or file URL, an absolute
     * path, or a file in the app's assets: under asuka dev the ones it
     * pushed, else the APK's) as a Uri, for a package's player or loader.
     */
    fun uri(source: String): android.net.Uri = when {
        source.startsWith("http://") || source.startsWith("https://") || source.startsWith("file://") ||
            source.startsWith("content://") || source.startsWith("data:") -> android.net.Uri.parse(source)
        source.startsWith("/") -> android.net.Uri.fromFile(java.io.File(source))
        else -> {
            val rel = source.removePrefix("./")
            val pushed = java.io.File(context.filesDir, "asuka/assets/$rel")
            if (pushed.exists()) android.net.Uri.fromFile(pushed) else android.net.Uri.parse("asset:///$rel")
        }
    }

    /**
     * Asks for permissions (once any is granted, done gets true at once),
     * on the main thread: the system's dialog over the activity showing
     * the app; false without one.
     */
    fun requestPermissions(permissions: Array<String>, done: (Boolean) -> kotlin.Unit) {
        val rt = AsukaRuntime.current ?: run { done(false); return }
        onMain { rt.withPermission(permissions, done) }
    }

    /**
     * Whether the app has permission: one newer than this Android (the
     * notifications', nearby devices', activity recognition's) counts as
     * had where what it guards needed no asking.
     */
    fun granted(permission: String): Boolean = AsukaRuntime.current?.granted(permission) ?: false

    /** Whether the app's manifest asks for permission (its own, or a package's). */
    fun declares(permission: String): Boolean = AsukaRuntime.current?.declares(permission) ?: false

    /**
     * A content: Uri of a file of the app's (its cache or files), through
     * the app's FileProvider: what another app (the camera's, a share
     * target) is handed.
     */
    fun fileUri(file: java.io.File): android.net.Uri =
        androidx.core.content.FileProvider.getUriForFile(context, context.packageName + ".asuka.files", file)

    /** Where the generated Kotlin's suspend funs run. */
    @JvmField val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    @JvmStatic external fun done(call: Long, answer: ByteArray)
    @JvmStatic external fun invoke(id: Long, args: ByteArray): ByteArray?

    /** The provider of a class's name, looked up once. */
    /**
     * The provider of a class's name: its Kotlin class, or the package's
     * path (platform/network, as the C core names it), whose class is
     * KotlinClass's (internal/nativegen): the path's host reversed, then
     * its segments, lower case, letters and digits, then .AsukaModule.
     */
    fun provider(name: String): AsukaModuleProvider? = providers[name] ?: try {
        val cls = if (name.endsWith(".AsukaModule")) name else kotlinClass(name)
        (Class.forName(cls).getField("INSTANCE").get(null) as AsukaModuleProvider).also { providers[name] = it }
    } catch (_: ReflectiveOperationException) {
        null
    }

    private val reserved = ("as break class continue do else false for fun if in interface is null object package return super this throw true try typealias typeof val var when while " +
        "abstract assert boolean byte case catch char const default double enum extends final finally float goto implements import instanceof int long native new private protected public short static strictfp switch synchronized throws transient void volatile").split(" ").toSet()

    internal fun kotlinClass(path: String): String {
        var parts = path.split("/")
        val segs = ArrayList<String>()
        if (parts[0].contains('.')) {
            segs.addAll(parts[0].split(".").reversed())
            parts = parts.drop(1)
        }
        segs.addAll(parts)
        return segs.joinToString(".") { seg ->
            var s = seg.map { c -> if (c.code < 128 && c.isLetterOrDigit()) c else '_' }.joinToString("").lowercase()
            if (s.isEmpty() || s[0].isDigit()) s = "_$s"
            if (s in reserved) s += "_"
            s
        } + ".AsukaModule"
    }

    /** Keeps an object for Go: its handle. */
    fun retain(obj: Any, provider: AsukaModuleProvider, iface: Int): Long {
        val h = nextHandle.incrementAndGet()
        objects[h] = Entry(obj, provider, iface)
        return h
    }

    /** The object of a handle Go passed (null for 0, or one let go). */
    fun obj(h: Long): Any? = objects[h]?.obj

    // What Go calls, through JNI.

    @JvmStatic fun new(cls: ByteArray, iface: Int): ByteArray {
        val name = String(cls, Charsets.UTF_8)
        val p = provider(name) ?: return answer(AsukaUnsupportedException("no Kotlin $name: build the app with its packages' native code"))
        return try {
            val o = p.make(iface)
            AsukaNativeWriter.ok().apply { int64(retain(o, p, iface)) }.toByteArray()
        } catch (e: Throwable) {
            answer(e)
        }
    }

    @JvmStatic fun call(h: Long, method: Int, input: ByteArray): ByteArray {
        val e = objects[h] ?: return answer(AsukaClosedException())
        val w = AsukaNativeWriter.ok()
        return try {
            e.provider.call(e.obj, e.iface, method, AsukaNativeReader(input), w)
            w.toByteArray()
        } catch (t: Throwable) {
            answer(t)
        }
    }

    @JvmStatic fun start(h: Long, method: Int, input: ByteArray, call: Long) {
        val e = objects[h]
        if (e == null) {
            done(call, answer(AsukaClosedException()))
            return
        }
        val job = try {
            e.provider.start(e.obj, e.iface, method, AsukaNativeReader(input)) { finish(call, it) }
        } catch (t: Throwable) {
            finish(call, Result.failure(t))
            null
        }
        if (!finished.remove(call) && job != null) jobs[call] = job
    }

    private fun finish(call: Long, result: Result<AsukaNativeWriter>) {
        if (jobs.remove(call) == null) finished.add(call)
        done(call, result.fold({ it.toByteArray() }, { answer(it) }))
    }

    @JvmStatic fun cancel(call: Long) {
        jobs[call]?.cancel()
    }

    @JvmStatic fun release(h: Long) {
        val o = objects.remove(h)?.obj as? AsukaReleasing ?: return
        onMain { o.released() }
    }

    // What the generated Kotlin calls.

    /** Calls a Go callback that answers nothing (it runs on Go's main thread). */
    fun notify(id: Long, w: AsukaNativeWriter) {
        if (id != 0L) invoke(id, w.toByteArray())
    }

    /** Calls a Go callback and reads its answer. */
    fun ask(id: Long, w: AsukaNativeWriter): AsukaNativeReader =
        AsukaNativeReader(if (id == 0L) ByteArray(0) else invoke(id, w.toByteArray()) ?: ByteArray(0))

    /** Runs body on the main thread: now if this is it, else waiting for it. */
    fun <T> onMain(body: () -> T): T {
        if (Looper.myLooper() == Looper.getMainLooper()) return body()
        var result: Result<T>? = null
        val latch = CountDownLatch(1)
        main.post {
            result = runCatching(body)
            latch.countDown()
        }
        latch.await()
        return result!!.getOrThrow()
    }

    private val results = AtomicLong()

    /**
     * Starts an activity for its result through the showing activity's
     * registry (a picker, a system screen) and waits for it: the
     * contract's answer. Any thread; it fails with an unsupported error
     * when no activity shows the app. Cancelling the coroutine leaves the
     * activity, whose answer is then dropped.
     */
    suspend fun <I, O> launch(contract: androidx.activity.result.contract.ActivityResultContract<I, O>, input: I): O =
        kotlinx.coroutines.suspendCancellableCoroutine { cont ->
            main.post {
                val act = activity as? androidx.activity.ComponentActivity
                if (act == null) {
                    cont.resumeWith(Result.failure(AsukaUnsupportedException("no activity shows the app")))
                    return@post
                }
                var launcher: androidx.activity.result.ActivityResultLauncher<I>? = null
                launcher = act.activityResultRegistry.register("asuka.native.${results.incrementAndGet()}", contract) { out ->
                    launcher?.unregister()
                    if (cont.isActive) cont.resumeWith(Result.success(out))
                }
                try {
                    launcher.launch(input)
                } catch (t: Throwable) {
                    launcher.unregister()
                    cont.resumeWith(Result.failure(t))
                }
            }
        }

    /**
     * The local file a source names, for a package that hands files to
     * the system (platform/preview, platform/print): a path, a file:// URL, an
     * asset's path (pushed by asuka dev, else copied out of the APK), or an
     * http(s) URL, downloaded to the caches first (under its name, with the
     * extension of type, or the server's, if it has none). Any thread.
     */
    suspend fun localFile(source: String, type: String = ""): java.io.File =
        kotlinx.coroutines.withContext(Dispatchers.IO) {
            (AsukaRuntime.current ?: throw IllegalStateException("asuka: the host hasn't started")).localFile(AsukaSource(source, type))
        }

    private val urlWatchers = java.util.concurrent.CopyOnWriteArrayList<(String) -> Boolean>()
    private val lifecycleWatchers = java.util.concurrent.CopyOnWriteArrayList<(Boolean) -> kotlin.Unit>()

    /**
     * Sees each URL the app is opened with (its scheme, an app link)
     * before the app does, on the main thread: a handler that answers
     * true takes it (a sign-in's redirect), and the app never gets it.
     * Returns what stops it.
     */
    fun watchURLs(handler: (String) -> Boolean): () -> kotlin.Unit {
        urlWatchers.add(handler)
        return { urlWatchers.remove(handler) }
    }

    /**
     * Calls f on the main thread as the activity showing the app pauses
     * (false: another app's, a Custom Tab, came over it) and resumes
     * (true). Returns what stops it.
     */
    fun watchLifecycle(f: (resumed: Boolean) -> kotlin.Unit): () -> kotlin.Unit {
        lifecycleWatchers.add(f)
        return { lifecycleWatchers.remove(f) }
    }

    /** The host's: a URL opened the app; whether a package took it. */
    internal fun openedURL(url: String): Boolean = urlWatchers.any { it(url) }

    /** The host's: the activity paused or resumed. */
    internal fun lifecycle(resumed: Boolean) {
        for (f in lifecycleWatchers) f(resumed)
    }

    /** An error's answer: declared (its index), unsupported, canceled, or its class, 0 and message. */
    fun answer(e: Throwable): ByteArray {
        val w = AsukaNativeWriter()
        when {
            e is AsukaDeclaredError -> { w.int8(1); w.int32(e.asukaIndex) }
            e is AsukaUnsupportedException -> { w.int8(3); w.string(e.message ?: "") }
            e is kotlinx.coroutines.CancellationException -> w.int8(4)
            else -> { w.int8(2); w.string(e.javaClass.name); w.int64(0); w.string(e.message ?: e.toString()) }
        }
        return w.toByteArray()
    }

    /** A typed view of a kind (its module's class # its name). */
    fun makeView(kind: String, ctx: AsukaNativeContext): AsukaNativeView? {
        val i = kind.indexOf('#')
        if (i < 0) return null
        return provider(kind.substring(0, i))?.makeView(kind.substring(i + 1), ctx)
    }
}
