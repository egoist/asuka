// platform/screen on Android: the displays are DisplayManager's (the
// default display first, in dp, each one's work area its bounds: the
// system bars are the app's insets), followed by its listener. The
// orientation, the bars and FLAG_SECURE are the host's AsukaDisplay's,
// which the activity applies again when it's made again; screenshots
// are Android 14's ScreenCaptureCallback (DETECT_SCREEN_CAPTURE). The
// brightness is the window's own (AsukaDisplay's too), the system's
// setting read when the app sets none.
package platform.screen

import android.content.Context
import android.hardware.display.DisplayManager
import android.os.Handler
import android.os.Looper
import android.view.WindowManager
import dev.asuka.AsukaDisplay
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.withContext

class AndroidScreens(private val app: Context) : Screens {
    private val dm = app.getSystemService(Context.DISPLAY_SERVICE) as DisplayManager

    override fun displays(): List<Display> {
        val all = dm.displays.sortedBy { if (it.displayId == android.view.Display.DEFAULT_DISPLAY) -1 else it.displayId }
        var x = 0.0
        return all.map { d ->
            val ctx = app.createDisplayContext(d)
            val density = ctx.resources.displayMetrics.density
            val px = ctx.getSystemService(WindowManager::class.java)?.maximumWindowMetrics?.bounds
            val w = ((px?.width() ?: ctx.resources.displayMetrics.widthPixels) / density).toDouble()
            val h = ((px?.height() ?: ctx.resources.displayMetrics.heightPixels) / density).toDouble()
            // The default display at the origin, the others after it.
            val r = Rect(x = x, y = 0.0, width = w, height = h)
            x += w
            Display(id = d.displayId.toString(), name = d.name ?: "", bounds = r, workArea = r, scale = density.toDouble(),
                refreshRate = d.refreshRate.toDouble(), primary = d.displayId == android.view.Display.DEFAULT_DISPLAY)
        }
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
}

fun makeScreens(): Screens = AndroidScreens(AsukaModules.context)
