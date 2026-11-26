// platform/nfc on Android: the NFC adapter's reader mode
// (enableReaderMode) for the activity in front, enabled again as it
// comes back while a session looks. Tags are talked to on a thread of
// their own (their I/O blocks); the session's state is the main
// thread's. NFC turned on or off is ACTION_ADAPTER_STATE_CHANGED; tags
// the system opens the app with (NDEF_DISCOVERED, TECH_DISCOVERED, of
// asuka.toml's nfc filters) come from the activity's intent as it's
// resumed.
package platform.nfc

import android.app.Activity
import android.app.Application
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.nfc.NdefMessage
import android.nfc.NfcAdapter
import android.nfc.Tag
import android.nfc.tech.IsoDep
import android.nfc.tech.MifareUltralight
import android.nfc.tech.Ndef
import android.nfc.tech.NdefFormatable
import android.nfc.tech.NfcA
import android.nfc.tech.NfcB
import android.nfc.tech.NfcF
import android.nfc.tech.NfcV
import android.nfc.tech.TagTechnology
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import androidx.core.content.ContextCompat
import androidx.core.content.IntentCompat
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.IOException
import java.util.concurrent.Executors
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

private val main = Handler(Looper.getMainLooper())
private val io = Executors.newSingleThreadExecutor()

class AndroidReader(private val app: Application) : Reader {
    private val adapter: NfcAdapter? get() = NfcAdapter.getDefaultAdapter(app)

    private class Session(val id: Long, val flags: Int, val extras: Bundle?) {
        var waiting: ((kotlin.Result<Found>) -> Unit)? = null
        val queue = ArrayDeque<Found>()
    }

    private class Held(val tag: Tag) {
        var tech: TagTechnology? = null
        fun close() = io.execute { try { tech?.close() } catch (_: Exception) {}; tech = null }
    }

    private var session: Session? = null
    private var lastEnded: Pair<Long, Exception>? = null
    private var ids = 0L
    private var handles = 0L
    private val held = HashMap<Long, Held>()
    private val openReaders = HashMap<Int, (Found) -> Unit>()
    private val openQueue = ArrayDeque<Found>()
    private var nextReader = 0

    init {
        main.post {
            app.registerActivityLifecycleCallbacks(object : Application.ActivityLifecycleCallbacks {
                override fun onActivityResumed(a: Activity) {
                    session?.let { enable(it, a) }
                    opened(a)
                }
                override fun onActivityPaused(a: Activity) {
                    if (session != null) runCatching { adapter?.disableReaderMode(a) }
                }
                override fun onActivityCreated(a: Activity, b: Bundle?) {}
                override fun onActivityStarted(a: Activity) {}
                override fun onActivityStopped(a: Activity) {}
                override fun onActivitySaveInstanceState(a: Activity, b: Bundle) {}
                override fun onActivityDestroyed(a: Activity) {}
            })
            AsukaModules.activity?.let { opened(it) }
        }
    }

    override fun state(): State {
        val a = adapter ?: return State()
        return State(supported = true, enabled = a.isEnabled)
    }

    override suspend fun watchState(changed: (State) -> Unit) {
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) { changed(state()) }
        }
        withContext(Dispatchers.Main) {
            changed(state())
            ContextCompat.registerReceiver(app, r, IntentFilter(NfcAdapter.ACTION_ADAPTER_STATE_CHANGED), ContextCompat.RECEIVER_NOT_EXPORTED)
        }
        try {
            awaitCancellation()
        } finally {
            main.post { runCatching { app.unregisterReceiver(r) } }
        }
    }

    override suspend fun begin(options: Options): Long = withContext(Dispatchers.Main) {
        val a = adapter ?: throw NfcException(NfcError.NOT_AVAILABLE)
        if (!a.isEnabled) throw NfcException(NfcError.DISABLED)
        session?.let { end(it.id, "") }
        var flags = 0
        if (options.poll.isEmpty()) {
            flags = NfcAdapter.FLAG_READER_NFC_A or NfcAdapter.FLAG_READER_NFC_B or NfcAdapter.FLAG_READER_NFC_F or NfcAdapter.FLAG_READER_NFC_V
        } else {
            if (Technology.A in options.poll) flags = flags or NfcAdapter.FLAG_READER_NFC_A
            if (Technology.B in options.poll) flags = flags or NfcAdapter.FLAG_READER_NFC_B
            if (Technology.F in options.poll) flags = flags or NfcAdapter.FLAG_READER_NFC_F
            if (Technology.V in options.poll) flags = flags or NfcAdapter.FLAG_READER_NFC_V
        }
        if (options.skipNDEF) flags = flags or NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK
        if (options.noSound) flags = flags or NfcAdapter.FLAG_READER_NO_PLATFORM_SOUNDS
        val extras = if (options.presenceDelay.inWholeMilliseconds > 0) Bundle().apply {
            putInt(NfcAdapter.EXTRA_READER_PRESENCE_CHECK_DELAY, options.presenceDelay.inWholeMilliseconds.toInt())
        } else null
        val s = Session(++ids, flags, extras)
        session = s
        AsukaModules.activity?.let { enable(s, it) }
        s.id
    }

    private fun enable(s: Session, act: Activity) {
        val a = adapter ?: return
        runCatching { a.enableReaderMode(act, { tag -> main.post { discovered(s, tag) } }, s.flags, s.extras) }
    }

    override suspend fun next(session: Long): Found = suspendCancellableCoroutine { c ->
        main.post {
            val s = this.session
            if (s == null || s.id != session) {
                val l = lastEnded
                c.resumeWithException(if (l != null && l.first == session) l.second else NfcException(NfcError.CANCELED))
                return@post
            }
            val f = s.queue.removeFirstOrNull()
            if (f != null) c.resume(f) else s.waiting = { r -> if (c.isActive) r.fold({ c.resume(it) }, { c.resumeWithException(it) }) }
        }
    }

    override fun setAlert(session: Long, alert: String) {}

    override fun end(session: Long, message: String) {
        main.post {
            val s = this.session ?: return@post
            if (s.id != session) return@post
            this.session = null
            lastEnded = session to NfcException(NfcError.CANCELED)
            AsukaModules.activity?.let { runCatching { adapter?.disableReaderMode(it) } }
            s.waiting?.invoke(kotlin.Result.failure(NfcException(NfcError.CANCELED)))
            s.waiting = null
            for (h in held.values) h.close()
            held.clear()
        }
    }

    private fun discovered(s: Session, tag: Tag) {
        if (session !== s) return
        val h = ++handles
        held[h] = Held(tag)
        io.execute {
            val f = found(tag, h, s.flags and NfcAdapter.FLAG_READER_SKIP_NDEF_CHECK != 0)
            main.post {
                if (session !== s) return@post
                val w = s.waiting
                if (w != null) {
                    s.waiting = null
                    w(kotlin.Result.success(f))
                } else s.queue.addLast(f)
            }
        }
    }

    private fun techName(cls: String): String = when (cls.substringAfterLast('.')) {
        "NfcA" -> "nfcA"
        "NfcB" -> "nfcB"
        "NfcF" -> "felica"
        "NfcV" -> "iso15693"
        "IsoDep" -> "isoDep"
        "MifareClassic" -> "mifareClassic"
        "MifareUltralight" -> "mifareUltralight"
        "Ndef" -> "ndef"
        "NdefFormatable" -> "ndefFormatable"
        "NfcBarcode" -> "barcode"
        else -> cls.substringAfterLast('.').replaceFirstChar { it.lowercase() }
    }

    /** A tag as the package takes it: its message read (unless skipped). On the tag's thread. */
    private fun found(tag: Tag, handle: Long, skipNDEF: Boolean): Found {
        val techs = tag.techList.map { techName(it) }.toMutableList()
        if (techs.any { it.startsWith("mifare") } && "mifare" !in techs) techs.add(1.coerceAtMost(techs.size), "mifare")
        if ("isoDep" in techs) techs.add("iso7816")
        var f = Found(handle = handle, id = tag.id ?: ByteArray(0), techs = techs)
        val ndef = Ndef.get(tag)
        when {
            ndef != null -> {
                f = f.copy(format = if (ndef.isWritable) Format.READ_WRITE else Format.READ_ONLY, capacity = ndef.maxSize.toLong(),
                    canMakeReadOnly = ndef.canMakeReadOnly(), type = ndef.type)
                if (!skipNDEF) {
                    val m = ndef.cachedNdefMessage
                    f = f.copy(hasMessage = true, message = m?.toByteArray() ?: byteArrayOf(0xd0.toByte(), 0, 0))
                }
            }
            NdefFormatable.get(tag) != null -> f = f.copy(format = Format.FORMATABLE)
        }
        return f
    }

    private fun <T : TagTechnology> connect(h: Held, t: T?): T {
        if (t == null) throw NfcException(NfcError.NOT_SUPPORTED)
        val cur = h.tech
        if (cur != null && cur !== t) runCatching { cur.close() }
        if (!t.isConnected) t.connect()
        h.tech = t
        return t
    }

    /** Runs f on the tag's thread with the tag the handle names. */
    private suspend fun <T> op(handle: Long, f: (Held) -> T): T {
        val h = withContext(Dispatchers.Main) { held[handle] } ?: throw NfcException(NfcError.TAG_LOST)
        return suspendCancellableCoroutine { c ->
            io.execute {
                try {
                    c.resume(f(h))
                } catch (e: android.nfc.TagLostException) {
                    c.resumeWithException(NfcException(NfcError.TAG_LOST))
                } catch (e: IOException) {
                    c.resumeWithException(NfcException(NfcError.TAG_LOST))
                } catch (e: Exception) {
                    c.resumeWithException(e)
                }
            }
        }
    }

    override suspend fun read(tag: Long): ByteArray = op(tag) { h ->
        val n = connect(h, Ndef.get(h.tag))
        n.ndefMessage?.toByteArray() ?: byteArrayOf(0xd0.toByte(), 0, 0)
    }

    override suspend fun write(tag: Long, message: ByteArray) = op(tag) { h ->
        val m = try { NdefMessage(message) } catch (e: Exception) { throw NfcException(NfcError.BAD_MESSAGE) }
        val n = Ndef.get(h.tag)
        if (n != null) {
            connect(h, n)
            if (!n.isWritable) throw NfcException(NfcError.READ_ONLY)
            if (message.size > n.maxSize) throw NfcException(NfcError.TOO_LARGE)
            n.writeNdefMessage(m)
        } else {
            connect(h, NdefFormatable.get(h.tag)).format(m)
        }
    }

    override suspend fun makeReadOnly(tag: Long) = op(tag) { h ->
        val n = connect(h, Ndef.get(h.tag))
        if (!n.canMakeReadOnly() || !n.makeReadOnly()) throw NfcException(NfcError.NOT_SUPPORTED)
    }

    override suspend fun transceive(tag: Long, data: ByteArray): ByteArray = op(tag) { h ->
        val t = h.tag
        IsoDep.get(t)?.let { return@op connect(h, it).transceive(data) }
        MifareUltralight.get(t)?.let { return@op connect(h, it).transceive(data) }
        NfcA.get(t)?.let { return@op connect(h, it).transceive(data) }
        NfcB.get(t)?.let { return@op connect(h, it).transceive(data) }
        // FeliCa's frames carry their length first.
        NfcF.get(t)?.let { f ->
            val a = connect(h, f).transceive(byteArrayOf((data.size + 1).toByte()) + data)
            return@op if (a.isEmpty()) a else a.copyOfRange(1, a.size)
        }
        NfcV.get(t)?.let { return@op connect(h, it).transceive(data) }
        throw NfcException(NfcError.NOT_SUPPORTED)
    }

    /** A tag the activity was opened (or resumed) with, given once. Main thread. */
    private fun opened(a: Activity) {
        val i = a.intent ?: return
        val action = i.action
        if (action != NfcAdapter.ACTION_NDEF_DISCOVERED && action != NfcAdapter.ACTION_TECH_DISCOVERED && action != NfcAdapter.ACTION_TAG_DISCOVERED) return
        if (i.getBooleanExtra("asuka.nfc.given", false)) return
        i.putExtra("asuka.nfc.given", true)
        // A URI's tag opens the app as its link (openURL).
        if (action == NfcAdapter.ACTION_NDEF_DISCOVERED && i.data != null) return
        val tag = IntentCompat.getParcelableExtra(i, NfcAdapter.EXTRA_TAG, Tag::class.java) ?: return
        val h = ++handles
        held[h] = Held(tag)
        io.execute {
            val f = found(tag, h, false)
            main.post { if (openReaders.isEmpty()) openQueue.addLast(f) else openReaders.values.toList().forEach { it(f) } }
        }
    }

    override suspend fun opened(tag: (Found) -> Unit) {
        val id = withContext(Dispatchers.Main) {
            val n = ++nextReader
            openReaders[n] = tag
            while (openQueue.isNotEmpty()) tag(openQueue.removeFirst())
            n
        }
        try {
            awaitCancellation()
        } finally {
            main.post { openReaders.remove(id) }
        }
    }

    override suspend fun openSettings() = withContext(Dispatchers.Main) {
        val i = Intent(android.provider.Settings.ACTION_NFC_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        (AsukaModules.activity ?: app).startActivity(i)
    }
}

fun makeReader(): Reader = AndroidReader(AsukaModules.context.applicationContext as Application)
