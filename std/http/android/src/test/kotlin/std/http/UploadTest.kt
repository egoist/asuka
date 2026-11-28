// Transport.upload and begin's Sender on this computer's JVM
// (HttpURLConnection is the JDK's here), against internal/testserver's
// /upload and /multipart, which read the body as it comes and say what
// came; and the background uploads' files (UploadStore), which are plain
// files here too. WorkManager itself needs a device.
package std.http

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.delay
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.net.SocketTimeoutException
import java.security.MessageDigest
import java.util.Collections
import kotlin.time.Duration
import kotlin.time.Duration.Companion.milliseconds
import kotlin.time.Duration.Companion.seconds

class UploadTest {
    @get:Rule val tmp = TemporaryFolder()
    private val t = ConnectionTransport()

    // A file of size bytes (a pattern no piece repeats in step with), and
    // its SHA-256, written a piece at a time.
    private fun file(size: Long, name: String = "body"): Pair<File, String> {
        val f = tmp.newFile(name)
        val md = MessageDigest.getInstance("SHA-256")
        val buf = ByteArray(1 shl 20)
        var left = size
        var n = 0
        FileOutputStream(f).use { out ->
            while (left > 0) {
                val k = minOf(left, buf.size.toLong()).toInt()
                for (i in 0 until k) buf[i] = ((n++ * 31 + (n shr 11)) and 0xff).toByte()
                out.write(buf, 0, k)
                md.update(buf, 0, k)
                left -= k
            }
        }
        return f to hex(md.digest())
    }

    private fun hex(b: ByteArray) = b.joinToString("") { "%02x".format(it) }
    private fun sha(b: ByteArray) = hex(MessageDigest.getInstance("SHA-256").digest(b))

    private class Progress {
        val calls: MutableList<Pair<Long, Long>> = Collections.synchronizedList(ArrayList())
        val fn: (Long, Long) -> kotlin.Unit = { a, b -> calls.add(a to b) }

        fun assertMonotonicEndingAt(total: Long) {
            val c = ArrayList(calls)
            assertTrue("no progress", c.isNotEmpty())
            for (i in 1 until c.size) assertTrue("progress went back: $c", c[i].first >= c[i - 1].first)
            assertEquals(total to total, c.last())
        }
    }

    private fun json(r: Response): JSONObject {
        assertEquals(String(r.body), 200L, r.status)
        return JSONObject(String(r.body))
    }

    // A file larger than the test's JVM may hold (its heap is capped
    // below it, build.gradle.kts): its bytes read from disk as they're
    // sent, with its Content-Length.
    @Test fun aLargeFileIsSentAsItsRead() = runBlocking {
        val size = 256L shl 20
        assertTrue("the heap (${Runtime.getRuntime().maxMemory()}) holds the file", Runtime.getRuntime().maxMemory() < size)
        val (f, sum) = file(size)
        val p = Progress()
        var most = 0L
        val sample: (Long, Long) -> kotlin.Unit = { a, b ->
            val rt = Runtime.getRuntime()
            most = maxOf(most, rt.totalMemory() - rt.freeMemory())
            p.fn(a, b)
        }
        val r = t.upload("PUT", TestServer.url + "/upload", mapOf("Content-Type" to "video/mp4"), listOf(Part(path = f.path)), size, 30.seconds, sample, false, "")
        val o = json(r)
        assertEquals(size, o.getLong("bytes"))
        assertEquals(sum, o.getString("sha256"))
        assertEquals(size, o.getLong("length"))
        assertFalse(o.getBoolean("chunked"))
        assertEquals("PUT", o.getString("method"))
        assertEquals("video/mp4", o.getString("type"))
        p.assertMonotonicEndingAt(size)
        assertTrue("heap use $most for a $size-byte body", most < size / 2)
    }

    // A length that isn't known: sent chunked, its progress's whole the
    // bytes sent once all are.
    @Test fun anUnknownLengthIsSentChunked() = runBlocking {
        val data = ByteArray(300_000) { (it % 251).toByte() }
        val p = Progress()
        val r = t.upload("POST", TestServer.url + "/upload", emptyMap(), listOf(Part(data = data)), -1, 10.seconds, p.fn, false, "")
        val o = json(r)
        assertEquals(data.size.toLong(), o.getLong("bytes"))
        assertEquals(sha(data), o.getString("sha256"))
        assertEquals(-1L, o.getLong("length"))
        assertTrue(o.getBoolean("chunked"))
        p.assertMonotonicEndingAt(data.size.toLong())
    }

    // Parts as http.multipart makes them: bytes, a file, bytes.
    @Test fun aMultipartFormsPartsArriveInOrder() = runBlocking {
        val (f, sum) = file(3L shl 20, "video.mp4")
        val b = "----asuka-test0123456789"
        val head = "--$b\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nHoliday\r\n" +
            "--$b\r\nContent-Disposition: form-data; name=\"video\"; filename=\"video.mp4\"\r\nContent-Type: video/mp4\r\n\r\n"
        val note = "hello".toByteArray()
        val tail = "\r\n--$b\r\nContent-Disposition: form-data; name=\"note\"; filename=\"note.txt\"\r\nContent-Type: text/plain\r\n\r\n"
        val parts = listOf(
            Part(data = head.toByteArray()),
            Part(path = f.path),
            Part(data = tail.toByteArray() + note + "\r\n--$b--\r\n".toByteArray()),
        )
        val length = parts.sumOf { if (it.path.isNotEmpty()) File(it.path).length() else it.data.size.toLong() }
        val p = Progress()
        val r = t.upload("POST", TestServer.url + "/multipart", mapOf("Content-Type" to "multipart/form-data; boundary=$b"), parts, length, 10.seconds, p.fn, false, "")
        val o = json(r)
        assertEquals("Holiday", o.getJSONObject("fields").getString("title"))
        val files = o.getJSONArray("files")
        assertEquals(2, files.length())
        val v = files.getJSONObject(0)
        assertEquals("video", v.getString("name"))
        assertEquals("video.mp4", v.getString("filename"))
        assertEquals("video/mp4", v.getString("type"))
        assertEquals(3L shl 20, v.getLong("bytes"))
        assertEquals(sum, v.getString("sha256"))
        val n = files.getJSONObject(1)
        assertEquals("note.txt", n.getString("filename"))
        assertEquals(sha(note), n.getString("sha256"))
        assertEquals(length, o.getLong("length"))
        p.assertMonotonicEndingAt(length)
    }

    // A server that reads slowly: progress as the socket takes the body.
    @Test fun progressFollowsTheNetwork() = runBlocking {
        val (f, _) = file(2L shl 20)
        val p = Progress()
        t.upload("POST", TestServer.url + "/upload?delay=10", emptyMap(), listOf(Part(path = f.path)), f.length(), 10.seconds, p.fn, false, "")
        assertTrue("progress: ${p.calls}", p.calls.size >= 2)
        p.assertMonotonicEndingAt(f.length())
    }

    // The network taking nothing for the timeout (a server that reads a
    // piece every 1.5 s): the upload ends with a timeout.
    @Test fun aStalledUploadTimesOut() = runBlocking {
        val (f, _) = file(32L shl 20)
        try {
            withTimeout(20.seconds) {
                t.upload("POST", TestServer.url + "/upload?delay=1500", emptyMap(), listOf(Part(path = f.path)), f.length(), 300.milliseconds, { _, _ -> }, false, "")
            }
            fail("a stalled upload ended")
        } catch (e: SocketTimeoutException) {
        }
    }

    @Test fun cancellingAnUploadDisconnects() = runBlocking {
        val (f, _) = file(32L shl 20)
        val p = Progress()
        val r = async(Dispatchers.IO) {
            t.upload("POST", TestServer.url + "/upload?delay=200", emptyMap(), listOf(Part(path = f.path)), f.length(), Duration.ZERO, p.fn, false, "")
        }
        delay(300)
        r.cancel()
        withTimeout(10.seconds) { r.join() }
        assertTrue(r.isCancelled)
    }

    @Test fun aMissingFileThrows() = runBlocking {
        try {
            t.upload("POST", TestServer.url + "/upload", emptyMap(), listOf(Part(path = tmp.root.path + "/none")), 10, 5.seconds, { _, _ -> }, false, "")
            fail("sent a missing file")
        } catch (e: IOException) {
        }
    }

    // Off a device (no host) a background upload is sent as one that
    // isn't, and none has finished.
    @Test fun withoutAHostBackgroundChangesNothing() = runBlocking {
        val data = "background".toByteArray()
        val o = json(t.upload("POST", TestServer.url + "/upload", emptyMap(), listOf(Part(data = data)), data.size.toLong(), 5.seconds, { _, _ -> }, true, "u1"))
        assertEquals(sha(data), o.getString("sha256"))
        assertTrue(t.finished().isEmpty())
    }

    // ---- Sender ----

    private suspend fun begin(path: String, length: Long, timeout: Duration = 10.seconds, progress: (Long, Long) -> kotlin.Unit = { _, _ -> }) =
        t.begin("POST", TestServer.url + path, mapOf("Content-Type" to "application/x-test"), length, timeout, progress)

    @Test fun aSenderWithoutALengthSendsChunked() = runBlocking {
        val p = Progress()
        val s = begin("/upload", -1, progress = p.fn)
        val pieces = listOf("one ".toByteArray(), ByteArray(200_000) { 7 }, "three".toByteArray())
        for (piece in pieces) assertTrue(s.write(piece))
        val o = json(s.finish())
        val all = pieces.reduce { a, b -> a + b }
        assertEquals(all.size.toLong(), o.getLong("bytes"))
        assertEquals(sha(all), o.getString("sha256"))
        assertTrue(o.getBoolean("chunked"))
        assertEquals(-1L, o.getLong("length"))
        assertEquals("application/x-test", o.getString("type"))
        p.assertMonotonicEndingAt(all.size.toLong())
        s.close()
        assertFalse(s.write("more".toByteArray()))
    }

    @Test fun aSenderWithALengthSendsIt() = runBlocking {
        val piece = ByteArray(100_000) { (it % 13).toByte() }
        val s = begin("/upload", piece.size * 3L)
        repeat(3) { assertTrue(s.write(piece)) }
        val o = json(s.finish())
        assertEquals(piece.size * 3L, o.getLong("length"))
        assertEquals(piece.size * 3L, o.getLong("bytes"))
        assertFalse(o.getBoolean("chunked"))
        s.close()
    }

    // The server answering at once, reading nothing: the writes stop
    // (false) long before the body's end, and finish gives the answer
    // where the connection still gives it, else the write's failure.
    @Test fun aSenderStopsWhenTheServerAnswersEarly() = runBlocking {
        val length = 256L shl 20
        val s = begin("/upload?answer=413", length)
        val piece = ByteArray(64 * 1024)
        var sent = 0L
        withTimeout(30.seconds) {
            while (sent < length && s.write(piece)) sent += piece.size
        }
        assertTrue("sent all $sent bytes to a server that answered", sent < length)
        try {
            val r = s.finish()
            assertEquals(413L, r.status)
            assertEquals("refused", String(r.body))
        } catch (e: IOException) {
            // The JDK's HttpURLConnection (and Android's) won't read an
            // answer once the body's write has failed.
        }
        s.close()
    }

    // Closed while a write waits on a server that doesn't read: it ends.
    @Test fun closingASenderEndsAWaitingWrite() = runBlocking {
        val s = begin("/upload?delay=1500", -1, timeout = Duration.ZERO)
        val piece = ByteArray(1 shl 20)
        val w = async(Dispatchers.IO) {
            while (s.write(piece)) Unit
            false
        }
        delay(500)  // writes waiting in the socket (the test passes either way)
        s.close()
        assertFalse(withTimeout(10.seconds) { w.await() })
        try {
            s.finish()
            fail("a closed Sender finished")
        } catch (e: IOException) {
        }
    }

    @Test fun makingASenderIsUnsupported() {
        try {
            makeSender()
            fail("made a Sender")
        } catch (e: dev.asuka.AsukaUnsupportedException) {
        }
    }

    // ---- The background uploads' files ----

    @Test fun aSpooledBodyIsItsPartsInOrder() {
        val store = UploadStore(tmp.newFolder("uploads"))
        val (f, _) = file(200_000, "photo.jpg")
        val parts = listOf(Part(data = "head".toByteArray()), Part(path = f.path), Part(data = "tail".toByteArray()))
        val n = store.spool("k1", parts)
        val want = "head".toByteArray() + f.readBytes() + "tail".toByteArray()
        assertEquals(want.size.toLong(), n)
        assertEquals(sha(want), sha(store.body("k1").readBytes()))

        // A file alone: linked (or copied, where it can't be), the same bytes.
        assertEquals(f.length(), store.spool("k2", listOf(Part(path = f.path))))
        assertEquals(sha(f.readBytes()), sha(store.body("k2").readBytes()))
        try {
            store.spool("k3", listOf(Part(path = f.path + ".none")))
            fail("spooled a missing file")
        } catch (e: IOException) {
        }
    }

    @Test fun aRequestAndItsResultAreKept() {
        val store = UploadStore(tmp.newFolder("uploads"))
        store.spool("k", listOf(Part(data = "x".toByteArray())))
        val tls = TLS(ca = "PEM", trustSystem = true, certificate = "C", key = "K")
        val req = UploadRequest(id = "video-1", method = "PUT", url = "https://example.com/v", headers = mapOf("content-type" to "video/mp4", "x-token" to "a\"b"), length = 1, timeoutMs = 60_000, tls = tls)
        store.writeRequest("k", req)
        assertEquals(req, store.request("k"))
        assertNull(store.request("other"))
        val body = ByteArray(1000) { it.toByte() }
        store.writeResult("k", UploadResult(status = 201, url = "https://example.com/v/1", headers = mapOf("location" to "/v/1"), body = body))
        val r = store.claim("k")!!
        assertEquals(201L, r.status)
        assertEquals("/v/1", r.headers["location"])
        assertEquals(sha(body), sha(r.body))
        assertNull(store.claim("k"))  // given once
        assertFalse(store.dir("k").exists())
        // A result for an upload let go (its directory gone) isn't written.
        store.writeResult("k", UploadResult(status = 200))
        assertFalse(store.dir("k").exists())
    }

    @Test fun finishedGivesWhatNoCallWaitsForOnce() {
        val store = UploadStore(tmp.newFolder("uploads"))
        fun upload(key: String, id: String) {
            store.spool(key, listOf(Part(data = "x".toByteArray())))
            store.writeRequest(key, UploadRequest(id = id, method = "POST", url = "u", headers = emptyMap(), length = 1, timeoutMs = 0))
        }
        upload("a", "answered")
        store.writeResult("a", UploadResult(status = 200, url = "https://e/a", headers = mapOf("etag" to "1")))
        upload("b", "failed")
        store.writeResult("b", UploadResult(error = "the network went"))
        upload("c", "waited for")
        store.writeResult("c", UploadResult(status = 200))
        upload("d", "running")
        upload("e", "never enqueued")
        val got = store.finished(setOf("c")) { it == "e" }.sortedBy { it.id }
        assertEquals(listOf("answered", "failed"), got.map { it.id })
        assertEquals(200L, got[0].status)
        assertEquals("1", got[0].headers["etag"])
        assertEquals("", got[0].error)
        assertEquals(0L, got[1].status)
        assertEquals("the network went", got[1].error)
        assertEquals(listOf("c", "d"), store.keys())
        assertTrue(store.finished(setOf("c")) { false }.isEmpty())  // each given once
        assertEquals(1, store.finished(emptySet()) { false }.size)  // c, once its call is gone
    }
}
