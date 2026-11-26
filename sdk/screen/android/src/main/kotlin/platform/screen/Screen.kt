// platform/screen on Android: the displays are DisplayManager's (the
// default display first, in dp, each one's work area its bounds: the
// system bars are the app's insets), followed by its listener. The
// orientation, the bars and FLAG_SECURE are the host's AsukaDisplay's,
// which the activity applies again when it's made again; screenshots
// are Android 14's ScreenCaptureCallback (DETECT_SCREEN_CAPTURE). The
// brightness is the window's own (AsukaDisplay's too), the system's
// setting read when the app sets none. A capture is the activity's
// window: PixelCopy's (with its SurfaceViews and TextureViews: video,
// maps) from Android 8, the views drawn into a bitmap before it.
package platform.screen

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.hardware.display.DisplayManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.DisplayMetrics
import android.view.PixelCopy
import android.view.Window
import android.view.WindowManager
import androidx.annotation.RequiresApi
import androidx.core.graphics.createBitmap
import dev.asuka.AsukaDisplay
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

class AndroidScreens(private val app: Context) : Screens {
    private val dm = app.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager

    override fun displays(): List<Display> {
        val all = dm.displays.sortedBy { if (it.displayId == android.view.Display.DEFAULT_DISPLAY) -1 else it.displayId }
        var x = 0.0
        return all.map { d ->
            val ctx = app.createDisplayContext(d)
            val density = ctx.resources.displayMetrics.density
            val px = pixels(ctx, d)
            val w = ((px?.width() ?: ctx.resources.displayMetrics.widthPixels) / density).toDouble()
            val h = ((px?.height() ?: ctx.resources.displayMetrics.heightPixels) / density).toDouble()
            // The default display at the origin, the others after it.
            val r = Rect(x = x, y = 0.0, width = w, height = h)
            x += w
            Display(id = d.displayId.toString(), name = d.name ?: "", bounds = r, workArea = r, scale = density.toDouble(),
                refreshRate = d.refreshRate.toDouble(), primary = d.displayId == android.view.Display.DEFAULT_DISPLAY)
        }
    }

    /** A display's whole size in pixels: before Android 11, its real metrics. */
    @Suppress("DEPRECATION")
    private fun pixels(ctx: Context, d: android.view.Display): android.graphics.Rect? {
        if (Build.VERSION.SDK_INT >= 30) return ctx.getSystemService(WindowManager::class.java)?.maximumWindowMetrics?.bounds
        val m = DisplayMetrics()
        d.getRealMetrics(m)
        return android.graphics.Rect(0, 0, m.widthPixels, m.heightPixels)
    }

    override suspend fun watchDisplays(changed: (List<Display>) -> kotlin.Unit) = withContext(Dispatchers.Main) {
        var last: List<Display>? = null
        val report = {
            val ds = displays()
            if (ds != last) {
                last = ds
                changed(ds)
            }
        }
        val listener = object : DisplayManager.DisplayListener {
            override fun onDisplayAdded(id: Int) = report()
            override fun onDisplayRemoved(id: Int) = report()
            override fun onDisplayChanged(id: Int) = report()
        }
        dm.registerDisplayListener(listener, Handler(Looper.getMainLooper()))
        report()
        try {
            awaitCancellation()
        } finally {
            dm.unregisterDisplayListener(listener)
        }
    }

    override fun lock(orientation: Orientation) {
        AsukaDisplay.lock = orientation.value.toInt()
        AsukaDisplay.applyNow()
    }

    override fun setBars(style: BarStyle, hideStatus: Boolean, hideNavigation: Boolean) {
        AsukaDisplay.style = style.value.toInt()
        AsukaDisplay.hideStatus = hideStatus
        AsukaDisplay.hideNavigation = hideNavigation
        AsukaDisplay.applyNow()
    }

    override fun preventCapture(on: Boolean) {
        AsukaDisplay.secure = on
        AsukaDisplay.applyNow()
    }

    override fun setBrightness(level: Double) {
        AsukaDisplay.brightness = level.toFloat()
        AsukaDisplay.applyNow()
    }

    override fun brightness(): Double {
        if (AsukaDisplay.brightness >= 0f) return AsukaDisplay.brightness.toDouble()
        // The system's setting, 0 to 255.
        val v = runCatching { android.provider.Settings.System.getInt(app.contentResolver, android.provider.Settings.System.SCREEN_BRIGHTNESS) }.getOrNull()
        return if (v == null) -1.0 else (v / 255.0).coerceIn(0.0, 1.0)
    }

    override suspend fun watchScreenshots(taken: (Long) -> kotlin.Unit) = withContext(Dispatchers.Main) {
        var n = 0L
        val stop = AsukaDisplay.watchScreenshots { taken(++n) }
        try {
            awaitCancellation()
        } finally {
            stop()
        }
    }

    // What the app's window shows: on a phone, what's on screen while
    // the app is in front. Another display's id: not available.
    override suspend fun capture(display: String): String {
        val bmp = withContext(Dispatchers.Main) {
            val act = AsukaModules.activity ?: throw CaptureException(CaptureError.NOT_AVAILABLE)
            val window = act.window ?: throw CaptureException(CaptureError.NOT_AVAILABLE)
            @Suppress("DEPRECATION")
            val shown = (if (Build.VERSION.SDK_INT >= 30) act.display else act.windowManager.defaultDisplay)?.displayId
                ?: android.view.Display.DEFAULT_DISPLAY
            if (display != "" && display != shown.toString()) throw CaptureException(CaptureError.NOT_AVAILABLE)
            val root = window.decorView
            if (root.width <= 0 || root.height <= 0) throw CaptureException(CaptureError.NOT_AVAILABLE)
            val b = createBitmap(root.width, root.height)
            // Not recycled when this throws: a cancelled PixelCopy may still write to it.
            if (Build.VERSION.SDK_INT >= 26) copy(window, b) else root.draw(Canvas(b))
            b
        }
        return withContext(Dispatchers.IO) {
            try {
                val f = File(app.cacheDir, "screen-${UUID.randomUUID()}.png")
                try {
                    f.outputStream().use { out ->
                        if (!bmp.compress(Bitmap.CompressFormat.PNG, 100, out)) throw Exception("screen.capture: the PNG wasn't written")
                    }
                } catch (e: Throwable) {
                    f.delete()
                    throw e
                }
                f.absolutePath
            } finally {
                bmp.recycle()
            }
        }
    }

    /** The window's pixels into b, as the compositor has them. */
    @RequiresApi(26)
    private suspend fun copy(window: Window, b: Bitmap) = suspendCancellableCoroutine<kotlin.Unit> { cont ->
        PixelCopy.request(window, b, { result ->
            if (result == PixelCopy.SUCCESS) cont.resume(kotlin.Unit)
            else cont.resumeWithException(Exception("screen.capture: PixelCopy failed ($result)"))
        }, Handler(Looper.getMainLooper()))
    }
}

fun makeScreens(): Screens = AndroidScreens(AsukaModules.context)
