// platform/power on Android: the battery is the sticky
// ACTION_BATTERY_CHANGED; low power Battery Saver; thermal PowerManager's
// status. The screen locks as it goes off on a secure lock screen, and
// unlocks at ACTION_USER_PRESENT. A phone doesn't suspend apps as a
// computer sleeps (no suspend or resume), nor say the idle time.
package platform.power

import android.app.KeyguardManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.os.BatteryManager
import android.os.Build
import android.os.PowerManager
import androidx.core.content.ContextCompat
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.withContext

class AndroidPower(private val app: Context) : Monitor {
    private val pm = app.getSystemService(Context.POWER_SERVICE) as PowerManager
    private val km = app.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager

    override fun current(): Status = status(app.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)))

    private fun status(b: Intent?): Status {
        val present = b?.getBooleanExtra(BatteryManager.EXTRA_PRESENT, false) == true
        val level = b?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = b?.getIntExtra(BatteryManager.EXTRA_SCALE, -1) ?: -1
        val st = b?.getIntExtra(BatteryManager.EXTRA_STATUS, -1) ?: -1
        val plugged = (b?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        // Before Android 10 the system doesn't say: nominal.
        val heat = if (Build.VERSION.SDK_INT < 29) Thermal.NOMINAL else when (pm.currentThermalStatus) {
            PowerManager.THERMAL_STATUS_NONE, PowerManager.THERMAL_STATUS_LIGHT -> Thermal.NOMINAL
            PowerManager.THERMAL_STATUS_MODERATE -> Thermal.FAIR
            PowerManager.THERMAL_STATUS_SEVERE -> Thermal.SERIOUS
            else -> Thermal.CRITICAL
        }
        return Status(
            battery = present,
            onBattery = present && !plugged,
            level = if (present && level >= 0 && scale > 0) (level.toDouble() / scale).coerceIn(0.0, 1.0) else -1.0,
            charging = present && st == BatteryManager.BATTERY_STATUS_CHARGING,
            lowPower = pm.isPowerSaveMode,
            thermal = heat,
        )
    }

    override suspend fun watch(changed: (Status) -> kotlin.Unit) = withContext(Dispatchers.Main) {
        var last: Status? = null
        var battery: Intent? = null
        val report = {
            val s = status(battery ?: app.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED)))
            if (s != last) {
                last = s
                changed(s)
            }
        }
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                if (i.action == Intent.ACTION_BATTERY_CHANGED) battery = i
                report()
            }
        }
        val f = IntentFilter().apply {
            addAction(Intent.ACTION_BATTERY_CHANGED)
            addAction(PowerManager.ACTION_POWER_SAVE_MODE_CHANGED)
        }
        battery = ContextCompat.registerReceiver(app, receiver, f, ContextCompat.RECEIVER_NOT_EXPORTED)
        // Before Android 10 there's no thermal status to watch.
        val unwatch: () -> kotlin.Unit = if (Build.VERSION.SDK_INT >= 29) {
            val thermal = PowerManager.OnThermalStatusChangedListener { report() }
            pm.addThermalStatusListener(ContextCompat.getMainExecutor(app), thermal)
            ({ pm.removeThermalStatusListener(thermal) })
        } else ({})
        report()
        try {
            awaitCancellation()
        } finally {
            app.unregisterReceiver(receiver)
            unwatch()
        }
    }

    override suspend fun events(happened: (Event) -> kotlin.Unit) = withContext(Dispatchers.Main) {
        val receiver = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                when (i.action) {
                    Intent.ACTION_SCREEN_OFF -> if (km.isKeyguardSecure) happened(Event.LOCK)
                    Intent.ACTION_USER_PRESENT -> happened(Event.UNLOCK)
                }
            }
        }
        val f = IntentFilter().apply {
            addAction(Intent.ACTION_SCREEN_OFF)
            addAction(Intent.ACTION_USER_PRESENT)
        }
        ContextCompat.registerReceiver(app, receiver, f, ContextCompat.RECEIVER_NOT_EXPORTED)
        try {
            awaitCancellation()
        } finally {
            app.unregisterReceiver(receiver)
        }
    }

    override suspend fun idle(): kotlin.time.Duration = throw PowerException(PowerError.IDLE_UNKNOWN)
}

fun makeMonitor(): Monitor = AndroidPower(AsukaModules.context)
