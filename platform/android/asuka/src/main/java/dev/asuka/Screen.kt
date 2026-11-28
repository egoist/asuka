package dev.asuka

import android.app.Activity
import android.content.pm.ActivityInfo
import android.os.Build
import android.view.WindowManager
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat

/**
 * platform/screen's settings: the app's, kept here so that an Activity made
 * again (a configuration change) takes them, as AsukaActivity applies them.
 */
object AsukaDisplay {
    /** screen.Orientation; 0: the manifest's. */
    var lock = 0
    /** The manifest's orientation, before the first lock. */
    private var original: Int? = null
    var style = 0 // auto, light, dark
    var hideStatus = false
    var hideNavigation = false
    var secure = false
    /** The window's brightness, 0 to 1; negative: the system's (BRIGHTNESS_OVERRIDE_NONE). */
    var brightness = -1f
    val screenshots = ArrayList<(Result<ByteArray>) -> Unit>()
    private var callback: Any? = null // Activity.ScreenCaptureCallback, on Android 14 and later

    /** Applies the settings to the Activity: its orientation, bars and capture. */
    fun apply(act: AsukaActivity, night: Boolean) {
        if (original == null) original = act.requestedOrientation
        act.requestedOrientation = when (lock) {
            1 -> ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT
            2 -> ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            3 -> ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE
            4 -> ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE // its bottom on the left: UIKit's landscapeLeft
            5 -> ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE
            else -> original ?: ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
        }
        bars(act, night)
        if (secure) act.window.addFlags(WindowManager.LayoutParams.FLAG_SECURE)
        else act.window.clearFlags(WindowManager.LayoutParams.FLAG_SECURE)
        // The window's own: the system's again as the app leaves the
        // screen, the app's again as it comes back.
        val want = if (brightness < 0f) WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE else brightness.coerceIn(0f, 1f)
        if (act.window.attributes.screenBrightness != want) {
            act.window.attributes = act.window.attributes.apply { screenBrightness = want }
        }
    }

    /** The bars' icons and whether they show; after edge to edge, which sets the icons by the scheme. */
    fun bars(act: Activity, night: Boolean) {
        val c = WindowCompat.getInsetsController(act.window, act.window.decorView)
        val light = when (style) { 1 -> false; 2 -> true; else -> !night } // light bars: dark icons
        c.isAppearanceLightStatusBars = light
        c.isAppearanceLightNavigationBars = light
        c.systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
        if (hideStatus) c.hide(WindowInsetsCompat.Type.statusBars()) else c.show(WindowInsetsCompat.Type.statusBars())
        if (hideNavigation) c.hide(WindowInsetsCompat.Type.navigationBars()) else c.show(WindowInsetsCompat.Type.navigationBars())
    }

    private val watchers = ArrayList<() -> kotlin.Unit>()

    /** Applies the settings to the activity showing the app, now, as they're set (platform/screen). Main thread. */
    fun applyNow() {
        val rt = AsukaRuntime.current ?: return
        val act = rt.activity ?: return
        apply(act, rt.isNight(act.resources.configuration))
    }

    /**
     * Calls taken at each screenshot of the app (Android 14 and later),
     * until the function it returns is called (platform/screen). Main thread.
     */
    fun watchScreenshots(taken: () -> kotlin.Unit): () -> kotlin.Unit {
        watchers.add(taken)
        AsukaRuntime.current?.activity?.let { watch(it, true) }
        return {
            watchers.remove(taken)
            if (watchers.isEmpty() && screenshots.isEmpty()) AsukaRuntime.current?.activity?.let { watch(it, false) }
        }
    }

    /** Follows screenshots while the Activity shows and someone waits for one (Android 14 and later). */
    // DETECT_SCREEN_CAPTURE is in platform/screen's manifest, which an app
    // that watches screenshots has.
    @android.annotation.SuppressLint("MissingPermission")
    fun watch(act: Activity, on: Boolean) {
        if (Build.VERSION.SDK_INT < 34) return
        try {
            if (on && callback == null && (screenshots.isNotEmpty() || watchers.isNotEmpty())) {
                val cb = Activity.ScreenCaptureCallback {
                    val ws = ArrayList(screenshots)
                    screenshots.clear()
                    for (w in ws) w(Result.success(ByteArray(0)))
                    for (w in ArrayList(watchers)) w()
                }
                act.registerScreenCaptureCallback(act.mainExecutor, cb)
                callback = cb
            } else if (!on && callback != null) {
                act.unregisterScreenCaptureCallback(callback as Activity.ScreenCaptureCallback)
                callback = null
            }
        } catch (_: SecurityException) {
            // No DETECT_SCREEN_CAPTURE: the app doesn't import platform/screen's package path.
        }
    }
}
