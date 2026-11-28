package dev.asuka.video

import android.annotation.SuppressLint
import android.app.PictureInPictureParams
import android.content.Context
import android.content.pm.ActivityInfo
import android.graphics.Color
import android.graphics.Rect
import android.net.Uri
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Rational
import android.view.View
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.ImageView
import androidx.activity.OnBackPressedCallback
import androidx.appcompat.app.AppCompatActivity
import androidx.core.view.WindowCompat
import androidx.core.view.WindowInsetsCompat
import androidx.core.view.WindowInsetsControllerCompat
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MimeTypes
import androidx.media3.common.PlaybackException
import androidx.media3.common.PlaybackParameters
import androidx.media3.common.Player
import androidx.media3.common.TrackSelectionOverride
import androidx.media3.common.Tracks
import androidx.media3.common.VideoSize
import androidx.media3.common.util.UnstableApi
import androidx.media3.datasource.DefaultDataSource
import androidx.media3.datasource.DefaultHttpDataSource
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory
import androidx.media3.ui.AspectRatioFrameLayout
import androidx.media3.ui.PlayerView
import dev.asuka.AsukaVideoEvents
import dev.asuka.AsukaVideoPlayer
import dev.asuka.AsukaVideoReport
import java.io.File
import java.util.Locale

/**
 * A Video's player: Media3's ExoPlayer in its PlayerView (its controls),
 * reporting the video's size, its end, its errors, its playback
 * (VideoState) and its tracks (VideoTracks) to the host's AsukaVideo,
 * which makes it by name. Full screen and picture in picture move the
 * picture to a PlayerView of their own over the whole activity
 * (PlayerView.switchTargetView), which in picture in picture is the
 * window the system shrinks.
 */
@SuppressLint("ViewConstructor")
@androidx.annotation.OptIn(UnstableApi::class)
class AsukaExoVideo(context: Context, private val events: AsukaVideoEvents) : FrameLayout(context), AsukaVideoPlayer {
    private val app = context.applicationContext
    private val player: ExoPlayer = ExoPlayer.Builder(app).build()
    private val playerView = inflatePlayer(this)
    private val poster = ImageView(context).apply { scaleType = ImageView.ScaleType.FIT_CENTER; visibility = GONE }
    override val view: View get() = this
    override var autoPlay = false
        // The props go in the order the app wrote them, so Source may be
        // applied before AutoPlay: what's loaded plays when it comes.
        set(v) {
            if (v && !field && player.mediaItemCount > 0) player.playWhenReady = true
            field = v
        }
    override var loop = false
        set(v) { field = v; player.repeatMode = if (v) Player.REPEAT_MODE_ONE else Player.REPEAT_MODE_OFF }
    override var muted = false
        set(v) { field = v; player.volume = if (v) 0f else volume }
    private var volume = 1f
    private var fill = false
    private var headers: Map<String, String> = emptyMap()
    private var startAt = 0.0
    private var source = ""
    private var files: List<Triple<String, String, String>> = emptyList()
    private var wantSubtitle: String? = null
    private var wantAudio: String? = null
    private var subtitleApplied = false
    private var audioApplied = false
    private var interval = 0.0
    private val main = Handler(Looper.getMainLooper())
    private val tick = object : Runnable {
        override fun run() {
            if (interval <= 0 || !player.isPlaying) return
            report(force = true)
            main.postDelayed(this, (interval * 1000).toLong())
        }
    }
    private var ended = false
    private var lastState: ByteArray? = null
    private var lastTracks: List<AsukaVideoReport.Track>? = null
    private var size = 0f to 0f
    private var allowPiP = false
    private var autoPiP = false
    private var inPiP = false
    /** The PlayerView over the activity, full screen or in picture in picture. */
    private var takeover: FrameLayout? = null
    private var takeoverView: PlayerView? = null
    private var full = false
    private var orientation = ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED
    private val back = object : OnBackPressedCallback(false) {
        override fun handleOnBackPressed() = act(3)
    }

    init {
        playerView.player = player
        addView(playerView, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        addView(poster, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        playerView.setFullscreenButtonClickListener { on -> act(if (on) 2 else 3) }
        player.addListener(object : Player.Listener {
            override fun onVideoSizeChanged(size: VideoSize) {
                if (size.width == 0 || size.height == 0) return
                reportSize(size.width * size.pixelWidthHeightRatio.toDouble(), size.height.toDouble())
            }

            // The size from the track, known before the video draws: a
            // view of no size (until it's known) has no surface to draw on.
            override fun onTracksChanged(tracks: Tracks) {
                for (g in tracks.groups) {
                    if (g.type != C.TRACK_TYPE_VIDEO || !g.isSelected) continue
                    for (i in 0 until g.length) {
                        if (!g.isTrackSelected(i)) continue
                        val f = g.getTrackFormat(i)
                        if (f.width <= 0 || f.height <= 0) continue
                        val turned = f.rotationDegrees == 90 || f.rotationDegrees == 270
                        val w = f.width * f.pixelWidthHeightRatio.toDouble()
                        val h = f.height.toDouble()
                        if (turned) reportSize(h, w) else reportSize(w, h)
                        break
                    }
                }
                applyTracks(tracks)
                reportTracks(tracks)
            }

            override fun onPlaybackStateChanged(state: Int) {
                if (state == Player.STATE_ENDED) {
                    ended = true
                    events.ended()
                }
                report()
            }

            override fun onIsPlayingChanged(isPlaying: Boolean) {
                main.removeCallbacks(tick)
                if (isPlaying && interval > 0) main.postDelayed(tick, (interval * 1000).toLong())
                updatePiPParams()
                report(force = true)
            }

            override fun onPlayWhenReadyChanged(playWhenReady: Boolean, reason: Int) = report()

            override fun onPlaybackParametersChanged(p: PlaybackParameters) = report()

            override fun onRenderedFirstFrame() { poster.visibility = GONE }

            // Looping, an end is a jump back to the start; a seek, a jump too.
            override fun onPositionDiscontinuity(old: Player.PositionInfo, new: Player.PositionInfo, reason: Int) {
                if (reason == Player.DISCONTINUITY_REASON_AUTO_TRANSITION) events.ended()
                if (reason == Player.DISCONTINUITY_REASON_SEEK) ended = false
                report(force = true)
            }

            override fun onPlayerError(e: PlaybackException) = events.failed(e.message ?: e.errorCodeName)
        })
    }

    private fun reportSize(w: Double, h: Double) {
        if (size == w.toFloat() to h.toFloat()) return
        size = w.toFloat() to h.toFloat()
        events.loaded(w, h)
        updatePiPParams()
        report()
    }

    private fun uriOf(s: String): Uri = when {
        s.startsWith("http://") || s.startsWith("https://") || s.startsWith("file://") -> Uri.parse(s)
        s.startsWith("/") -> Uri.fromFile(File(s))
        else -> events.assetUri(s)
    }

    override fun load(source: String) {
        this.source = source
        size = 0f to 0f
        ended = false
        lastTracks = null
        subtitleApplied = false
        audioApplied = false
        poster.visibility = if (poster.drawable != null) VISIBLE else GONE
        if (source.isEmpty()) { player.clearMediaItems(); report(); return }
        prepare(resetPosition = true)
    }

    /** Prepares the source with its headers and subtitle files, at startAt (or where it is). */
    private fun prepare(resetPosition: Boolean) {
        val subs = files.map { (url, lang, label) ->
            MediaItem.SubtitleConfiguration.Builder(uriOf(url)).setId(url).setMimeType(MimeTypes.TEXT_VTT)
                .setLanguage(lang.ifEmpty { null }).setLabel(label.ifEmpty { null }).build()
        }
        val item = MediaItem.Builder().setUri(uriOf(source)).setSubtitleConfigurations(subs).build()
        val http = DefaultHttpDataSource.Factory().setAllowCrossProtocolRedirects(true).setDefaultRequestProperties(headers)
        val media = DefaultMediaSourceFactory(DefaultDataSource.Factory(app, http)).createMediaSource(item)
        if (resetPosition) player.setMediaSource(media, (startAt * 1000).toLong()) else player.setMediaSource(media, false)
        player.prepare()
        if (resetPosition) player.playWhenReady = autoPlay
        report()
    }

    override fun setFill(fill: Boolean) {
        this.fill = fill
        playerView.resizeMode = if (fill) AspectRatioFrameLayout.RESIZE_MODE_ZOOM else AspectRatioFrameLayout.RESIZE_MODE_FIT
        poster.scaleType = if (fill) ImageView.ScaleType.CENTER_CROP else ImageView.ScaleType.FIT_CENTER
    }

    override fun setControls(on: Boolean) { playerView.useController = on }

    override fun play() {
        if (player.playbackState == Player.STATE_ENDED) player.seekTo(0)
        ended = false
        player.play()
    }

    override fun pause() = player.pause()
    override fun seek(seconds: Double) = player.seekTo((seconds * 1000).toLong())

    override fun setRate(rate: Double) {
        player.playbackParameters = PlaybackParameters(if (rate > 0) rate.toFloat() else 1f)
    }

    override fun setVolume(volume: Double) {
        this.volume = volume.coerceIn(0.0, 1.0).toFloat()
        if (!muted) player.volume = this.volume
    }

    override fun setHeaders(headers: Map<String, String>) { this.headers = headers }

    override fun setPoster(source: String) {
        if (source.isEmpty()) { poster.setImageDrawable(null); poster.visibility = GONE; return }
        events.image(source) { b ->
            if (b == null) return@image
            poster.setImageBitmap(b)
            if (player.videoSize.width == 0 || player.currentPosition == 0L && !player.isPlaying) poster.visibility = VISIBLE
        }
    }

    override fun setStartAt(seconds: Double) { startAt = seconds }

    override fun setInterval(seconds: Double) {
        interval = seconds
        main.removeCallbacks(tick)
        if (seconds > 0 && player.isPlaying) main.postDelayed(tick, (seconds * 1000).toLong())
    }

    override fun setSubtitleFiles(files: List<Triple<String, String, String>>) {
        if (files == this.files) return
        this.files = files
        if (source.isNotEmpty() && player.mediaItemCount > 0) prepare(resetPosition = false)
    }

    // ---- Tracks ----

    override fun setSubtitle(id: String) {
        wantSubtitle = id
        subtitleApplied = false
        applyTracks(player.currentTracks)
    }

    override fun setAudioTrack(id: String) {
        wantAudio = id
        audioApplied = false
        applyTracks(player.currentTracks)
    }

    /** A track's id: its format's (a subtitle file's: its URL, which Media3 numbers), else its group's and index. */
    private fun idOf(g: Tracks.Group, gi: Int, i: Int): String {
        val id = g.getTrackFormat(i).id ?: return "$gi:$i"
        return files.firstOrNull { id == it.first || id.endsWith(":" + it.first) }?.first ?: id
    }

    /** Selects the app's choice, once its track is known. */
    private fun applyTracks(tracks: Tracks) {
        if (tracks.groups.isEmpty()) return
        val b = player.trackSelectionParameters.buildUpon()
        var changed = false
        wantSubtitle?.let { want ->
            if (subtitleApplied) return@let
            if (want.isEmpty()) {
                b.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, true)
                subtitleApplied = true
                changed = true
            } else find(tracks, C.TRACK_TYPE_TEXT, want)?.let { (g, i) ->
                b.setTrackTypeDisabled(C.TRACK_TYPE_TEXT, false).setOverrideForType(TrackSelectionOverride(g.mediaTrackGroup, i))
                subtitleApplied = true
                changed = true
            }
        }
        wantAudio?.let { want ->
            if (audioApplied) return@let
            find(tracks, C.TRACK_TYPE_AUDIO, want)?.let { (g, i) ->
                b.setOverrideForType(TrackSelectionOverride(g.mediaTrackGroup, i))
                audioApplied = true
                changed = true
            }
        }
        if (changed) player.trackSelectionParameters = b.build()
    }

    private fun find(tracks: Tracks, type: Int, id: String): Pair<Tracks.Group, Int>? {
        tracks.groups.forEachIndexed { gi, g ->
            if (g.type != type) return@forEachIndexed
            for (i in 0 until g.length) if (g.isTrackSupported(i) && idOf(g, gi, i) == id) return g to i
        }
        return null
    }

    private fun reportTracks(tracks: Tracks) {
        val ts = ArrayList<AsukaVideoReport.Track>()
        val textOff = player.trackSelectionParameters.disabledTrackTypes.contains(C.TRACK_TYPE_TEXT)
        tracks.groups.forEachIndexed { gi, g ->
            if (g.type != C.TRACK_TYPE_TEXT && g.type != C.TRACK_TYPE_AUDIO) return@forEachIndexed
            for (i in 0 until g.length) {
                if (!g.isTrackSupported(i)) continue
                val f = g.getTrackFormat(i)
                val lang = f.language?.takeIf { it != C.LANGUAGE_UNDETERMINED } ?: ""
                val label = f.label ?: lang.takeIf { it.isNotEmpty() }?.let { Locale.forLanguageTag(it).displayName } ?: ""
                val sub = g.type == C.TRACK_TYPE_TEXT
                ts.add(AsukaVideoReport.Track(sub, g.isTrackSelected(i) && !(sub && textOff), idOf(g, gi, i), label, lang))
            }
        }
        if (ts == lastTracks) return
        lastTracks = ts
        events.tracks(AsukaVideoReport.tracks(ts))
    }

    // ---- Reports ----

    private fun report(force: Boolean = false) {
        var flags = 0
        val state = player.playbackState
        val ready = player.mediaItemCount > 0 && (state == Player.STATE_READY || state == Player.STATE_ENDED ||
            state == Player.STATE_BUFFERING && player.duration != C.TIME_UNSET)
        if (ready) flags = flags or AsukaVideoReport.READY
        if (player.playWhenReady && state != Player.STATE_ENDED && state != Player.STATE_IDLE) {
            flags = flags or AsukaVideoReport.PLAYING
            if (state == Player.STATE_BUFFERING) flags = flags or AsukaVideoReport.BUFFERING
        }
        if (ended && state == Player.STATE_ENDED) flags = flags or AsukaVideoReport.ENDED
        if (inPiP) flags = flags or AsukaVideoReport.PIP
        if (full) flags = flags or AsukaVideoReport.FULL_SCREEN
        val dur = when {
            !ready -> Double.NaN
            player.isCurrentMediaItemLive || player.duration == C.TIME_UNSET -> Double.POSITIVE_INFINITY
            else -> player.duration / 1000.0
        }
        val s = AsukaVideoReport.state(flags, player.currentPosition / 1000.0, dur, player.playbackParameters.speed, size.first, size.second)
        val last = lastState
        // All but the time (bytes 1 to 8) the same: told with the next tick.
        if (!force && last != null && last[0] == s[0] && (9 until s.size).all { last[it] == s[it] }) return
        lastState = s
        events.state(s)
    }

    // ---- Full screen and picture in picture ----

    override fun setPictureInPicture(on: Boolean) {
        allowPiP = on
        updatePiPParams()
    }

    override fun setAutoPictureInPicture(on: Boolean) {
        autoPiP = on
        updatePiPParams()
    }

    private fun pipParams(): PictureInPictureParams? {
        if (Build.VERSION.SDK_INT < 26) return null
        val b = PictureInPictureParams.Builder()
        val (w, h) = size
        if (w > 0 && h > 0) {
            // Android's limits: between 1:2.39 and 2.39:1.
            val r = (w / h).coerceIn(1 / 2.39f, 2.39f)
            b.setAspectRatio(Rational((r * 1000).toInt(), 1000))
        }
        val rect = Rect()
        if (getGlobalVisibleRect(rect)) b.setSourceRectHint(rect)
        if (Build.VERSION.SDK_INT >= 31) b.setAutoEnterEnabled(allowPiP && autoPiP && player.isPlaying)
        return b.build()
    }

    /** The activity's picture in picture params: this video's, while it may (Android 12's auto-enter). */
    private fun updatePiPParams() {
        if (Build.VERSION.SDK_INT < 26 || !allowPiP) return
        val a = events.activity() ?: return
        pipParams()?.let { runCatching { a.setPictureInPictureParams(it) } }
    }

    override fun leaving(): Boolean {
        if (Build.VERSION.SDK_INT >= 31 || !allowPiP || !autoPiP || !player.isPlaying) return false
        act(0)
        return true
    }

    override fun act(action: Int) {
        val a = events.activity() ?: return
        when (action) {
            0 -> { // enter picture in picture
                if (Build.VERSION.SDK_INT < 26 || !allowPiP || inPiP) return
                if (!a.packageManager.hasSystemFeature(android.content.pm.PackageManager.FEATURE_PICTURE_IN_PICTURE)) return
                val params = pipParams() ?: return
                takeOver(a, controls = false)
                if (!runCatching { a.enterPictureInPictureMode(params) }.getOrDefault(false) && !full) giveBack(a)
            }
            1 -> { // exit: back to the app's window, at full size
                if (!inPiP) return
                a.startActivity(android.content.Intent(a, a.javaClass).addFlags(android.content.Intent.FLAG_ACTIVITY_REORDER_TO_FRONT))
            }
            2 -> { // enter full screen
                if (full) return
                full = true
                takeOver(a, controls = true)
                takeoverView?.setFullscreenButtonState(true)
                orientation = a.requestedOrientation
                val (w, h) = size
                if (w > h) a.requestedOrientation = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
                WindowCompat.getInsetsController(a.window, a.window.decorView).apply {
                    systemBarsBehavior = WindowInsetsControllerCompat.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
                    hide(WindowInsetsCompat.Type.systemBars())
                }
                back.isEnabled = true
                (a as? AppCompatActivity)?.onBackPressedDispatcher?.addCallback(back)
                report()
            }
            3 -> { // exit full screen
                if (!full) return
                full = false
                back.isEnabled = false
                back.remove()
                a.requestedOrientation = orientation
                WindowCompat.getInsetsController(a.window, a.window.decorView).show(WindowInsetsCompat.Type.systemBars())
                if (!inPiP) giveBack(a)
                report()
            }
        }
    }

    /** Moves the picture to a PlayerView over the whole activity. */
    private fun takeOver(a: android.app.Activity, controls: Boolean) {
        takeoverView?.let { it.useController = controls; return }
        val root = a.findViewById<ViewGroup>(android.R.id.content) ?: return
        val box = FrameLayout(a).apply { setBackgroundColor(Color.BLACK); isClickable = true }
        val pv = inflatePlayer(box)
        pv.useController = controls
        pv.resizeMode = AspectRatioFrameLayout.RESIZE_MODE_FIT
        pv.setFullscreenButtonClickListener { on -> act(if (on) 2 else 3) }
        box.addView(pv, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.MATCH_PARENT))
        root.addView(box, ViewGroup.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))
        PlayerView.switchTargetView(player, playerView, pv)
        takeover = box
        takeoverView = pv
    }

    /** The picture back in the view. */
    private fun giveBack(a: android.app.Activity) {
        val pv = takeoverView ?: return
        PlayerView.switchTargetView(player, pv, playerView)
        (takeover?.parent as? ViewGroup)?.removeView(takeover)
        takeover = null
        takeoverView = null
        playerView.setFullscreenButtonState(false)
        a.window.decorView.requestLayout()
    }

    override fun pictureInPictureChanged(active: Boolean) {
        val a = events.activity() ?: return
        if (active) {
            // Android 12's auto-enter, or ours: the picture fills the window.
            if (!allowPiP || takeoverView == null && !player.isPlaying) return
            takeOver(a, controls = false)
            inPiP = true
        } else {
            if (!inPiP) return
            inPiP = false
            if (full) takeoverView?.useController = true else giveBack(a)
        }
        report()
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        if (!inPiP && !full) player.pause() // gone from the screen (popped, dismissed)
    }

    override fun release() {
        main.removeCallbacks(tick)
        events.activity()?.let { if (full) act(3); giveBack(it) }
        player.release()
    }

    private companion object {
        fun inflatePlayer(parent: ViewGroup): PlayerView =
            (android.view.LayoutInflater.from(parent.context).inflate(R.layout.asuka_player, parent, false) as PlayerView).apply {
                setShowBuffering(PlayerView.SHOW_BUFFERING_WHEN_PLAYING)
                setShowSubtitleButton(true)
            }
    }
}
