// Background.kt: std/http's background uploads on Android (upload's
// background), sent by WorkManager, which goes on while the app is out
// of sight and after the system ends it (it starts the app's process
// again to run the work).
//
// Each upload is a directory of the app's own (noBackupFilesDir's
// asuka-http/uploads/<work id>): its body, one file (a file part linked
// or copied there, the other parts written after it: the worker may run
// in a process that has no parts), its request (request.json: the
// method, URL, headers, length, timeout and the transport's TLS
// settings), then its answer (result.json, the body result.body) once
// the worker has one. The work (UploadWorker, tagged asuka.http.upload
// and asuka.http.upload:<id>) needs a network, runs in the foreground
// (WorkManager's SystemForegroundService, a dataSync one, which the
// package's asuka.toml declares for the apps that refer to
// http.Background) with its progress in a notification (else as plain
// work), and retries what the network failed, a few times. The call
// that enqueued it waits for its WorkInfo, its progress the worker's,
// and takes the answer from the directory (then gone); cancelled, it
// cancels the work. An answer no call in this
// process waits for is the app's next finished(), given once.
package std.http

import android.Manifest
import android.annotation.SuppressLint
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.ComponentName
import android.content.Context
import android.content.pm.PackageManager
import android.content.pm.ServiceInfo
import android.os.Build
import androidx.core.app.NotificationCompat
import androidx.work.BackoffPolicy
import androidx.work.Constraints
import androidx.work.CoroutineWorker
import androidx.work.ForegroundInfo
import androidx.work.NetworkType
import androidx.work.OneTimeWorkRequestBuilder
import androidx.work.WorkInfo
import androidx.work.WorkManager
import androidx.work.WorkerParameters
import androidx.work.await
import androidx.work.workDataOf
import dev.asuka.AsukaModules
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.io.FileInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL
import java.util.UUID
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.TimeUnit
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds

// A background upload's request, as its worker reads it.
internal data class UploadRequest(
    val id: String,
    val method: String,
    val url: String,
    val headers: Map<String, String>,
    val length: Long,
    val timeoutMs: Long,
    val tls: TLS? = null,
)

// What a background upload ended with: the server's answer, or what
// went wrong (error, status 0).
internal data class UploadResult(
    val status: Long = 0,
    val url: String = "",
    val headers: Map<String, String> = emptyMap(),
    val error: String = "",
    val body: ByteArray = ByteArray(0),
)

// The uploads' directories under root, one per upload (its key, the
// work's id). Plain files, so it's the same in any process: what one
// writes it writes beside and renames.
internal class UploadStore(val root: File) {
    fun dir(key: String) = File(root, key)
    fun body(key: String) = File(dir(key), "body")

    fun keys(): List<String> = root.listFiles()?.filter { it.isDirectory }?.map { it.name }?.sorted() ?: emptyList()

    // The upload's body, parts in order, as one file: a file alone is
    // linked there (no copy, when it's on the same file system), else
    // each part's bytes are copied in, 64 KiB at a time. Its length.
    // check throws to stop it (the call cancelled).
    fun spool(key: String, parts: List<Part>, check: () -> kotlin.Unit = {}): Long {
        val d = dir(key)
        if (!d.isDirectory && !d.mkdirs()) throw IOException("upload: can't make ${d.path}")
        val to = body(key)
        to.delete()
        val only = parts.singleOrNull()
        if (only != null && only.path.isNotEmpty()) {
            val from = File(only.path)
            if (!from.isFile) throw java.io.FileNotFoundException("upload: no file at ${only.path}")
            if (link(from, to)) return to.length()
        }
        val buf = ByteArray(PIECE)
        FileOutputStream(to).use { out ->
            for (p in parts) {
                check()
                if (p.path.isNotEmpty()) {
                    FileInputStream(File(p.path)).use { input ->
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            check()
                        }
                    }
                } else {
                    out.write(p.data)
                }
            }
            out.fd.sync()
        }
        return to.length()
    }

    // A hard link from to to from, where the file system has one (the
    // app's own files: the same one); a link that isn't made (another
    // file system, the JVM's stub of Os) is copied instead.
    private fun link(from: File, to: File): Boolean = try {
        android.system.Os.link(from.absolutePath, to.absolutePath)
        to.isFile && to.length() == from.length()
    } catch (e: Throwable) {
        false
    }

    fun writeRequest(key: String, r: UploadRequest) {
        val o = JSONObject()
            .put("id", r.id).put("method", r.method).put("url", r.url)
            .put("headers", JSONObject(r.headers as Map<*, *>)).put("length", r.length).put("timeout", r.timeoutMs)
        r.tls?.let {
            o.put("tls", JSONObject().put("ca", it.ca).put("trustSystem", it.trustSystem).put("certificate", it.certificate)
                .put("key", it.key).put("insecureSkipVerify", it.insecureSkipVerify))
        }
        write(File(dir(key), "request.json"), o.toString().toByteArray())
    }

    fun request(key: String): UploadRequest? {
        val o = read(File(dir(key), "request.json")) ?: return null
        val tls = o.optJSONObject("tls")?.let {
            TLS(ca = it.optString("ca"), trustSystem = it.optBoolean("trustSystem"), certificate = it.optString("certificate"),
                key = it.optString("key"), insecureSkipVerify = it.optBoolean("insecureSkipVerify"))
        }
        return UploadRequest(id = o.optString("id"), method = o.optString("method", "POST"), url = o.optString("url"),
            headers = strings(o.optJSONObject("headers")), length = o.optLong("length", -1), timeoutMs = o.optLong("timeout"), tls = tls)
    }

    fun writeResult(key: String, r: UploadResult) {
        val d = dir(key)
        if (!d.isDirectory) return  // the upload was let go: its call cancelled
        write(File(d, "result.body"), r.body)
        val o = JSONObject().put("status", r.status).put("url", r.url).put("headers", JSONObject(r.headers as Map<*, *>)).put("error", r.error)
        write(File(d, "result.json"), o.toString().toByteArray())
    }

    fun result(key: String): UploadResult? {
        val o = read(File(dir(key), "result.json")) ?: return null
        val body = File(dir(key), "result.body").takeIf { it.isFile }?.readBytes() ?: ByteArray(0)
        return UploadResult(status = o.optLong("status"), url = o.optString("url"), headers = strings(o.optJSONObject("headers")),
            error = o.optString("error"), body = body)
    }

    // The upload's answer, its files then gone: one taker gets it.
    fun claim(key: String): UploadResult? = synchronized(UploadStore) {
        val r = result(key)
        remove(key)
        r
    }

    fun remove(key: String) {
        dir(key).deleteRecursively()
    }

    // The uploads that ended with no call waiting for them (none of
    // waiting's), each given once (its files then gone), by id; the ones
    // that will never end (gone: its work is no more, the app having
    // died before it was enqueued, or after it was cancelled) removed.
    fun finished(waiting: Set<String>, gone: (String) -> Boolean): List<FinishedUpload> = synchronized(UploadStore) {
        val out = ArrayList<FinishedUpload>()
        for (key in keys()) {
            if (key in waiting) continue
            val r = result(key)
            if (r == null) {
                if (gone(key)) remove(key)
                continue
            }
            val id = request(key)?.id ?: ""
            remove(key)
            out.add(FinishedUpload(id = id, status = r.status, url = r.url, headers = r.headers, error = r.error))
        }
        out
    }

    companion object {
        private fun write(f: File, data: ByteArray) {
            val part = File(f.path + ".part")
            FileOutputStream(part).use {
                it.write(data)
                it.fd.sync()
            }
            if (!part.renameTo(f)) {
                f.delete()
                if (!part.renameTo(f)) throw IOException("upload: can't write ${f.path}")
            }
        }

        private fun read(f: File): JSONObject? = try {
            if (f.isFile) JSONObject(f.readText()) else null
        } catch (e: Exception) {
            null
        }

        private fun strings(o: JSONObject?): Map<String, String> {
            if (o == null) return emptyMap()
            val m = LinkedHashMap<String, String>()
            for (k in o.keys()) m[k] = o.optString(k)
            return m
        }
    }
}

internal object BackgroundUploads {
    const val TAG = "asuka.http.upload"
    // A network failure is tried again this many times in all (each after
    // a wait that doubles from 30 s), then the upload fails.
    const val ATTEMPTS = 5

    // The uploads a call in this process waits for: finished leaves them.
    private val waiting = ConcurrentHashMap.newKeySet<String>()

    // The app's context, when the host has started (not on a plain JVM:
    // there, uploads aren't background ones).
    val context: Context?
        get() = try {
            AsukaModules.context
        } catch (e: Throwable) {
            null
        }

    fun store(ctx: Context) = UploadStore(File(ctx.noBackupFilesDir, "asuka-http/uploads"))

    suspend fun upload(ctx: Context, tls: TLS?, method: String, url: String, headers: Map<String, String>, parts: List<Part>, timeout: Duration, progress: (Long, Long) -> kotlin.Unit, id: String): Response {
        val wm = WorkManager.getInstance(ctx)
        val store = store(ctx)
        val work = UUID.randomUUID()
        val key = work.toString()
        waiting.add(key)
        try {
            val size = withContext(Dispatchers.IO) {
                val n = store.spool(key, parts) { ensureActive() }
                store.writeRequest(key, UploadRequest(id = id, method = method, url = url, headers = headers, length = n,
                    timeoutMs = if (timeout.isPositive()) timeout.inWholeMilliseconds else 0L, tls = tls))
                n
            }
            // Its progress shows in a notification: the app may need to ask.
            if (Build.VERSION.SDK_INT >= 33 && AsukaModules.declares(Manifest.permission.POST_NOTIFICATIONS) &&
                !AsukaModules.granted(Manifest.permission.POST_NOTIFICATIONS)) {
                AsukaModules.requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS)) {}
            }
            val request = OneTimeWorkRequestBuilder<UploadWorker>()
                .setId(work)
                .setConstraints(Constraints.Builder().setRequiredNetworkType(NetworkType.CONNECTED).build())
                .setBackoffCriteria(BackoffPolicy.EXPONENTIAL, 30, TimeUnit.SECONDS)
                .addTag(TAG)
                .addTag("$TAG:$id")
                .build()
            wm.enqueue(request).await()
            var reported = -1L
            val info = wm.getWorkInfoByIdFlow(work).first { info ->
                if (info != null && info.state == WorkInfo.State.RUNNING) {
                    val sent = info.progress.getLong("sent", -1)
                    if (sent >= 0 && sent != reported) {
                        reported = sent
                        progress(sent, info.progress.getLong("total", size))
                    }
                }
                info == null || info.state.isFinished
            }
            return withContext(Dispatchers.IO) {
                val r = store.claim(key)
                when {
                    info?.state == WorkInfo.State.CANCELLED -> throw IOException("upload: the background upload was cancelled")
                    r == null -> throw IOException("upload: the background upload ended without an answer")
                    r.error.isNotEmpty() -> throw IOException(r.error)
                    else -> {
                        progress(size, size)
                        Response(status = r.status, url = r.url, headers = r.headers, body = r.body)
                    }
                }
            }
        } catch (e: Throwable) {
            // Cancelled (or failed before it was enqueued): the work, if
            // any, stops, and its files go.
            withContext(NonCancellable) {
                wm.cancelWorkById(work)
                withContext(Dispatchers.IO) { store.remove(key) }
            }
            throw e
        } finally {
            waiting.remove(key)
        }
    }

    suspend fun finished(ctx: Context): List<FinishedUpload> {
        val wm = WorkManager.getInstance(ctx)
        val store = store(ctx)
        // The works that are no more, or ended: what their directories
        // hold is all there will be.
        val ended = HashSet<String>()
        for (key in withContext(Dispatchers.IO) { store.keys() }) {
            val uuid = try { UUID.fromString(key) } catch (e: IllegalArgumentException) { null }
            val info = uuid?.let { wm.getWorkInfoByIdFlow(it).first() }
            if (info == null || info.state.isFinished) ended.add(key)
        }
        return withContext(Dispatchers.IO) { store.finished(waiting.toSet()) { it in ended } }
    }
}

// Sends a background upload: its body the file its directory holds, on
// a connection made with its transport's TLS settings, in the foreground
// (a dataSync service, its progress a notification) when the system lets
// it start one. An answer of any status is the work's success; a network
// failure is tried again (ATTEMPTS in all), then is its failure. Either
// way its directory has the result, for the call waiting or finished.
class UploadWorker(context: Context, params: WorkerParameters) : CoroutineWorker(context, params) {
    private val key = id.toString()
    private val notification = (key.hashCode() and 0x7fffffff) or 1  // never 0, which isn't one

    override suspend fun getForegroundInfo(): ForegroundInfo = foreground(0, -1)

    override suspend fun doWork(): Result {
        val store = BackgroundUploads.store(applicationContext)
        val req = withContext(Dispatchers.IO) { store.request(key) } ?: return Result.failure()
        val body = store.body(key)
        if (!body.isFile) {
            withContext(Dispatchers.IO) { store.writeResult(key, UploadResult(error = "upload: the background upload's body is gone")) }
            return Result.failure()
        }
        val inForeground = canRunInForeground(applicationContext) && try {
            setForeground(foreground(0, req.length))
            true
        } catch (e: Exception) {
            // Android 12 and later don't let an app out of sight start a
            // foreground service: it goes on as plain work (and if the
            // system stops it, WorkManager runs it again).
            false
        }
        val shown = Throttle({ sent, total -> setForegroundAsync(foreground(sent, total)) }, 1_000_000_000L)
        val progress: (Long, Long) -> kotlin.Unit = { sent, total ->
            setProgressAsync(workDataOf("sent" to sent, "total" to total))
            if (inForeground) shown.report(sent, total)
        }
        return try {
            val policy = req.tls?.let { TLSPolicy.of(it.ca, it.trustSystem, it.certificate, it.key, it.insecureSkipVerify) }
            val c = URL(req.url).openConnection() as HttpURLConnection
            policy?.apply(c)
            val r = streamUpload(c, req.method, req.headers, listOf(Part(path = body.path)), req.length, req.timeoutMs.milliseconds, progress)
            withContext(Dispatchers.IO) {
                store.writeResult(key, UploadResult(status = r.status, url = r.url, headers = r.headers, body = r.body))
                body.delete()
            }
            Result.success()
        } catch (e: CancellationException) {
            throw e
        } catch (e: IOException) {
            if (runAttemptCount + 1 < BackgroundUploads.ATTEMPTS) return Result.retry()
            failed(store, e)
        } catch (e: Exception) {
            failed(store, e)
        }
    }

    private suspend fun failed(store: UploadStore, e: Exception): Result {
        withContext(Dispatchers.IO) {
            store.writeResult(key, UploadResult(error = e.message ?: e.toString()))
            store.body(key).delete()
        }
        return Result.failure()
    }

    // The foreground service's notification: the upload's progress, a
    // tap opening the app, and Cancel (which cancels the work). Its type
    // is in the app's manifest, not the module's (asuka.toml's
    // [uses.Background.android]), which lint can't see.
    @SuppressLint("SpecifyForegroundServiceType")
    private fun foreground(sent: Long, total: Long): ForegroundInfo {
        val ctx = applicationContext
        val nm = ctx.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
        if (Build.VERSION.SDK_INT >= 26 && nm.getNotificationChannel(CHANNEL) == null) {
            nm.createNotificationChannel(NotificationChannel(CHANNEL, ctx.getString(R.string.asuka_http_uploads), NotificationManager.IMPORTANCE_LOW))
        }
        val b = NotificationCompat.Builder(ctx, CHANNEL)
            .setSmallIcon(ctx.applicationInfo.icon)
            .setContentTitle(ctx.getString(R.string.asuka_http_uploading))
            .setCategory(NotificationCompat.CATEGORY_PROGRESS)
            .setOngoing(true)
            .setOnlyAlertOnce(true)
            .setSilent(true)
            .addAction(0, ctx.getString(android.R.string.cancel), WorkManager.getInstance(ctx).createCancelPendingIntent(id))
        ctx.packageManager.getLaunchIntentForPackage(ctx.packageName)?.let {
            b.setContentIntent(PendingIntent.getActivity(ctx, 0, it, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT))
        }
        if (total > 0) {
            val f = { n: Long -> android.text.format.Formatter.formatShortFileSize(ctx, n) }
            b.setProgress(1000, (sent * 1000 / total).toInt().coerceIn(0, 1000), false)
                .setContentText(ctx.getString(R.string.asuka_http_progress, f(sent), f(total)))
        } else {
            b.setProgress(0, 0, true)
        }
        return if (Build.VERSION.SDK_INT >= 29) ForegroundInfo(notification, b.build(), ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC)
        else ForegroundInfo(notification, b.build())
    }

    private companion object {
        const val CHANNEL = "asuka.http.uploads"

        // Whether the app's manifest lets WorkManager's service run in the
        // foreground as a dataSync one: asuka.toml's [uses.Background.android]
        // gives an app that refers to http.Background its permissions and
        // the service's type. Without them (the work enqueued by an app
        // built before, say), the work runs as plain work: starting the
        // service would throw in it, out of the worker's reach.
        fun canRunInForeground(ctx: Context): Boolean = try {
            when {
                Build.VERSION.SDK_INT >= 34 &&
                    ctx.checkSelfPermission(Manifest.permission.FOREGROUND_SERVICE_DATA_SYNC) != PackageManager.PERMISSION_GRANTED -> false
                Build.VERSION.SDK_INT >= 28 &&
                    ctx.checkSelfPermission(Manifest.permission.FOREGROUND_SERVICE) != PackageManager.PERMISSION_GRANTED -> false
                Build.VERSION.SDK_INT >= 29 -> {
                    val service = ComponentName(ctx, "androidx.work.impl.foreground.SystemForegroundService")
                    (ctx.packageManager.getServiceInfo(service, 0).foregroundServiceType and ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC) != 0
                }
                else -> true
            }
        } catch (e: Exception) {
            false
        }
    }
}
