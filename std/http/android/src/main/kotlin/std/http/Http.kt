// std/http on Android: HttpURLConnection, the platform's own client,
// on the IO dispatcher; cancelling the call disconnects it. open answers
// with the head, its body read as it comes (ConnectionBody); upload and
// begin send a body as it's read (Upload.kt), a background upload
// WorkManager's (Background.kt). A transport secured with TLS settings
// (TLS.kt) gives its HTTPS connections its policy's socket factory,
// which the connection pool keeps apart.
package std.http

import dev.asuka.AsukaReleasing
import kotlinx.coroutines.CancellableContinuation
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.asExecutor
import kotlinx.coroutines.ensureActive
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.time.Duration

class ConnectionTransport(private val policy: TLSPolicy? = null, private val tls: TLS? = null) : Transport {
    // A connection to url, its TLS the policy's.
    private fun connection(url: String): HttpURLConnection {
        val c = URL(url).openConnection() as HttpURLConnection
        policy?.apply(c)
        return c
    }

    override suspend fun secured(tls: TLS): Transport = withContext(Dispatchers.IO) {
        val p = TLSPolicy.of(tls.ca, tls.trustSystem, tls.certificate, tls.key, tls.insecureSkipVerify)
        synchronized(secured) { secured.getOrPut(p) { ConnectionTransport(p, tls) } }
    }

    // A background upload is WorkManager's where the host has started
    // (an app); elsewhere (a plain JVM) it's sent as one that isn't.
    override suspend fun upload(method: String, url: String, headers: Map<String, String>, parts: List<Part>, length: Long, timeout: Duration, progress: (Long, Long) -> kotlin.Unit, background: Boolean, id: String): Response {
        val ctx = if (background) BackgroundUploads.context else null
        if (ctx != null) return BackgroundUploads.upload(ctx, tls, method, url, headers, parts, timeout, progress, id)
        return streamUpload(connection(url), method, headers, parts, length, timeout, progress)
    }

    override suspend fun begin(method: String, url: String, headers: Map<String, String>, length: Long, timeout: Duration, progress: (Long, Long) -> kotlin.Unit): Sender =
        beginUpload(connection(url), method, headers, length, timeout, progress)

    override suspend fun finished(): List<FinishedUpload> {
        val ctx = BackgroundUploads.context ?: return emptyList()
        return BackgroundUploads.finished(ctx)
    }

    companion object {
        private val secured = HashMap<TLSPolicy, ConnectionTransport>()
    }

    override suspend fun download(method: String, url: String, headers: Map<String, String>, body: ByteArray, timeout: Duration, path: String, progress: (Long, Long) -> kotlin.Unit): Response =
        fetchToFile(connection(url), method, headers, body, timeout, path, progress)

    override suspend fun send(method: String, url: String, headers: Map<String, String>, body: ByteArray, timeout: Duration): Response =
        withContext(Dispatchers.IO) {
            val c = connection(url)
            suspendCancellableCoroutine { k ->
                k.invokeOnCancellation { c.disconnect() }
                try {
                    c.requestMethod = method
                    c.instanceFollowRedirects = true
                    if (timeout.isPositive()) {
                        val ms = timeout.inWholeMilliseconds.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
                        c.connectTimeout = ms
                        c.readTimeout = ms
                    }
                    for ((k2, v) in headers) c.setRequestProperty(k2, v)
                    if (body.isNotEmpty()) {
                        c.doOutput = true
                        c.outputStream.use { it.write(body) }
                    }
                    val status = c.responseCode
                    val stream = if (status >= 400) c.errorStream else c.inputStream
                    val data = stream?.use { it.readBytes() } ?: ByteArray(0)
                    val out = HashMap<String, String>()
                    for ((k2, v) in c.headerFields) if (k2 != null) out[k2.lowercase()] = v.joinToString(", ")
                    k.resume(Response(status = status.toLong(), url = c.url.toString(), headers = out, body = data))
                } catch (e: Throwable) {
                    k.resumeWithException(e)
                } finally {
                    c.disconnect()
                }
            }
        }

    // The head, from a blocking connect on the IO dispatcher's threads
    // while the call waits: cancelled, it ends at once and disconnects
    // (off the canceller's thread), or closes the body it got after all.
    override suspend fun open(method: String, url: String, headers: Map<String, String>, body: ByteArray, timeout: Duration): Body =
        suspendCancellableCoroutine { k ->
            val c = connection(url)
            k.invokeOnCancellation { io.execute { c.disconnect() } }
            io.execute {
                try {
                    c.requestMethod = method
                    c.instanceFollowRedirects = true
                    // The longest wait for the head, then for each piece
                    // (zero: none).
                    val ms = if (timeout.isPositive()) timeout.inWholeMilliseconds.coerceIn(1L, Int.MAX_VALUE.toLong()).toInt() else 0
                    c.connectTimeout = ms
                    c.readTimeout = ms
                    for ((k2, v) in headers) c.setRequestProperty(k2, v)
                    if (body.isNotEmpty()) {
                        c.doOutput = true
                        c.outputStream.use { it.write(body) }
                    }
                    val status = c.responseCode
                    val stream = if (status >= 400) c.errorStream else c.inputStream
                    val out = HashMap<String, String>()
                    for ((k2, v) in c.headerFields) if (k2 != null) out[k2.lowercase()] = v.joinToString(", ")
                    val head = Response(status = status.toLong(), url = c.url.toString(), headers = out, body = ByteArray(0))
                    k.resume(ConnectionBody(c, head, stream)) { _, b, _ -> b.close() }
                } catch (e: Throwable) {
                    c.disconnect()
                    k.resumeWithException(e)
                }
            }
        }
}

// The IO dispatcher's threads, for blocking work a call waits for
// without being on them (so cancelling it ends it at once).
internal val io = Dispatchers.IO.asExecutor()

// A response's body as it comes: each read a blocking read into a
// buffer on the IO dispatcher's threads, which answers with what's in (a
// piece, not the buffer's fill), within the connection's read timeout.
// Cancelling a read, closing the body or Asuka letting it go disconnects,
// and answers a read waiting with the end at once (a JDK's connection
// keeps its blocked read until it returns); a closed body reads as ended.
class ConnectionBody(private val c: HttpURLConnection, private val head: Response, private val stream: java.io.InputStream?) : Body, AsukaReleasing {
    private val buf = ByteArray(64 * 1024)
    @Volatile private var closed = false
    @Volatile private var ended = stream == null
    private var waiting: Read? = null  // guarded by this

    // A read's answer, given once: by the read, or by close.
    private class Read(val k: CancellableContinuation<ByteArray>) {
        private val answered = AtomicBoolean()
        fun answer(r: Result<ByteArray>) {
            if (answered.compareAndSet(false, true)) k.resumeWith(r)
        }
    }

    override fun head(): Response = head

    override suspend fun read(): ByteArray {
        if (ended || closed) return ByteArray(0)
        val input = stream!!
        return suspendCancellableCoroutine { k ->
            val r = Read(k)
            synchronized(this) { waiting = r }
            k.invokeOnCancellation { close() }
            io.execute {
                val got = try {
                    val n = input.read(buf)
                    if (n < 0) {
                        ended = true
                        input.close()  // the connection back to the pool
                        ByteArray(0)
                    } else {
                        buf.copyOf(n)
                    }
                } catch (e: Throwable) {
                    if (closed) ByteArray(0) else {
                        r.answer(Result.failure(e))
                        close()
                        return@execute
                    }
                }
                r.answer(Result.success(if (closed) ByteArray(0) else got))
                synchronized(this) { if (waiting === r) waiting = null }
            }
        }
    }

    // Disconnecting may block (the JDK's waits for a read in progress;
    // a TLS socket's says goodbye): not on the caller's thread, which
    // may be the main one.
    override fun close() {
        val r = synchronized(this) {
            if (closed) return
            closed = true
            waiting.also { waiting = null }
        }
        r?.answer(Result.success(ByteArray(0)))
        io.execute { c.disconnect() }
    }

    override fun released() = close()
}

// A Body comes from Transport.open: Asuka doesn't make one.
fun makeBody(): Body = throw dev.asuka.AsukaUnsupportedException("http: a Body comes from Transport.open")

// download, on the IO dispatcher: the body read in chunks into a file
// beside path, renamed once it's all in; cancelling disconnects it and
// leaves no file.
private suspend fun fetchToFile(c: HttpURLConnection, method: String, headers: Map<String, String>, body: ByteArray, timeout: Duration, path: String, progress: (Long, Long) -> kotlin.Unit): Response =
    withContext(Dispatchers.IO) {
        val to = File(path)
        val part = File(path + ".download")
        suspendCancellableCoroutine { k ->
            k.invokeOnCancellation { c.disconnect() }
            try {
                c.requestMethod = method
                c.instanceFollowRedirects = true
                if (timeout.isPositive()) {
                    val ms = timeout.inWholeMilliseconds.coerceAtMost(Int.MAX_VALUE.toLong()).toInt()
                    c.connectTimeout = ms
                    c.readTimeout = ms  // the longest wait for more
                }
                for ((k2, v) in headers) c.setRequestProperty(k2, v)
                if (body.isNotEmpty()) {
                    c.doOutput = true
                    c.outputStream.use { it.write(body) }
                }
                val status = c.responseCode
                val total = c.contentLengthLong
                to.absoluteFile.parentFile?.mkdirs()
                var got = 0L
                var reported = 0L
                val stream = if (status >= 400) c.errorStream else c.inputStream
                FileOutputStream(part).use { out ->
                    stream?.use { input ->
                        val buf = ByteArray(64 * 1024)
                        while (true) {
                            val n = input.read(buf)
                            if (n < 0) break
                            out.write(buf, 0, n)
                            got += n
                            val now = System.nanoTime()
                            if (now - reported > 100_000_000L) {
                                reported = now
                                progress(got, total)
                            }
                        }
                    }
                }
                if (!part.renameTo(to)) {
                    to.delete()
                    if (!part.renameTo(to)) throw java.io.IOException("can't write $path")
                }
                progress(got, if (total >= 0) total else got)
                val out = HashMap<String, String>()
                for ((k2, v) in c.headerFields) if (k2 != null) out[k2.lowercase()] = v.joinToString(", ")
                k.resume(Response(status = status.toLong(), url = c.url.toString(), headers = out, body = ByteArray(0)))
            } catch (e: Throwable) {
                part.delete()
                k.resumeWithException(e)
            } finally {
                c.disconnect()
            }
        }
    }

fun makeTransport(): Transport = ConnectionTransport()

// LocalServer: a ServerSocket on the loopback, accepting on the IO
// dispatcher a fifth of a second at a time, so a cancelled next stops
// waiting (and the server stays for the next).
class LoopbackServers : LocalServer {
    private class Server(val socket: java.net.ServerSocket, val path: String, val page: String)

    private val servers = java.util.concurrent.ConcurrentHashMap<Long, Server>()

    override suspend fun listen(path: String, page: String): Long = withContext(Dispatchers.IO) {
        val s = java.net.ServerSocket(0, 4, java.net.InetAddress.getLoopbackAddress())
        s.soTimeout = 200
        val port = s.localPort.toLong()
        servers[port] = Server(s, path, page)
        port
    }

    override suspend fun next(port: Long): String = withContext(Dispatchers.IO) {
        val s = servers[port] ?: throw java.io.IOException("no loopback server at port $port")
        while (true) {
            val c = try {
                s.socket.accept()
            } catch (e: java.net.SocketTimeoutException) {
                ensureActive()  // the scope's: cancelled, it throws
                continue
            }
            try {
                val head = StringBuilder()
                val input = c.getInputStream()
                val buf = ByteArray(8192)
                while (head.length < 8192 && !head.contains("\r\n\r\n")) {
                    val n = input.read(buf)
                    if (n < 0) break
                    head.append(String(buf, 0, n, Charsets.ISO_8859_1))
                }
                val target = head.lineSequence().firstOrNull()?.split(" ")?.getOrNull(1) ?: ""
                val ours = target == s.path || target.startsWith(s.path + "?")
                val body = if (ours) s.page.toByteArray() else ByteArray(0)
                val status = if (ours) "200 OK" else "404 Not Found"
                val out = c.getOutputStream()
                out.write("HTTP/1.1 $status\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: ${body.size}\r\nConnection: close\r\n\r\n".toByteArray())
                out.write(body)
                out.flush()
                if (ours) return@withContext "http://127.0.0.1:$port$target"
            } finally {
                c.close()
            }
        }
        @Suppress("UNREACHABLE_CODE")
        throw IllegalStateException()
    }

    override fun close(port: Long) {
        servers.remove(port)?.socket?.close()
    }
}

private val loopbackServers = LoopbackServers()

fun makeLocalServer(): LocalServer = loopbackServers
