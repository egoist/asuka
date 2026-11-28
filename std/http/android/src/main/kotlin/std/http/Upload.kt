// Upload.kt: std/http's bodies sent as they're read, on Android. upload
// streams its parts (a file's bytes copied from disk 64 KiB at a time,
// never whole in memory) into an HttpURLConnection in fixed-length
// streaming mode when the length is known, chunked when it isn't, so the
// connection holds nothing back; begin answers with a Sender whose
// writes go to the same kind of connection as they're made, each waiting
// while the socket hasn't taken the one before. HttpURLConnection has no
// write timeout: a watchdog disconnects a body whose writing hasn't
// moved for the timeout. Background uploads are Background.kt's.
package std.http

import dev.asuka.AsukaReleasing
import kotlinx.coroutines.CancellableContinuation
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.OutputStream
import java.net.HttpURLConnection
import java.net.SocketTimeoutException
import java.util.concurrent.Executors
import java.util.concurrent.ScheduledFuture
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.time.Duration

// The pieces a body's copied in.
internal const val PIECE = 64 * 1024

// The watchdogs' thread: a daemon, so it never keeps a JVM up.
private val watchdogs = Executors.newSingleThreadScheduledExecutor { r ->
    Thread(r, "asuka-http-watchdog").apply { isDaemon = true }
}

// Stall watches a body being written: while it's writing (busy) and
// nothing has moved for the timeout, it calls stalled once (which
// disconnects, failing the write). Zero or less: no watch.
internal class Stall(timeout: Duration, private val stalled: () -> kotlin.Unit) {
    private val limit = if (timeout.isPositive()) timeout.inWholeNanoseconds else 0L
    @Volatile private var moved = System.nanoTime()
    @Volatile private var busy = false
    @Volatile var fired = false
        private set
    private val task: ScheduledFuture<*>? = if (limit <= 0) null else {
        val every = (limit / 4).coerceIn(10_000_000L, 1_000_000_000L)
        watchdogs.scheduleWithFixedDelay({
            if (busy && !fired && System.nanoTime() - moved > limit) {
                fired = true
                stalled()
            }
        }, every, every, TimeUnit.NANOSECONDS)
    }

    fun moved() { moved = System.nanoTime() }
    fun begin() { moved(); busy = true }
    fun end() { busy = false }
    fun cancel() { task?.cancel(false) }

    // The error a failed write is: a timeout when the watchdog cut it.
    fun error(e: IOException): IOException =
        if (fired) SocketTimeoutException("upload: the network took nothing for ${limit / 1_000_000} ms").also { it.initCause(e) } else e
}

// Progress, at most ten times a second (and whenever forced).
internal class Throttle(private val progress: (Long, Long) -> kotlin.Unit, private val every: Long = 100_000_000L) {
    private var last = 0L
    fun report(sent: Long, total: Long, force: Boolean = false) {
        val now = System.nanoTime()
        if (force || now - last >= every) {
            last = now
            progress(sent, total)
        }
    }
}

// A connection's request made for a streamed body of length bytes (-1:
// chunked): its method, headers and timeouts.
internal fun prepareStreaming(c: HttpURLConnection, method: String, headers: Map<String, String>, length: Long, timeout: Duration) {
    c.requestMethod = method
    c.instanceFollowRedirects = true
    if (timeout.isPositive()) {
        val ms = timeout.inWholeMilliseconds.coerceIn(1L, Int.MAX_VALUE.toLong()).toInt()
        c.connectTimeout = ms
        c.readTimeout = ms  // the longest wait for the answer
    }
    for ((k, v) in headers) c.setRequestProperty(k, v)
    c.doOutput = true
    c.useCaches = false
    if (length >= 0) c.setFixedLengthStreamingMode(length) else c.setChunkedStreamingMode(PIECE)
}

// The answer to a connection whose body was sent: status, URL, headers
// by lower-case name, and its body.
internal fun answer(c: HttpURLConnection): Response {
    val status = c.responseCode
    val stream = if (status >= 400) c.errorStream else c.inputStream
    val data = stream?.use { it.readBytes() } ?: ByteArray(0)
    return Response(status = status.toLong(), url = c.url.toString(), headers = headersOf(c), body = data)
}

internal fun headersOf(c: HttpURLConnection): Map<String, String> {
    val out = HashMap<String, String>()
    for ((k, v) in c.headerFields) if (k != null) out[k.lowercase()] = v.joinToString(", ")
    return out
}

// Writes parts in order to out: a file's bytes read 64 KiB at a time, a
// part's data in pieces of that size, each counted (sent) as the socket
// takes it.
internal fun writeParts(out: OutputStream, parts: List<Part>, sent: (Int) -> kotlin.Unit) {
    val buf = ByteArray(PIECE)
    for (p in parts) {
        if (p.path.isNotEmpty()) {
            FileInputStream(File(p.path)).use { input ->
                while (true) {
                    val n = input.read(buf)
                    if (n < 0) break
                    if (n == 0) continue
                    out.write(buf, 0, n)
                    sent(n)
                }
            }
        } else {
            var at = 0
            while (at < p.data.size) {
                val n = minOf(PIECE, p.data.size - at)
                out.write(p.data, at, n)
                at += n
                sent(n)
            }
        }
    }
}

// upload's request, on the IO dispatcher: parts streamed into c, then
// its answer. Cancelling disconnects it.
internal suspend fun streamUpload(c: HttpURLConnection, method: String, headers: Map<String, String>, parts: List<Part>, length: Long, timeout: Duration, progress: (Long, Long) -> kotlin.Unit): Response =
    withContext(Dispatchers.IO) {
        suspendCancellableCoroutine { k ->
            k.invokeOnCancellation { c.disconnect() }
            val stall = Stall(timeout) { c.disconnect() }
            try {
                prepareStreaming(c, method, headers, length, timeout)
                val report = Throttle(progress)
                var sent = 0L
                stall.begin()
                try {
                    c.outputStream.use { out ->
                        writeParts(out, parts) { n ->
                            sent += n
                            stall.moved()
                            report.report(sent, length)
                        }
                    }
                } catch (e: IOException) {
                    throw stall.error(e)
                } finally {
                    stall.end()
                }
                report.report(sent, if (length >= 0) length else sent, force = true)
                k.resume(answer(c))
            } catch (e: Throwable) {
                k.resumeWithException(e)
            } finally {
                stall.cancel()
                c.disconnect()
            }
        }
    }

// begin's connection, its head sent (the connection made) on the IO
// dispatcher: a Sender to write the body to. Cancelling disconnects it.
internal suspend fun beginUpload(c: HttpURLConnection, method: String, headers: Map<String, String>, length: Long, timeout: Duration, progress: (Long, Long) -> kotlin.Unit): Sender =
    withContext(Dispatchers.IO) {
        suspendCancellableCoroutine { k ->
            k.invokeOnCancellation { c.disconnect() }
            try {
                prepareStreaming(c, method, headers, length, timeout)
                val out = c.outputStream
                k.resume(ConnectionSender(c, out, length, timeout, progress)) { _, s, _ -> s.close() }
            } catch (e: Throwable) {
                c.disconnect()
                k.resumeWithException(e)
            }
        }
    }

// A body written as it's made: each write a blocking write on the IO
// dispatcher's threads (the socket's backpressure the wait), the call
// waiting without being on them, so cancelling it (or closing) ends it
// at once. A write that fails (the server answered and closed, the
// network went, the watchdog found it stalled) ends the request: false,
// and finish says how. Asuka letting it go closes it.
class ConnectionSender internal constructor(
    private val c: HttpURLConnection,
    private val out: OutputStream,
    private val length: Long,
    timeout: Duration,
    progress: (Long, Long) -> kotlin.Unit,
) : Sender, AsukaReleasing {
    private val stall = Stall(timeout) { c.disconnect() }
    private val report = Throttle(progress)
    private val lock = Object()
    @Volatile private var closed = false
    @Volatile private var failed: IOException? = null
    private var sent = 0L      // guarded by lock (one write at a time)
    private var finished = false

    // A blocking step's answer, given once: by the step, or by close.
    private class Step<T>(val k: CancellableContinuation<T>) {
        private val answered = AtomicBoolean()
        fun answer(r: Result<T>) {
            if (answered.compareAndSet(false, true)) k.resumeWith(r)
        }
    }

    private suspend fun <T> blocking(onClose: () -> Result<T>, work: () -> T): T =
        suspendCancellableCoroutine { k ->
            val s = Step(k)
            k.invokeOnCancellation { close() }
            io.execute {
                val r = try {
                    Result.success(work())
                } catch (e: Throwable) {
                    Result.failure(e)
                }
                s.answer(if (closed && r.isFailure) onClose() else r)
            }
        }

    override suspend fun write(piece: ByteArray): Boolean {
        if (closed || failed != null || finished) return false
        if (piece.isEmpty()) return true
        return blocking({ Result.success(false) }) {
            synchronized(lock) {
                if (closed || failed != null || finished) return@synchronized false
                stall.begin()
                try {
                    var at = 0
                    while (at < piece.size) {
                        val n = minOf(PIECE, piece.size - at)
                        out.write(piece, at, n)
                        at += n
                        sent += n
                        stall.moved()
                        report.report(sent, length)
                    }
                    true
                } catch (e: IOException) {
                    failed = stall.error(e)
                    false
                } finally {
                    stall.end()
                }
            }
        }
    }

    override suspend fun finish(): Response =
        blocking({ Result.failure(IOException("upload: the request was stopped")) }) {
            synchronized(lock) {
                if (closed) throw IOException("upload: the request was stopped")
                finished = true
                stall.cancel()
                try {
                    val wrote = failed
                    if (wrote == null) {
                        try {
                            out.close()
                        } catch (e: IOException) {
                            failed = e
                        }
                    }
                    val err = failed
                    if (err == null) report.report(sent, if (length >= 0) length else sent, force = true)
                    try {
                        answer(c)
                    } catch (e: IOException) {
                        // The server answered early and the body's write
                        // failed: the answer when the connection still
                        // gives it, else why the write did.
                        if (err != null) {
                            err.addSuppressed(e)
                            throw err
                        }
                        throw e
                    }
                } finally {
                    c.disconnect()
                }
            }
        }

    // Disconnecting may block (a TLS socket says goodbye): not on the
    // caller's thread, which may be the main one.
    override fun close() {
        if (closed) return
        closed = true
        stall.cancel()
        io.execute { c.disconnect() }
    }

    override fun released() = close()
}

// A Sender comes from Transport.begin: Asuka doesn't make one.
fun makeSender(): Sender = throw dev.asuka.AsukaUnsupportedException("http: a Sender comes from Transport.begin")
