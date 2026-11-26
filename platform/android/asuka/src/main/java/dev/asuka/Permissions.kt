package dev.asuka

import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.ContextCompat

private var asks = 0

/**
 * The Android version each runtime permission came in. Before it, what it
 * guards needed no asking (notifications: the user's switch for the app),
 * so an app asking for it on older Android has it: the system would refuse
 * a permission it doesn't know.
 */
private val since = mapOf(
    "android.permission.POST_NOTIFICATIONS" to 33,
    "android.permission.NEARBY_WIFI_DEVICES" to 33,
    "android.permission.BODY_SENSORS_BACKGROUND" to 33,
    "android.permission.READ_MEDIA_IMAGES" to 33,
    "android.permission.READ_MEDIA_VIDEO" to 33,
    "android.permission.READ_MEDIA_AUDIO" to 33,
    "android.permission.READ_MEDIA_VISUAL_USER_SELECTED" to 34,
    "android.permission.BLUETOOTH_SCAN" to 31,
    "android.permission.BLUETOOTH_CONNECT" to 31,
    "android.permission.BLUETOOTH_ADVERTISE" to 31,
    "android.permission.UWB_RANGING" to 31,
    "android.permission.ACTIVITY_RECOGNITION" to 29,
    "android.permission.ACCESS_BACKGROUND_LOCATION" to 29,
    "android.permission.ACCESS_MEDIA_LOCATION" to 29,
    "android.permission.ANSWER_PHONE_CALLS" to 26,
    "android.permission.READ_PHONE_NUMBERS" to 26,
)

/** Whether the app has permission, counting one newer than this Android as had (see since). */
fun AsukaRuntime.granted(permission: String): Boolean {
    val n = since[permission]
    if (n != null && Build.VERSION.SDK_INT < n) {
        return permission != "android.permission.POST_NOTIFICATIONS" || NotificationManagerCompat.from(app).areNotificationsEnabled()
    }
    return ContextCompat.checkSelfPermission(app, permission) == PackageManager.PERMISSION_GRANTED
}

/** Asks for the permissions (those not granted yet), then calls done with whether any of them is granted. Main thread. */
fun AsukaRuntime.withPermission(permissions: Array<String>, done: (Boolean) -> Unit) {
    if (permissions.any { granted(it) }) { done(true); return }
    // Those this Android knows (one it doesn't, before its version, was
    // answered above: notifications switched off stay off, as asking can't
    // turn them on there).
    val asked = permissions.filter { (since[it] ?: 0) <= Build.VERSION.SDK_INT }.toTypedArray()
    if (asked.isEmpty()) { done(false); return }
    val act = activity ?: run { done(false); return }
    var launcher: androidx.activity.result.ActivityResultLauncher<Array<String>>? = null
    launcher = act.activityResultRegistry.register("asuka-permission-${asks++}", ActivityResultContracts.RequestMultiplePermissions()) { granted ->
        launcher?.unregister()
        done(granted.values.any { it })
    }
    launcher.launch(asked)
}

/** Whether the app's manifest asks for permission (asuka.toml's, or a package's). */
fun AsukaRuntime.declares(permission: String): Boolean = try {
    app.packageManager.getPackageInfo(app.packageName, android.content.pm.PackageManager.GET_PERMISSIONS)
        .requestedPermissions?.contains(permission) == true
} catch (_: Exception) { false }
