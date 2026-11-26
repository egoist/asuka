// platform/record on Android: MediaRecorder, AAC in MPEG-4. With
// background, a foreground service of the microphone type keeps it going
// out of sight, its notification with a button to stop. On the main
// thread. Video is platform/camera's.
package platform.record

import android.Manifest
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.SystemClock
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.File
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds

private const val CHANNEL = "asuka.recording"
private const val NOTIFICATION = 0x6e76_0001

/**
 * Whether the app has asked for a permission: Android says only whether
 * it's granted, so each ask is kept in the app's preferences, by the
 * permission's name (platform/camera's file too: both ask for the
 * microphone).
 */
private val asked get() = AsukaModules.context.getSharedPreferences("asuka.permissions", Context.MODE_PRIVATE)

private fun isGranted(permission: String) =
    ContextCompat.checkSelfPermission(AsukaModules.context, permission) == PackageManager.PERMISSION_GRANTED

/** Asks for a permission (if not granted), noting the ask when there's a screen to ask on. Main thread. */
private fun requestPermission(permission: String, done: (Boolean) -> kotlin.Unit) {
    val shown = AsukaModules.activity != null
    AsukaModules.requestPermissions(arrayOf(permission)) { ok ->
        if (shown) asked.edit { putBoolean(permission, true) }
        done(ok)
    }
}

/**
 * A permission now, asking nothing: "allowed"; "denied" once asked (the
 * user said no, now or for good); "notAsked" before.
 */
private fun permissionAccess(permission: String): String {
    val a = asked
    if (isGranted(permission)) {
        // Granted (here, or in Settings): taken away later, it's denied.
        if (!a.getBoolean(permission, false)) a.edit { putBoolean(permission, true) }
        return "allowed"
    }
    return if (a.getBoolean(permission, false)) "denied" else "notAsked"
}

internal object AudioRecorder : Recorder {
    private val app get() = AsukaModules.context
    private var recorder: MediaRecorder? = null
    private var file: File? = null
    private var done: ((Result<String>) -> kotlin.Unit)? = null
    private var kept = 0L // milliseconds before the last start or resume
    private var since = 0L // when it last started or resumed; 0: paused
    private var service = false
    var title = "Recording"

    override suspend fun record(quality: Quality, maxDuration: Duration, path: String, background: Boolean, title: String): String =
        withContext(Dispatchers.Main) {
            if (recorder != null) throw RecordException(RecordError.BUSY)
            val ok = suspendCancellableCoroutine { c -> requestPermission(Manifest.permission.RECORD_AUDIO) { if (c.isActive) c.resume(it) } }
            if (!ok) throw RecordException(RecordError.NOT_ALLOWED)
            if (recorder != null) throw RecordException(RecordError.BUSY)
            this@AudioRecorder.title = title.ifEmpty { "Recording" }
            suspendCancellableCoroutine { c ->
                start(quality == Quality.MUSIC, maxDuration.inWholeMilliseconds.toInt(), background, path) { r ->
                    if (c.isActive) r.fold({ c.resume(it) }, { c.resumeWithException(it) })
                }
                // The task that waits went: the recording ends.
                c.invokeOnCancellation { Handler(Looper.getMainLooper()).post { finish(null) } }
            }
        }

    private fun start(music: Boolean, maxMillis: Int, background: Boolean, path: String, reply: (Result<String>) -> kotlin.Unit) {
        val dir = File(app.cacheDir, "asuka-audio").apply { mkdirs() }
        val f = if (path.isNotEmpty()) File(path) else File(dir, java.util.UUID.randomUUID().toString() + ".m4a")
        // While the app is on screen: Android doesn't let one start later.
        if (background) runCatching { startService() }
        // Before Android 12 a recorder isn't given the app's context.
        val r = if (Build.VERSION.SDK_INT >= 31) MediaRecorder(app) else @Suppress("DEPRECATION") MediaRecorder()
        try {
            r.setAudioSource(MediaRecorder.AudioSource.MIC)
            r.setOutputFormat(MediaRecorder.OutputFormat.MPEG_4)
            r.setAudioEncoder(MediaRecorder.AudioEncoder.AAC)
            r.setAudioSamplingRate(if (music) 44100 else 22050)
            r.setAudioChannels(if (music) 2 else 1)
            r.setAudioEncodingBitRate(if (music) 128_000 else 64_000)
            if (maxMillis > 0) r.setMaxDuration(maxMillis)
            r.setOutputFile(f.absolutePath)
            r.setOnInfoListener { _, what, _ ->
                if (what == MediaRecorder.MEDIA_RECORDER_INFO_MAX_DURATION_REACHED) Handler(Looper.getMainLooper()).post { finish(null) }
            }
            r.setOnErrorListener { _, what, _ -> Handler(Looper.getMainLooper()).post { finish(Exception("the recording failed ($what)")) } }
            r.prepare()
            r.start()
        } catch (e: Exception) {
            r.release()
            stopService()
            reply(Result.failure(e))
            return
        }
        kept = 0
        since = SystemClock.elapsedRealtime()
        recorder = r
        file = f
        done = reply
    }

    private fun startService() {
        val nm = app.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26 && nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, "Recording", NotificationManager.IMPORTANCE_LOW))
        }
        // Its notification shows (the system asks first); recording goes on either way.
        AsukaModules.requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS)) {}
        ContextCompat.startForegroundService(app, Intent(app, RecordService::class.java))
        service = true
    }

    private fun stopService() {
        if (service) app.stopService(Intent(app, RecordService::class.java))
        service = false
    }

    /** Stops the recording (if any) and answers record: the file, or err. */
    fun finish(err: Exception?) {
        val r = recorder ?: return
        recorder = null
        since = 0
        stopService()
        val stopped = runCatching { r.stop() }.isSuccess // fails if nothing was recorded
        r.release()
        val d = done
        done = null
        val f = file
        file = null
        when {
            err != null -> d?.invoke(Result.failure(err))
            !stopped || f == null -> {
                f?.delete()
                d?.invoke(Result.failure(Exception("nothing was recorded")))
            }
            else -> d?.invoke(Result.success(f.absolutePath))
        }
    }

    override fun stop() = finish(null)

    override fun pause() {
        runCatching { recorder?.pause() }
        if (since != 0L) kept += SystemClock.elapsedRealtime() - since
        since = 0
    }

    override fun resume() {
        runCatching { recorder?.resume() }
        if (recorder != null && since == 0L) since = SystemClock.elapsedRealtime()
    }

    override fun access(): String = permissionAccess(Manifest.permission.RECORD_AUDIO)

    override suspend fun requestAccess(): String = withContext(Dispatchers.Main) {
        val p = Manifest.permission.RECORD_AUDIO
        if (!isGranted(p)) suspendCancellableCoroutine { c -> requestPermission(p) { if (c.isActive) c.resume(kotlin.Unit) } }
        permissionAccess(p)
    }

    override fun status(): Status {
        val r = recorder ?: return Status()
        // The loudest sample since the last call, of 32767.
        val amp = if (since == 0L) 0 else runCatching { r.maxAmplitude }.getOrDefault(0)
        return Status(
            level = minOf(amp, 32767) / 32767.0,
            recording = since != 0L,
            elapsed = (kept + if (since != 0L) SystemClock.elapsedRealtime() - since else 0L).milliseconds,
        )
    }
}

/** The foreground service that keeps a recording going out of sight. */
class RecordService : Service() {
    override fun onBind(intent: Intent?): IBinder? = null

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == "asuka.record.stop") {
            AudioRecorder.finish(null)
            return START_NOT_STICKY
        }
        val open = packageManager.getLaunchIntentForPackage(packageName)
        val stop = PendingIntent.getService(this, 0, Intent(this, RecordService::class.java).setAction("asuka.record.stop"),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        val b = NotificationCompat.Builder(this, CHANNEL)
            .setSmallIcon(applicationInfo.icon)
            .setContentTitle(AudioRecorder.title)
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_LOW) // before Android 8, which has no channels
            .setUsesChronometer(true)
            .setForegroundServiceBehavior(NotificationCompat.FOREGROUND_SERVICE_IMMEDIATE)
            .setCategory(NotificationCompat.CATEGORY_SERVICE)
            .addAction(0, "Stop", stop)
        if (open != null) {
            b.setContentIntent(PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        }
        ServiceCompat.startForeground(this, NOTIFICATION, b.build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE)
        return START_NOT_STICKY
    }
}

fun makeRecorder(): Recorder = AudioRecorder
