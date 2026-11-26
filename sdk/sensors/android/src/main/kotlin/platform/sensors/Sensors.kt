// platform/sensors on Android: SensorManager. Each stream registers its own
// listeners at the interval it asks for (SensorManager runs a sensor at
// the fastest its listeners ask), on the main looper. Accelerations in g;
// device motion is gravity, linear acceleration, rotation rate and the
// rotation vector's quaternion (x, y, z, w), from which the package's
// Asuka computes the attitude. Android keeps no step history.
package platform.sensors

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.Sensor as HwSensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Handler
import android.os.Looper
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlin.coroutines.resume
import kotlin.math.sqrt
import kotlin.time.Duration.Companion.nanoseconds

private const val G = SensorManager.GRAVITY_EARTH.toDouble()

class AndroidSensors(private val context: Context) : Sensors {
    private val sm get() = context.getSystemService(Context.SENSOR_SERVICE) as SensorManager

    /** The platform sensors each kind reads. */
    private fun types(s: Sensor): List<Int> = when (s) {
        Sensor.ACCELEROMETER -> listOf(HwSensor.TYPE_ACCELEROMETER)
        Sensor.GYROSCOPE -> listOf(HwSensor.TYPE_GYROSCOPE)
        Sensor.MAGNETOMETER -> listOf(HwSensor.TYPE_MAGNETIC_FIELD)
        Sensor.MOTION -> listOf(HwSensor.TYPE_ROTATION_VECTOR, HwSensor.TYPE_GRAVITY, HwSensor.TYPE_LINEAR_ACCELERATION, HwSensor.TYPE_GYROSCOPE)
        Sensor.BAROMETER -> listOf(HwSensor.TYPE_PRESSURE)
        Sensor.LIGHT -> listOf(HwSensor.TYPE_LIGHT)
        Sensor.PEDOMETER -> listOf(HwSensor.TYPE_STEP_COUNTER)
        else -> emptyList()
    }

    override fun available(sensor: Sensor): Boolean {
        val ts = types(sensor)
        return ts.isNotEmpty() && ts.all { sm.getDefaultSensor(it) != null }
    }

    override suspend fun watch(sensor: Sensor, every: kotlin.time.Duration, reading: (Sample) -> Unit) {
        if (!available(sensor)) throw SensorException(SensorError.NOT_AVAILABLE)
        if (sensor == Sensor.PEDOMETER && !permitted(Manifest.permission.ACTIVITY_RECOGNITION)) {
            throw SensorException(SensorError.NOT_ALLOWED)
        }
        val micros = maxOf(every.inWholeMicroseconds, 5000L).toInt()
        val main = Handler(Looper.getMainLooper())
        val ls = ArrayList<SensorEventListener>()
        // Each stream's samples no closer than its interval.
        var last = -1L
        val everyNs = every.inWholeNanoseconds * 9 / 10
        fun send(nanos: Long, v: DoubleArray) {
            if (last >= 0 && nanos - last < everyNs) return
            last = nanos
            reading(Sample(v.toList(), nanos.nanoseconds))
        }
        fun on(type: Int, f: (SensorEvent) -> Unit) {
            val l = object : SensorEventListener {
                override fun onSensorChanged(e: SensorEvent) = f(e)
                override fun onAccuracyChanged(s: HwSensor, accuracy: Int) {}
            }
            sm.registerListener(l, sm.getDefaultSensor(type), micros, main)
            ls.add(l)
        }
        withContext(Dispatchers.Main) {
            when (sensor) {
                Sensor.ACCELEROMETER -> on(HwSensor.TYPE_ACCELEROMETER) { e -> send(e.timestamp, doubleArrayOf(e.values[0] / G, e.values[1] / G, e.values[2] / G)) }
                Sensor.GYROSCOPE, Sensor.MAGNETOMETER -> on(types(sensor)[0]) { e ->
                    send(e.timestamp, doubleArrayOf(e.values[0].toDouble(), e.values[1].toDouble(), e.values[2].toDouble()))
                }
                Sensor.MOTION -> {
                    // The latest of each, sent with each rotation vector.
                    val gravity = DoubleArray(3)
                    val linear = DoubleArray(3)
                    val gyro = DoubleArray(3)
                    on(HwSensor.TYPE_GRAVITY) { e -> for (i in 0..2) gravity[i] = e.values[i] / G }
                    on(HwSensor.TYPE_LINEAR_ACCELERATION) { e -> for (i in 0..2) linear[i] = e.values[i] / G }
                    on(HwSensor.TYPE_GYROSCOPE) { e -> for (i in 0..2) gyro[i] = e.values[i].toDouble() }
                    on(HwSensor.TYPE_ROTATION_VECTOR) { e ->
                        val x = e.values[0].toDouble()
                        val y = e.values[1].toDouble()
                        val z = e.values[2].toDouble()
                        // w: the 4th value where the sensor gives it, else from the unit length.
                        val w = if (e.values.size > 3) e.values[3].toDouble() else sqrt(maxOf(0.0, 1 - x * x - y * y - z * z))
                        send(e.timestamp, gravity + linear + gyro + doubleArrayOf(x, y, z, w))
                    }
                }
                // The altitude: the package's, from the pressure.
                Sensor.BAROMETER -> on(HwSensor.TYPE_PRESSURE) { e -> send(e.timestamp, doubleArrayOf(e.values[0].toDouble())) }
                Sensor.LIGHT -> on(HwSensor.TYPE_LIGHT) { e -> send(e.timestamp, doubleArrayOf(e.values[0].toDouble())) }
                Sensor.PEDOMETER -> {
                    // The counter counts since boot: steps since the first reading.
                    var base = -1.0
                    on(HwSensor.TYPE_STEP_COUNTER) { e ->
                        val n = e.values[0].toDouble()
                        if (base < 0) base = n
                        last = -1
                        send(e.timestamp, doubleArrayOf(n - base, -1.0))
                    }
                }
            }
        }
        try {
            awaitCancellation()
        } finally {
            main.post { for (l in ls) sm.unregisterListener(l) }
        }
    }

    override suspend fun steps(from: Long, to: Long): Steps = throw SensorException(SensorError.NOT_AVAILABLE)

    override suspend fun simulate(json: String) = throw dev.asuka.AsukaUnsupportedException("only on the iOS simulator")

    /** Asks for a permission not granted yet: whether it is. */
    private suspend fun permitted(permission: String): Boolean = withContext(Dispatchers.Main) {
        if (AsukaModules.granted(permission)) return@withContext true
        val act = AsukaModules.activity as? ComponentActivity ?: return@withContext false
        suspendCancellableCoroutine { c ->
            var launcher: ActivityResultLauncher<String>? = null
            launcher = act.activityResultRegistry.register("asuka-sensors-${asks++}", ActivityResultContracts.RequestPermission()) { ok ->
                launcher?.unregister()
                if (c.isActive) c.resume(ok)
            }
            launcher.launch(permission)
        }
    }

    private companion object {
        var asks = 0
    }
}

fun makeSensors(): Sensors = AndroidSensors(AsukaModules.context)
