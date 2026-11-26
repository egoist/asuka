// platform/transcribe on Android: Android 13's on-device recognizer, in a
// segmented session, fed the file decoded to 16 kHz mono, or listening to
// the microphone with partial results: only ever on the device. Its
// listener runs on the main thread.
package platform.transcribe

import android.Manifest
import android.content.Intent
import android.media.AudioFormat
import android.media.MediaCodec
import android.media.MediaExtractor
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.speech.RecognitionListener
import android.speech.RecognizerIntent
import android.speech.SpeechRecognizer
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

private object Transcriber {
    private val app get() = AsukaModules.context
    var busy = false

    fun available() = SpeechRecognizer.isOnDeviceRecognitionAvailable(app)

    fun run(path: String, language: String, reply: (kotlin.Result<Result>) -> kotlin.Unit): () -> kotlin.Unit {
        busy = true
        val pipe = ParcelFileDescriptor.createPipe()
        val sr = SpeechRecognizer.createOnDeviceSpeechRecognizer(app)
        val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH)
            .putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
            .putExtra(RecognizerIntent.EXTRA_LANGUAGE, language)
            .putExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE, pipe[0])
            .putExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_CHANNEL_COUNT, 1)
            .putExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_ENCODING, AudioFormat.ENCODING_PCM_16BIT)
            .putExtra(RecognizerIntent.EXTRA_AUDIO_SOURCE_SAMPLING_RATE, 16000)
            .putExtra(RecognizerIntent.EXTRA_SEGMENTED_SESSION, RecognizerIntent.EXTRA_AUDIO_SOURCE)
        if (Build.VERSION.SDK_INT >= 34) intent.putExtra(RecognizerIntent.EXTRA_REQUEST_WORD_TIMING, true)

        val text = StringBuilder()
        val segments = ArrayList<Segment>()
        var done = false
        fun finish(err: Exception?) {
            if (done) return
            done = true
            busy = false
            runCatching { sr.destroy() }
            runCatching { pipe[0].close() }
            if (err != null) reply(kotlin.Result.failure(err))
            else reply(kotlin.Result.success(Result(text = text.toString().trim(), segments = segments, language = language)))
        }
        fun add(results: Bundle) {
            results.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull()?.takeIf { it.isNotBlank() }?.let {
                if (text.isNotEmpty()) text.append(' ')
                text.append(it.trim())
            }
            if (Build.VERSION.SDK_INT >= 34) {
                val parts = results.getParcelableArrayList(SpeechRecognizer.RECOGNITION_PARTS, android.speech.RecognitionPart::class.java)
                parts?.forEach { p -> segments.add(Segment(text = p.formattedText ?: p.rawText, start = p.timestampMillis / 1000.0, duration = 0.0)) }
            }
        }
        sr.setRecognitionListener(object : RecognitionListener {
            override fun onReadyForSpeech(params: Bundle?) {}
            override fun onBeginningOfSpeech() {}
            override fun onRmsChanged(rmsdB: Float) {}
            override fun onBufferReceived(buffer: ByteArray?) {}
            override fun onEndOfSpeech() {}
            override fun onPartialResults(partialResults: Bundle?) {}
            override fun onEvent(eventType: Int, params: Bundle?) {}
            override fun onSegmentResults(results: Bundle) { add(results) }
            override fun onEndOfSegmentedSession() { finish(null) }
            override fun onResults(results: Bundle?) { results?.let { add(it) }; finish(null) }
            override fun onError(error: Int) {
                when (error) {
                    SpeechRecognizer.ERROR_NO_MATCH, SpeechRecognizer.ERROR_SPEECH_TIMEOUT -> finish(null)
                    // Often only that its pack isn't here yet: ask for it, for next time.
                    SpeechRecognizer.ERROR_LANGUAGE_NOT_SUPPORTED -> {
                        runCatching { sr.triggerModelDownload(intent) }
                        finish(TranscribeException(TranscribeError.NOT_AVAILABLE))
                    }
                    SpeechRecognizer.ERROR_LANGUAGE_UNAVAILABLE -> {
                        runCatching { sr.triggerModelDownload(intent) }
                        finish(TranscribeException(TranscribeError.DOWNLOADING))
                    }
                    else -> finish(Exception("the recognizer failed ($error)"))
                }
            }
        })
        sr.startListening(intent)
        val main = Handler(Looper.getMainLooper())
        Thread { decode(path, pipe[1]) { e -> main.post { finish(e) } } }.start()
        return { main.post { if (!done) { runCatching { sr.cancel() }; finish(Exception("cancelled")) } } }
    }

    /** The microphone's recognizer while listening, which finish stops. */
    private var listening: SpeechRecognizer? = null

    /** Listens to the microphone on the device, calling heard with what
     *  it's heard (its partial results, then its final one). */
    fun listen(language: String, heard: (Heard) -> kotlin.Unit, reply: (kotlin.Result<kotlin.Unit>) -> kotlin.Unit): () -> kotlin.Unit {
        busy = true
        val sr = SpeechRecognizer.createOnDeviceSpeechRecognizer(app)
        listening = sr
        val intent = Intent(RecognizerIntent.ACTION_RECOGNIZE_SPEECH)
            .putExtra(RecognizerIntent.EXTRA_LANGUAGE_MODEL, RecognizerIntent.LANGUAGE_MODEL_FREE_FORM)
            .putExtra(RecognizerIntent.EXTRA_LANGUAGE, language)
            .putExtra(RecognizerIntent.EXTRA_PARTIAL_RESULTS, true)
            .putExtra(RecognizerIntent.EXTRA_PREFER_OFFLINE, true)
        if (Build.VERSION.SDK_INT >= 33) intent.putExtra(RecognizerIntent.EXTRA_ENABLE_FORMATTING, RecognizerIntent.FORMATTING_OPTIMIZE_QUALITY)
        var last = ""
        var done = false
        fun finish(err: Exception?) {
            if (done) return
            done = true
            busy = false
            if (listening === sr) listening = null
            runCatching { sr.destroy() }
            if (err != null) reply(kotlin.Result.failure(err))
            else {
                heard(Heard(text = last, final = true))
                reply(kotlin.Result.success(kotlin.Unit))
            }
        }
        fun best(b: Bundle?) = b?.getStringArrayList(SpeechRecognizer.RESULTS_RECOGNITION)?.firstOrNull()?.trim()
        sr.setRecognitionListener(object : RecognitionListener {
            override fun onReadyForSpeech(params: Bundle?) {}
            override fun onBeginningOfSpeech() {}
            override fun onRmsChanged(rmsdB: Float) {}
            override fun onBufferReceived(buffer: ByteArray?) {}
            override fun onEndOfSpeech() {}
            override fun onEvent(eventType: Int, params: Bundle?) {}
            override fun onPartialResults(partialResults: Bundle?) {
                val t = best(partialResults) ?: return
                if (t.isEmpty() || t == last) return
                last = t
                heard(Heard(text = t, final = false))
            }
            override fun onResults(results: Bundle?) {
                best(results)?.takeIf { it.isNotEmpty() }?.let { last = it }
                finish(null)
            }
            override fun onError(error: Int) {
                when (error) {
                    // Nothing said, or nothing more: what was heard is the answer.
                    SpeechRecognizer.ERROR_NO_MATCH, SpeechRecognizer.ERROR_SPEECH_TIMEOUT, SpeechRecognizer.ERROR_CLIENT -> finish(null)
                    SpeechRecognizer.ERROR_INSUFFICIENT_PERMISSIONS -> finish(TranscribeException(TranscribeError.NOT_ALLOWED))
                    SpeechRecognizer.ERROR_LANGUAGE_NOT_SUPPORTED -> {
                        runCatching { sr.triggerModelDownload(intent) }
                        finish(TranscribeException(TranscribeError.NOT_AVAILABLE))
                    }
                    SpeechRecognizer.ERROR_LANGUAGE_UNAVAILABLE -> {
                        runCatching { sr.triggerModelDownload(intent) }
                        finish(TranscribeException(TranscribeError.DOWNLOADING))
                    }
                    else -> finish(Exception("the recognizer failed ($error)"))
                }
            }
        })
        sr.startListening(intent)
        val main = Handler(Looper.getMainLooper())
        return { main.post { if (!done) { runCatching { sr.cancel() }; finish(Exception("cancelled")) } } }
    }

    /** Stops listening: the recognizer gives what it heard (onResults). */
    fun finishListening() {
        Handler(Looper.getMainLooper()).post { runCatching { listening?.stopListening() } }
    }

    /** Writes the file to out as 16-bit, 16 kHz mono PCM, and closes it. */
    private fun decode(path: String, out: ParcelFileDescriptor, failed: (Exception) -> Unit) {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        try {
            ParcelFileDescriptor.AutoCloseOutputStream(out).use { stream ->
                extractor.setDataSource(path)
                val track = (0 until extractor.trackCount).first { extractor.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true }
                extractor.selectTrack(track)
                val format = extractor.getTrackFormat(track)
                codec = MediaCodec.createDecoderByType(format.getString(MediaFormat.KEY_MIME)!!).apply { configure(format, null, null, 0); start() }
                val c = codec!!
                var rate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                var channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                val info = MediaCodec.BufferInfo()
                var inputDone = false
                var position = 0.0 // in source samples, of the next output sample
                var sourceIndex = 0L
                var last = 0
                while (true) {
                    if (!inputDone) {
                        val i = c.dequeueInputBuffer(10_000)
                        if (i >= 0) {
                            val n = extractor.readSampleData(c.getInputBuffer(i)!!, 0)
                            if (n < 0) {
                                c.queueInputBuffer(i, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                                inputDone = true
                            } else {
                                c.queueInputBuffer(i, 0, n, extractor.sampleTime, 0)
                                extractor.advance()
                            }
                        }
                    }
                    val o = c.dequeueOutputBuffer(info, 10_000)
                    if (o == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                        rate = c.outputFormat.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                        channels = c.outputFormat.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    } else if (o >= 0) {
                        val pcm = c.getOutputBuffer(o)!!.order(ByteOrder.LITTLE_ENDIAN).asShortBuffer()
                        val outBytes = ByteBuffer.allocate(pcm.remaining() * 2).order(ByteOrder.LITTLE_ENDIAN)
                        val step = rate / 16000.0
                        while (pcm.remaining() >= channels) {
                            var sum = 0
                            for (ch in 0 until channels) sum += pcm.get()
                            val sample = sum / channels
                            // Linear resampling: each output sample between last and this one.
                            while (position <= sourceIndex) {
                                val t = position - (sourceIndex - 1)
                                outBytes.putShort((last + (sample - last) * t).toInt().toShort())
                                position += step
                            }
                            last = sample
                            sourceIndex++
                        }
                        stream.write(outBytes.array(), 0, outBytes.position())
                        c.releaseOutputBuffer(o, false)
                        if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
                    }
                }
            }
        } catch (e: java.io.IOException) {
            // The recognizer closed its end: it answers for itself.
        } catch (e: Exception) {
            failed(e)
        } finally {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            extractor.release()
        }
    }
}

private class OnDeviceRecognizer : Recognizer {
    override suspend fun check(language: String): Availability = withContext(Dispatchers.Main) {
        Availability(onDevice = Transcriber.available(), language = language.ifEmpty { Locale.getDefault().toLanguageTag() })
    }

    override suspend fun file(path: String, language: String): Result = withContext(Dispatchers.Main) {
        val lang = language.ifEmpty { Locale.getDefault().toLanguageTag() }
        if (!Transcriber.available()) throw TranscribeException(TranscribeError.NOT_AVAILABLE)
        if (Transcriber.busy) throw Exception("already transcribing")
        val ok = suspendCancellableCoroutine { c -> AsukaModules.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO)) { if (c.isActive) c.resume(it) } }
        if (!ok) throw TranscribeException(TranscribeError.NOT_ALLOWED)
        suspendCancellableCoroutine { c ->
            val cancel = Transcriber.run(path.removePrefix("file://"), lang) { r -> if (c.isActive) r.fold({ c.resume(it) }, { c.resumeWithException(it) }) }
            c.invokeOnCancellation { cancel() }
        }
    }

    override suspend fun listen(language: String, heard: (Heard) -> kotlin.Unit) = withContext(Dispatchers.Main) {
        val lang = language.ifEmpty { Locale.getDefault().toLanguageTag() }
        if (!Transcriber.available()) throw TranscribeException(TranscribeError.NOT_AVAILABLE)
        if (Transcriber.busy) throw Exception("transcribe: already listening")
        val ok = suspendCancellableCoroutine { c -> AsukaModules.requestPermissions(arrayOf(Manifest.permission.RECORD_AUDIO)) { if (c.isActive) c.resume(it) } }
        if (!ok) throw TranscribeException(TranscribeError.NOT_ALLOWED)
        suspendCancellableCoroutine<kotlin.Unit> { c ->
            val cancel = Transcriber.listen(lang, heard) { r -> if (c.isActive) r.fold({ c.resume(kotlin.Unit) }, { c.resumeWithException(it) }) }
            c.invokeOnCancellation { cancel() }
        }
    }

    override fun finish() = Transcriber.finishListening()
}

fun makeRecognizer(): Recognizer = OnDeviceRecognizer()
