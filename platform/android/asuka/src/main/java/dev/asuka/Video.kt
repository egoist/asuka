package dev.asuka

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.graphics.Bitmap
import android.view.View
import android.widget.FrameLayout
import java.io.ByteArrayOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** What a Video's player reports, and what of the host it uses. */
interface AsukaVideoEvents {
    fun loaded(width: Double, height: Double)
    fun ended()
    fun failed(message: String)
    /** VideoState's payload (AsukaVideoReport.state). */
    fun state(payload: ByteArray)
    /** VideoTracks' payload (AsukaVideoReport.tracks). */
    fun tracks(payload: ByteArray)
    /** The activity it's in: picture in picture's, full screen's. */
    fun activity(): Activity?
    /** Loads an image (a poster): an http(s) URL, a file or an asset. */
    fun image(source: String, done: (Bitmap?) -> Unit)
    /** Where an asset is: asuka dev's copy in the app's files, else the APK's. */
    fun assetUri(source: String): android.net.Uri
}

/**
 * A Video's player (platform/android/asuka-video: Media3's ExoPlayer),
 * which only the apps that show a Video are built with.
 */
interface AsukaVideoPlayer {
    val view: View
    var autoPlay: Boolean
    var loop: Boolean
    var muted: Boolean
    /** Plays source: an http(s) URL, a file, or one of the app's assets. */
    fun load(source: String)
    fun setFill(fill: Boolean)
    fun setControls(on: Boolean)
    fun play()
    fun pause()
    fun seek(seconds: Double)
    /** Frees the player: the view is destroyed. */
    fun release()
    fun setRate(rate: Double)
    fun setVolume(volume: Double)
    /** HTTP headers, for the next source. */
    fun setHeaders(headers: Map<String, String>)
    fun setPoster(source: String)
    /** Seconds into the next source it starts at. */
    fun setStartAt(seconds: Double)
    /** While it plays, VideoState about every seconds (0: only as it changes). */
    fun setInterval(seconds: Double)
    /** The subtitle track shown by its reported id ("": none). */
    fun setSubtitle(id: String)
    fun setAudioTrack(id: String)
    /** WebVTT files: their URLs, languages and labels. */
    fun setSubtitleFiles(files: List<Triple<String, String, String>>)
    fun setPictureInPicture(on: Boolean)
    fun setAutoPictureInPicture(on: Boolean)
    /** A VideoAction. */
    fun act(action: Int)
    /** The activity went into picture in picture, or out. */
    fun pictureInPictureChanged(active: Boolean) {}
    /** The user is leaving the app (onUserLeaveHint): whether it went into picture in picture. */
    fun leaving(): Boolean = false
}

/**
 * A Video: the player of asuka-video, made by name as the first Video
 * shows (the CLI builds the module into an app whose sources name Video,
 * and into asuka dev's player). Without it, a Video's source fails.
 */
@SuppressLint("ViewConstructor")
class AsukaVideo(private val rt: AsukaRuntime, private val asukaID: Int) : FrameLayout(rt.themed), AsukaVideoEvents {
    val player: AsukaVideoPlayer? = make(rt.themed, this)?.also {
        addView(it.view, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
    }

    override fun loaded(width: Double, height: Double) = rt.emit(asukaID, AsukaEventKind.LOADED) { AsukaEvents.loaded(asukaID, width, height) }
    override fun ended() = rt.emit(asukaID, AsukaEventKind.ENDED) { AsukaEvents.ended(asukaID) }
    override fun failed(message: String) = rt.emit(asukaID, AsukaEventKind.FAILED) { AsukaEvents.failed(asukaID, message) }
    override fun state(payload: ByteArray) = rt.emit(asukaID, AsukaEventKind.VIDEO_STATE) { AsukaEvents.videoState(asukaID, payload) }
    override fun tracks(payload: ByteArray) = rt.emit(asukaID, AsukaEventKind.VIDEO_TRACKS) { AsukaEvents.videoTracks(asukaID, payload) }
    override fun activity(): Activity? = rt.activity
    override fun image(source: String, done: (Bitmap?) -> Unit) = AsukaImages.load(rt.app, source) { done(it.getOrNull()?.bitmap) }
    override fun assetUri(source: String): android.net.Uri {
        val dev = java.io.File(rt.app.filesDir, "asuka/assets/$source")
        return if (dev.exists()) android.net.Uri.fromFile(dev) else android.net.Uri.parse("asset:///$source")
    }

    fun load(source: String) {
        val p = player
        if (p != null) p.load(source)
        else if (source.isNotEmpty()) failed("Video isn't in this build: the app's sources don't name it")
    }

    /** The Headers and SubtitleFiles props: lists of strings. */
    fun setBytes(prop: Int, v: ByteBuffer) {
        val p = player ?: return
        val s = decodeStrings(v)
        when (prop) {
            AsukaProp.Video.HEADERS -> p.setHeaders((0 until s.size / 2).associate { s[2 * it] to s[2 * it + 1] })
            AsukaProp.Video.SUBTITLE_FILES -> p.setSubtitleFiles((0 until s.size / 3).map { Triple(s[3 * it], s[3 * it + 1], s[3 * it + 2]) })
        }
    }

    fun release() = player?.release()

    companion object {
        private var maker: java.lang.reflect.Constructor<*>? = null
        private var looked = false

        private fun make(context: Context, events: AsukaVideoEvents): AsukaVideoPlayer? {
            if (!looked) {
                looked = true
                maker = try {
                    Class.forName("dev.asuka.video.AsukaExoVideo").getDeclaredConstructor(Context::class.java, AsukaVideoEvents::class.java)
                } catch (_: ReflectiveOperationException) {
                    null
                }
            }
            return maker?.newInstance(context, events) as? AsukaVideoPlayer
        }
    }
}

/** The Videos showing, for the activity's picture in picture. */
fun AsukaRuntime.videos(): List<AsukaVideoPlayer> = nodes.values.mapNotNull { (it.view as? AsukaVideo)?.player }

/** A Video's reports (ops/video.go): VideoState's and VideoTracks' payloads. */
object AsukaVideoReport {
    const val READY = 1
    const val PLAYING = 2
    const val BUFFERING = 4
    const val ENDED = 8
    const val PIP = 16
    const val FULL_SCREEN = 32

    fun state(flags: Int, time: Double, duration: Double, rate: Float, width: Float, height: Float): ByteArray {
        val b = ByteBuffer.allocate(29).order(ByteOrder.LITTLE_ENDIAN)
        b.put(flags.toByte()).putDouble(time).putDouble(duration).putFloat(rate).putFloat(width).putFloat(height)
        return b.array()
    }

    /** A track: subtitle (else audio), chosen, id, label, language. */
    class Track(val subtitle: Boolean, val selected: Boolean, val id: String, val label: String, val language: String) {
        override fun equals(other: Any?) = other is Track && other.subtitle == subtitle && other.selected == selected && other.id == id &&
            other.label == label && other.language == language
        override fun hashCode() = id.hashCode()
    }

    fun tracks(ts: List<Track>): ByteArray {
        val out = ByteArrayOutputStream()
        fun u32(v: Int) = out.write(ByteBuffer.allocate(4).order(ByteOrder.LITTLE_ENDIAN).putInt(v).array())
        fun str(s: String) = s.toByteArray(Charsets.UTF_8).let { u32(it.size); out.write(it) }
        u32(ts.size)
        for (t in ts) {
            out.write(if (t.subtitle) 0 else 1)
            out.write(if (t.selected) 1 else 0)
            str(t.id)
            str(t.label)
            str(t.language)
        }
        return out.toByteArray()
    }
}
