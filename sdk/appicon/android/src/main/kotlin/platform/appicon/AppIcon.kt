// platform/appicon on Android: the launcher's entries are activity
// aliases of the app's activity, one an icon (the build's manifest:
// dev.asuka.AppIcon for the app's own, dev.asuka.AppIcon.<name> for each
// alternate), one enabled. set enables the one asked for and disables the
// others, as the app leaves the screen: changing the launcher's entry
// while the app shows can close it. The system keeps the choice.
package platform.appicon

import android.app.Activity
import android.app.Application
import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.os.Bundle
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private const val ALIAS = "dev.asuka.AppIcon"

class AndroidIcons(private val app: Context) : Icons {
    private val pm = app.packageManager
    // The icon asked for and not yet applied (the app's in front); null: none.
    private var pending: String? = null
    private var started = 0
    private var watching = false

    /** The aliases the manifest has: "" for the app's own, else each alternate's name. */
    private fun aliases(): List<String> {
        val flags = PackageManager.GET_ACTIVITIES or PackageManager.MATCH_DISABLED_COMPONENTS
        val info = runCatching { pm.getPackageInfo(app.packageName, flags) }.getOrNull() ?: return emptyList()
        return info.activities.orEmpty().mapNotNull { a ->
            when {
                a.name == ALIAS -> ""
                a.name.startsWith("$ALIAS.") -> a.name.removePrefix("$ALIAS.")
                else -> null
            }
        }
    }

    private fun component(name: String) = ComponentName(app.packageName, if (name.isEmpty()) ALIAS else "$ALIAS.$name")

    /** Whether the alias for name is enabled: its setting, else the manifest's (the app's own alone). */
    private fun enabled(name: String): Boolean = when (pm.getComponentEnabledSetting(component(name))) {
        PackageManager.COMPONENT_ENABLED_STATE_ENABLED -> true
        PackageManager.COMPONENT_ENABLED_STATE_DEFAULT -> name.isEmpty()
        else -> false
    }

    override fun names(): List<String> = aliases().filter { it.isNotEmpty() }.sorted()

    override fun current(): String {
        pending?.let { return it }
        return aliases().firstOrNull { it.isNotEmpty() && enabled(it) } ?: ""
    }

    override suspend fun set(name: String) = withContext(Dispatchers.Main) {
        val all = aliases()
        if (all.isEmpty()) throw AppIconException(AppIconError.NOT_AVAILABLE)
        if (name !in all) throw AppIconException(AppIconError.UNKNOWN)
        pending = name
        watch()
        // Out of sight already (a task in the background): at once.
        if (started == 0) apply()
    }

    /** Enables the pending icon's alias, then disables the others'. */
    private fun apply() {
        val name = pending ?: return
        pending = null
        val all = aliases()
        if (enabled(name) && all.none { it != name && enabled(it) }) return
        try {
            pm.setComponentEnabledSetting(component(name), PackageManager.COMPONENT_ENABLED_STATE_ENABLED, PackageManager.DONT_KILL_APP)
            for (other in all) {
                if (other != name) {
                    pm.setComponentEnabledSetting(component(other), PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP)
                }
            }
        } catch (e: Exception) {
            android.util.Log.w("asuka", "appicon: the launcher's icon didn't change: $e")
        }
    }

    /** Counts the app's started activities, to apply the icon as the last one stops. */
    private fun watch() {
        if (watching) return
        watching = true
        started = if (AsukaModules.activity != null) 1 else 0
        (app.applicationContext as Application).registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
            override fun onActivityStarted(a: Activity) { started++ }
            override fun onActivityStopped(a: Activity) {
                started = maxOf(0, started - 1)
                if (started == 0) apply()
            }
            override fun onActivityCreated(a: Activity, b: Bundle?) {}
            override fun onActivityResumed(a: Activity) {}
            override fun onActivityPaused(a: Activity) {}
            override fun onActivitySaveInstanceState(a: Activity, b: Bundle) {}
            override fun onActivityDestroyed(a: Activity) {}
        })
    }
}

fun makeIcons(): Icons = AndroidIcons(AsukaModules.context)
