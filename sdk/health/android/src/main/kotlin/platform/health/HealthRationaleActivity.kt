// Health Connect's link to why the app asks (its permission screen's, and
// on Android 14 and later the settings' "privacy policy"): the app's
// privacy policy, asuka.toml's health.privacyPolicy, which the CLI puts
// in the manifest's meta-data. Opened in the browser; the activity shows
// nothing of its own.
package platform.health

import android.app.Activity
import android.content.ActivityNotFoundException
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import androidx.core.net.toUri

/** The meta-data the CLI writes asuka.toml's health.privacyPolicy to. */
private const val POLICY = "platform.health.privacyPolicy"

class HealthRationaleActivity : Activity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val url = try {
            @Suppress("DEPRECATION")
            packageManager.getApplicationInfo(packageName, PackageManager.GET_META_DATA).metaData?.getString(POLICY)
        } catch (_: PackageManager.NameNotFoundException) {
            null
        }
        if (url.isNullOrEmpty()) {
            android.util.Log.w("asuka", "health: no privacy policy to show: set asuka.toml's health.privacyPolicy")
        } else {
            try {
                startActivity(Intent(Intent.ACTION_VIEW, url.toUri()).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
            } catch (e: ActivityNotFoundException) {
                android.util.Log.w("asuka", "health: no browser for the privacy policy: $e")
            }
        }
        finish()
    }
}
