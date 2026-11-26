// platform/health on Android: Health Connect (androidx.health.connect),
// the system's from Android 14, an app the user installs from Google Play
// on Android 9 to 13. Its client needs Android 8 (the module overrides its
// minSdk): below, and where Health Connect isn't installed or needs an
// update, available() is false and the package throws notAvailable.
//
// Each Kind is a record type (kinds): its permissions are Health
// Connect's, which the app's manifest must declare (asuka.toml's [health]
// read and write: Google Play asks the app to justify each), and Health
// Connect's permission screen links to the app's privacy policy
// (HealthRationaleActivity). A series (heart rate) gives a sample per
// measurement; a session (sleep, mindfulness) one of value 1.
package platform.health

import android.content.ActivityNotFoundException
import android.content.pm.PackageManager
import android.os.Build
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.annotation.RequiresApi
import androidx.health.connect.client.HealthConnectClient
import androidx.health.connect.client.HealthConnectFeatures
import androidx.health.connect.client.PermissionController
import androidx.health.connect.client.aggregate.AggregateMetric
import androidx.health.connect.client.feature.ExperimentalMindfulnessSessionApi
import androidx.health.connect.client.permission.HealthPermission
import androidx.health.connect.client.records.ActiveCaloriesBurnedRecord
import androidx.health.connect.client.records.BodyFatRecord
import androidx.health.connect.client.records.DistanceRecord
import androidx.health.connect.client.records.FloorsClimbedRecord
import androidx.health.connect.client.records.HeartRateRecord
import androidx.health.connect.client.records.HeightRecord
import androidx.health.connect.client.records.HydrationRecord
import androidx.health.connect.client.records.MindfulnessSessionRecord
import androidx.health.connect.client.records.OxygenSaturationRecord
import androidx.health.connect.client.records.Record
import androidx.health.connect.client.records.RespiratoryRateRecord
import androidx.health.connect.client.records.RestingHeartRateRecord
import androidx.health.connect.client.records.SleepSessionRecord
import androidx.health.connect.client.records.StepsRecord
import androidx.health.connect.client.records.WeightRecord
import androidx.health.connect.client.records.metadata.Metadata
import androidx.health.connect.client.request.AggregateRequest
import androidx.health.connect.client.request.ReadRecordsRequest
import androidx.health.connect.client.time.TimeRangeFilter
import androidx.health.connect.client.units.Energy
import androidx.health.connect.client.units.Length
import androidx.health.connect.client.units.Mass
import androidx.health.connect.client.units.Percentage
import androidx.health.connect.client.units.Volume
import dev.asuka.AsukaModules
import java.time.Instant
import java.time.ZoneId
import java.time.ZoneOffset
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.math.roundToLong
import kotlin.reflect.KClass
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext

/** The store: Health Connect's from Android 8, none before (its client needs 8). */
fun makeStore(): Store = if (Build.VERSION.SDK_INT >= 26) HealthConnectStore() else NoStore

/** No Health Connect (Android 7): nothing to ask. */
private object NoStore : Store {
    override fun available() = false
    override suspend fun request(read: List<String>, write: List<String>): Boolean = throw HealthException(HealthError.NOT_AVAILABLE)
    override suspend fun samples(kind: String, from: Long, to: Long, limit: Long): List<NativeSample> = throw HealthException(HealthError.NOT_AVAILABLE)
    override suspend fun total(kind: String, from: Long, to: Long): Double = throw HealthException(HealthError.NOT_AVAILABLE)
    override suspend fun write(kind: String, value: Double, from: Long, to: Long) = throw HealthException(HealthError.NOT_AVAILABLE)
}

/** The most samples samples gives when asked for all of them. */
private const val ALL = 100_000

/** Health Connect's most records a page. */
private const val PAGE = 5000

/** A Kind: its record type, and its sum where it adds up (total). */
private class Kind(val type: KClass<out Record>, val total: AggregateMetric<*>? = null, val interval: Boolean = false)

/** The Kinds by name, each a record type. */
@OptIn(ExperimentalMindfulnessSessionApi::class)
private val kinds = mapOf(
    "steps" to Kind(StepsRecord::class, StepsRecord.COUNT_TOTAL, interval = true),
    "distance" to Kind(DistanceRecord::class, DistanceRecord.DISTANCE_TOTAL, interval = true),
    "activeEnergy" to Kind(ActiveCaloriesBurnedRecord::class, ActiveCaloriesBurnedRecord.ACTIVE_CALORIES_TOTAL, interval = true),
    "floors" to Kind(FloorsClimbedRecord::class, FloorsClimbedRecord.FLOORS_CLIMBED_TOTAL, interval = true),
    "heartRate" to Kind(HeartRateRecord::class, interval = true),
    "restingHeartRate" to Kind(RestingHeartRateRecord::class),
    "oxygenSaturation" to Kind(OxygenSaturationRecord::class),
    "respiratoryRate" to Kind(RespiratoryRateRecord::class),
    "weight" to Kind(WeightRecord::class),
    "height" to Kind(HeightRecord::class),
    "bodyFat" to Kind(BodyFatRecord::class),
    "water" to Kind(HydrationRecord::class, HydrationRecord.VOLUME_TOTAL, interval = true),
    "sleep" to Kind(SleepSessionRecord::class, interval = true),
    "mindfulness" to Kind(MindfulnessSessionRecord::class, interval = true),
)

private var asks = 0

@RequiresApi(26)
@OptIn(ExperimentalMindfulnessSessionApi::class)
private class HealthConnectStore : Store {
    private val ctx get() = AsukaModules.context
    private val client by lazy { HealthConnectClient.getOrCreate(ctx) }
    private val labels = java.util.concurrent.ConcurrentHashMap<String, String>()

    override fun available(): Boolean =
        HealthConnectClient.getSdkStatus(ctx) == HealthConnectClient.SDK_AVAILABLE

    /** kind's, or notAvailable for one this Health Connect hasn't (mindfulness before its feature). */
    private fun kind(name: String): Kind {
        val k = kinds[name] ?: throw IllegalArgumentException("health: no kind $name")
        if (k.type == MindfulnessSessionRecord::class &&
            client.features.getFeatureStatus(HealthConnectFeatures.FEATURE_MINDFULNESS_SESSION) != HealthConnectFeatures.FEATURE_STATUS_AVAILABLE
        ) {
            throw HealthException(HealthError.NOT_AVAILABLE)
        }
        return k
    }

    override suspend fun request(read: List<String>, write: List<String>): Boolean {
        val asked = LinkedHashSet<String>()
        for (n in read) asked.add(HealthPermission.getReadPermission(kind(n).type))
        for (n in write) asked.add(HealthPermission.getWritePermission(kind(n).type))
        if (asked.isEmpty()) return true
        // Health Connect asks only for what the manifest declares (asuka.toml's [health]).
        val missing = asked.filter { !AsukaModules.declares(it) }
        if (missing.isNotEmpty()) {
            android.util.Log.w(
                "asuka",
                "health: the app doesn't declare ${missing.joinToString()}: list their kinds in asuka.toml's [health] read and write",
            )
            throw HealthException(HealthError.NOT_ALLOWED)
        }
        if (guarded { client.permissionController.getGrantedPermissions() }.containsAll(asked)) return true
        return withContext(Dispatchers.Main) {
            suspendCancellableCoroutine { c ->
                val act = AsukaModules.activity as? ComponentActivity
                if (act == null) {
                    c.resumeWithException(IllegalStateException("health: no screen to ask on"))
                    return@suspendCancellableCoroutine
                }
                var l: ActivityResultLauncher<Set<String>>? = null
                l = act.activityResultRegistry.register("asuka-health-${asks++}", PermissionController.createRequestPermissionResultContract()) { _ ->
                    l?.unregister()
                    if (c.isActive) c.resume(true)
                }
                try {
                    l.launch(asked)
                } catch (e: ActivityNotFoundException) {
                    l.unregister()
                    c.resumeWithException(HealthException(HealthError.NOT_AVAILABLE))
                }
            }
        }
    }

    override suspend fun samples(kind: String, from: Long, to: Long, limit: Long): List<NativeSample> {
        val k = kind(kind)
        val max = if (limit > 0) minOf(limit, ALL.toLong()).toInt() else ALL
        val range = TimeRangeFilter.between(Instant.ofEpochMilli(from), Instant.ofEpochMilli(to))
        val out = ArrayList<NativeSample>()
        var token: String? = null
        do {
            val req = ReadRecordsRequest(k.type, range, ascendingOrder = false, pageSize = minOf(PAGE, max - out.size), pageToken = token)
            val res = guarded { client.readRecords(req) }
            for (r in res.records) {
                if (out.size >= max) break
                add(out, r, max)
            }
            token = res.pageToken
        } while (token != null && out.size < max)
        return out
    }

    /** r's samples, newest first, into out (up to max). */
    private fun add(out: MutableList<NativeSample>, r: Record, max: Int) {
        val source = source(r.metadata)
        fun at(v: Double, t: Instant) = out.add(NativeSample(v, t.toEpochMilli(), t.toEpochMilli(), source))
        fun over(v: Double, s: Instant, e: Instant) = out.add(NativeSample(v, s.toEpochMilli(), e.toEpochMilli(), source))
        when (r) {
            is StepsRecord -> over(r.count.toDouble(), r.startTime, r.endTime)
            is DistanceRecord -> over(r.distance.inMeters, r.startTime, r.endTime)
            is ActiveCaloriesBurnedRecord -> over(r.energy.inKilocalories, r.startTime, r.endTime)
            is FloorsClimbedRecord -> over(r.floors, r.startTime, r.endTime)
            is HeartRateRecord -> for (s in r.samples.sortedByDescending { it.time }) {
                if (out.size >= max) break
                at(s.beatsPerMinute.toDouble(), s.time)
            }
            is RestingHeartRateRecord -> at(r.beatsPerMinute.toDouble(), r.time)
            is OxygenSaturationRecord -> at(r.percentage.value / 100, r.time)
            is RespiratoryRateRecord -> at(r.rate, r.time)
            is WeightRecord -> at(r.weight.inKilograms, r.time)
            is HeightRecord -> at(r.height.inMeters, r.time)
            is BodyFatRecord -> at(r.percentage.value / 100, r.time)
            is HydrationRecord -> over(r.volume.inLiters, r.startTime, r.endTime)
            is SleepSessionRecord -> over(1.0, r.startTime, r.endTime)
            is MindfulnessSessionRecord -> over(1.0, r.startTime, r.endTime)
            else -> {}
        }
    }

    /** Who wrote a record: its app's name where the app is visible (an app of Health Connect's), else its device's model, else its package. */
    private fun source(m: Metadata): String {
        val pkg = m.dataOrigin.packageName
        labels[pkg]?.let { return it }
        val label = try {
            val pm = ctx.packageManager
            pm.getApplicationLabel(pm.getApplicationInfo(pkg, 0)).toString()
        } catch (_: PackageManager.NameNotFoundException) {
            null
        }
        if (label != null) {
            labels[pkg] = label
            return label
        }
        return m.device?.model ?: pkg
    }

    override suspend fun total(kind: String, from: Long, to: Long): Double {
        val metric = kind(kind).total
            ?: throw IllegalArgumentException("health: $kind doesn't add up: total takes steps, distance, activeEnergy, floors or water")
        val range = TimeRangeFilter.between(Instant.ofEpochMilli(from), Instant.ofEpochMilli(to))
        val res = guarded { client.aggregate(AggregateRequest(setOf(metric), range)) }
        return when (val v = res[metric]) {
            null -> 0.0
            is Long -> v.toDouble()
            is Double -> v
            is Length -> v.inMeters
            is Energy -> v.inKilocalories
            is Volume -> v.inLiters
            else -> throw IllegalStateException("health: $kind's total is a ${v::class.simpleName}")
        }
    }

    override suspend fun write(kind: String, value: Double, from: Long, to: Long) {
        val k = kind(kind)
        val s = Instant.ofEpochMilli(from)
        // A period of a kind that has one ends after it starts (HealthKit
        // takes an instant: so does this, as a millisecond).
        val e = Instant.ofEpochMilli(if (k.interval && to <= from) from + 1 else to)
        val so = offset(s)
        val eo = offset(e)
        val m = Metadata.manualEntry()
        val r: Record = when (k.type) {
            StepsRecord::class -> StepsRecord(s, so, e, eo, value.roundToLong(), m)
            DistanceRecord::class -> DistanceRecord(s, so, e, eo, Length.meters(value), m)
            ActiveCaloriesBurnedRecord::class -> ActiveCaloriesBurnedRecord(s, so, e, eo, Energy.kilocalories(value), m)
            FloorsClimbedRecord::class -> FloorsClimbedRecord(s, so, e, eo, value, m)
            HeartRateRecord::class -> HeartRateRecord(s, so, e, eo, listOf(HeartRateRecord.Sample(s, value.roundToLong())), m)
            RestingHeartRateRecord::class -> RestingHeartRateRecord(s, so, value.roundToLong(), m)
            OxygenSaturationRecord::class -> OxygenSaturationRecord(s, so, Percentage(value * 100), m)
            RespiratoryRateRecord::class -> RespiratoryRateRecord(s, so, value, m)
            WeightRecord::class -> WeightRecord(s, so, Mass.kilograms(value), m)
            HeightRecord::class -> HeightRecord(s, so, Length.meters(value), m)
            BodyFatRecord::class -> BodyFatRecord(s, so, Percentage(value * 100), m)
            HydrationRecord::class -> HydrationRecord(s, so, e, eo, Volume.liters(value), m)
            SleepSessionRecord::class -> SleepSessionRecord(s, so, e, eo, m)
            MindfulnessSessionRecord::class -> MindfulnessSessionRecord(s, so, e, eo, m, MindfulnessSessionRecord.MINDFULNESS_SESSION_TYPE_UNKNOWN)
            else -> throw IllegalArgumentException("health: can't write $kind")
        }
        guarded { client.insertRecords(listOf(r)) }
    }

    /** The system's time zone's offset at t, which Health Connect keeps with each record. */
    private fun offset(t: Instant): ZoneOffset = ZoneId.systemDefault().rules.getOffset(t)

    /** A client call: a permission it lacks is notAllowed. */
    private suspend fun <T> guarded(call: suspend () -> T): T = try {
        call()
    } catch (e: SecurityException) {
        throw HealthException(HealthError.NOT_ALLOWED)
    }
}
