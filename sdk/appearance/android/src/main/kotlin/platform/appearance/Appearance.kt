// platform/appearance on Android: the accent is the wallpaper's color
// Material You takes (the system's accent1, at its light or dark tone, as
// the system's dynamic primary), none before Android 12, which has no
// system accent (an app's colors are its own); more contrast is the contrast level
// above standard (Android 14) or high contrast text. Android has no
// Reduce Transparency. The configuration's changes (dark mode, the
// wallpaper's colors) come through the application's callbacks.
package platform.appearance

import android.app.UiModeManager
import android.content.ComponentCallbacks
import android.content.Context
import android.content.res.Configuration
import android.database.ContentObserver
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.provider.Settings as AndroidSettings
import androidx.core.content.ContextCompat
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.withContext

class MaterialAppearance(private val app: Context) : System {
    private val ui = app.getSystemService(Context.UI_MODE_SERVICE) as UiModeManager

    override fun current(): Settings {
        val contrast = if (Build.VERSION.SDK_INT >= 34 && ui.contrast > 0f) true
        else AndroidSettings.Secure.getInt(app.contentResolver, "high_text_contrast_enabled", 0) == 1
        // Before Android 12 the system has no accent of its own: none.
        if (Build.VERSION.SDK_INT < 31) return Settings(hasAccent = false, increaseContrast = contrast, reduceTransparency = false)
        // The system's own colors, whatever the app's scheme.
        val night = (app.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK) == Configuration.UI_MODE_NIGHT_YES
        val c = app.getColor(if (night) android.R.color.system_accent1_200 else android.R.color.system_accent1_600)
        return Settings(hasAccent = true, red = ((c shr 16) and 0xff).toLong(), green = ((c shr 8) and 0xff).toLong(), blue = (c and 0xff).toLong(),
            increaseContrast = contrast, reduceTransparency = false)
    }

    override suspend fun watch(changed: (Settings) -> kotlin.Unit) = withContext(Dispatchers.Main) {
        var last: Settings? = null
        val report = {
            val s = current()
            if (s != last) {
                last = s
                changed(s)
            }
        }
        val main = Handler(Looper.getMainLooper())
        val observer = object : ContentObserver(main) {
            override fun onChange(selfChange: Boolean) = report()
        }
        val callbacks = object : ComponentCallbacks {
            override fun onConfigurationChanged(c: Configuration) = report()
            @Deprecated("") override fun onLowMemory() {}
        }
        val highText = AndroidSettings.Secure.getUriFor("high_text_contrast_enabled")
        app.contentResolver.registerContentObserver(highText, false, observer)
        app.registerComponentCallbacks(callbacks)
        var contrast: Any? = null
        if (Build.VERSION.SDK_INT >= 34) {
            val l = UiModeManager.ContrastChangeListener { report() }
            ui.addContrastChangeListener(ContextCompat.getMainExecutor(app), l)
            contrast = l
        }
        report()
        try {
            awaitCancellation()
        } finally {
            app.contentResolver.unregisterContentObserver(observer)
            app.unregisterComponentCallbacks(callbacks)
            if (Build.VERSION.SDK_INT >= 34) (contrast as? UiModeManager.ContrastChangeListener)?.let { ui.removeContrastChangeListener(it) }
        }
    }
}

fun makeSystem(): System = MaterialAppearance(AsukaModules.context)
