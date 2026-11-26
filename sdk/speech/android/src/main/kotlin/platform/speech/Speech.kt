// platform/speech on Android: TextToSpeech. Utterances queue here:
// TextToSpeech can't pause, so a pause stops at the word being spoken
// (onRangeStart) and a resume speaks on from it. Other apps duck while it
// speaks (transient audio focus). Everything runs on the main thread.
package platform.speech

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import dev.asuka.AsukaModules
import kotlinx.coroutines.CancellableContinuation
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.util.Locale
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import android.speech.tts.Voice as TtsVoice

private class Utterance(val id: String, val text: String, val o: Options, val done: (Throwable?) -> Unit) {
    var offset = 0 // where to speak from: the word a pause stopped at
}

/** The engine and its queue, on the main thread. */
private object Engine {
    private val main = Handler(Looper.getMainLooper())
    private var tts: TextToSpeech? = null
    private var ready = false
    private var failed: String? = null
    private val queue = ArrayDeque<Utterance>()
    var paused = false
        private set
    // Each start of the engine's queue (a pause or stop ends it): callbacks
    // for an earlier one are stale. The engine's utterance IDs are id#gen.
    private var gen = 0
    private var next = 0
    private val waitingVoices = ArrayList<() -> Unit>()
    private val spokenFrom = HashMap<String, Int>()
    private val app get() = AsukaModules.context
    private val audio get() = app.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private val attributes = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_ASSISTANCE_NAVIGATION_GUIDANCE)
        .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build()
    // Before Android 8 audio focus is asked for with a listener, for the
    // stream the attributes play on (music's).
    private val focus = if (Build.VERSION.SDK_INT >= 26) AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK).setAudioAttributes(attributes).build() else null
    private val focusListener = AudioManager.OnAudioFocusChangeListener {}
    private var focused = false

    @Suppress("DEPRECATION")
    private fun requestFocus(): Boolean {
        val r = if (Build.VERSION.SDK_INT >= 26) audio.requestAudioFocus(focus!!)
            else audio.requestAudioFocus(focusListener, AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK)
        return r == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
    }

    @Suppress("DEPRECATION")
    private fun abandonFocus() {
        if (Build.VERSION.SDK_INT >= 26) audio.abandonAudioFocusRequest(focus!!) else audio.abandonAudioFocus(focusListener)
    }

    val speaking get() = queue.isNotEmpty()

    /** The engine, made the first time it's needed; it's ready a moment later. */
    private fun engine(): TextToSpeech = tts ?: TextToSpeech(app) { status ->
        main.post {
            if (status == TextToSpeech.SUCCESS) {
                ready = true
                tts?.setAudioAttributes(attributes)
                if (!paused) for (u in queue) enqueue(u)
            } else {
                failed = "no text-to-speech engine"
                val us = queue.toList()
                queue.clear()
                for (u in us) u.done(Exception(failed))
            }
            val vs = ArrayList(waitingVoices)
            waitingVoices.clear()
            for (v in vs) v()
        }
    }.also { t ->
        tts = t
        t.setOnUtteranceProgressListener(object : UtteranceProgressListener() {
            override fun onStart(id: String) {}
            override fun onDone(id: String) { main.post { current(id)?.let { done(it, null) } } }
            @Deprecated("the old signature")
            override fun onError(id: String) { main.post { current(id)?.let { done(it, Exception("speech failed")) } } }
            override fun onError(id: String, code: Int) { main.post { current(id)?.let { done(it, Exception("speech failed ($code)")) } } }
            override fun onStop(id: String, interrupted: Boolean) { main.post { current(id)?.let { done(it, SpeechException(SpeechError.STOPPED)) } } }
            // The word being spoken: where a pause resumes from. Before
            // Android 8 the engine doesn't say, and a pause resumes from the
            // utterance's start.
            override fun onRangeStart(id: String, start: Int, end: Int, frame: Int) {
                main.post {
                    val uid = current(id) ?: return@post
                    val u = queue.firstOrNull { it.id == uid } ?: return@post
                    u.offset = (spokenFrom[u.id] ?: 0) + start
                }
            }
        })
    }

    private fun enqueue(u: Utterance) {
        val t = tts ?: return
        val o = u.o
        val chosen = o.voice.takeIf { it.isNotEmpty() }?.let { v -> t.voices?.firstOrNull { it.name == v } }
        when {
            chosen != null -> t.voice = chosen
            o.language.isNotEmpty() -> t.language = Locale.forLanguageTag(o.language)
            else -> t.language = Locale.getDefault()
        }
        t.setPitch((if (o.pitch > 0) o.pitch else 1.0).toFloat().coerceIn(0.5f, 2f))
        t.setSpeechRate((if (o.rate > 0) o.rate else 1.0).toFloat().coerceIn(0.5f, 2f))
        val params = Bundle().apply { putFloat(TextToSpeech.Engine.KEY_PARAM_VOLUME, o.volume.toFloat().coerceIn(0f, 1f)) }
        if (!focused) focused = requestFocus()
        spokenFrom[u.id] = u.offset
        t.speak(u.text.substring(u.offset), TextToSpeech.QUEUE_ADD, params, u.id + "#" + gen)
    }

    /** An engine utterance ID's utterance, if it's of the current generation. */
    private fun current(engineID: String): String? {
        val i = engineID.lastIndexOf('#')
        if (i < 0 || engineID.substring(i + 1).toIntOrNull() != gen) return null
        return engineID.substring(0, i)
    }

    private fun done(id: String, err: Throwable?) {
        val i = queue.indexOfFirst { it.id == id }
        if (i < 0) return
        val u = queue.removeAt(i)
        spokenFrom.remove(id)
        u.done(err)
        if (queue.isEmpty() && focused) {
            abandonFocus()
            focused = false
        }
    }

    /** Stops the engine's queue; its callbacks after are stale. */
    private fun halt() {
        gen++
        tts?.stop()
    }

    fun speak(text: String, o: Options, done: (Throwable?) -> Unit): String {
        failed?.let { done(Exception(it)); return "" }
        val u = Utterance((++next).toString(), text, o, done)
        queue.addLast(u)
        engine()
        if (ready && !paused) enqueue(u)
        return u.id
    }

    fun stop() {
        val us = queue.toList()
        queue.clear()
        spokenFrom.clear()
        paused = false
        halt()
        for (u in us) u.done(SpeechException(SpeechError.STOPPED))
        if (focused) {
            abandonFocus()
            focused = false
        }
    }

    fun pause() {
        if (!paused && queue.isNotEmpty()) {
            paused = true
            halt()
        }
    }

    fun resume() {
        if (paused) {
            paused = false
            if (ready) for (u in queue) enqueue(u)
        }
    }

    fun voices(answer: (List<Voice>) -> Unit) {
        engine()
        val list = {
            answer(tts?.voices.orEmpty().sortedBy { it.name }.map { v ->
                val q = when {
                    v.quality >= TtsVoice.QUALITY_VERY_HIGH -> Quality.PREMIUM
                    v.quality >= TtsVoice.QUALITY_HIGH -> Quality.ENHANCED
                    else -> Quality.STANDARD
                }
                Voice(id = v.name, name = v.name, language = v.locale.toLanguageTag(), quality = q, network = v.isNetworkConnectionRequired)
            })
        }
        if (ready || failed != null) list() else waitingVoices.add(list)
    }
}

private class TtsSynthesizer : Synthesizer {
    override suspend fun speak(text: String, options: Options) = withContext(Dispatchers.Main) {
        suspendCancellableCoroutine { c: CancellableContinuation<kotlin.Unit> ->
            Engine.speak(text, options) { err ->
                if (!c.isActive) return@speak
                if (err == null) c.resume(kotlin.Unit) else c.resumeWithException(err)
            }
            // The task that waits went: it stops speaking.
            c.invokeOnCancellation { Handler(Looper.getMainLooper()).post { Engine.stop() } }
        }
    }

    override fun stop() = onMain { Engine.stop() }
    override fun pause() = onMain { Engine.pause() }
    override fun resume() = onMain { Engine.resume() }
    override fun speaking(): Boolean = Engine.speaking

    override suspend fun voices(): List<Voice> = withContext(Dispatchers.Main) {
        suspendCancellableCoroutine { c -> Engine.voices { if (c.isActive) c.resume(it) } }
    }

    private fun onMain(f: () -> kotlin.Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) f() else Handler(Looper.getMainLooper()).post(f)
    }
}

fun makeSynthesizer(): Synthesizer = TtsSynthesizer()
