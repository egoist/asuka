// platform/webrtc on Android: Google's WebRTC (org.webrtc, Stream's build
// of libwebrtc). One PeerConnectionFactory for the process, its video
// coded with the device's codecs where it has them (and the software ones
// where it doesn't), sharing one EGL context with the capture and the
// views. Tracks are kept by an ID of their own ("l1", "r2"), which the
// views and the connections look them up by. While a call goes (a
// connection, or the microphone captured) the audio is in communication
// mode, with the audio focus; its route is the speaker when the camera
// was captured, the earpiece when it wasn't, until the app sets one.
// What the library says on its own threads comes to the main thread,
// kept until Asuka listens (events).
package platform.webrtc

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Matrix
import android.graphics.SurfaceTexture
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.os.SystemClock
import android.view.TextureView
import androidx.core.content.ContextCompat
import dev.asuka.AsukaModules
import dev.asuka.AsukaUnsupportedException
import kotlinx.coroutines.suspendCancellableCoroutine
import org.webrtc.AddIceObserver
import org.webrtc.AudioSource
import org.webrtc.Camera1Enumerator
import org.webrtc.Camera2Enumerator
import org.webrtc.CameraEnumerator
import org.webrtc.CameraVideoCapturer
import org.webrtc.CapturerObserver
import org.webrtc.DefaultVideoDecoderFactory
import org.webrtc.DefaultVideoEncoderFactory
import org.webrtc.EglBase
import org.webrtc.EglRenderer
import org.webrtc.GlRectDrawer
import org.webrtc.IceCandidate
import org.webrtc.JavaI420Buffer
import org.webrtc.MediaConstraints
import org.webrtc.MediaStream
import org.webrtc.MediaStreamTrack
import org.webrtc.PeerConnection
import org.webrtc.PeerConnectionFactory
import org.webrtc.RTCStats
import org.webrtc.RtpReceiver
import org.webrtc.RtpSender
import org.webrtc.RtpTransceiver
import org.webrtc.SdpObserver
import org.webrtc.SessionDescription
import org.webrtc.SurfaceTextureHelper
import org.webrtc.VideoCapturer
import org.webrtc.VideoFrame
import org.webrtc.VideoSink
import org.webrtc.VideoSource
import org.webrtc.VideoTrack
import org.webrtc.audio.JavaAudioDeviceModule
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.time.Duration.Companion.seconds
import org.webrtc.DataChannel as RtcChannel

private val main = Handler(Looper.getMainLooper())

private fun onMain(f: () -> kotlin.Unit) {
    if (Looper.myLooper() == Looper.getMainLooper()) f() else main.post(f)
}

// ---------------------------------------------------------------------
// The factory, the tracks, the views

private object Rtc {
    val egl: EglBase by lazy { EglBase.create() }

    val factory: PeerConnectionFactory by lazy {
        val ctx = AsukaModules.context
        PeerConnectionFactory.initialize(PeerConnectionFactory.InitializationOptions.builder(ctx).createInitializationOptions())
        val adm = JavaAudioDeviceModule.builder(ctx)
            .setUseHardwareAcousticEchoCanceler(JavaAudioDeviceModule.isBuiltInAcousticEchoCancelerSupported())
            .setUseHardwareNoiseSuppressor(JavaAudioDeviceModule.isBuiltInNoiseSuppressorSupported())
            .createAudioDeviceModule()
        PeerConnectionFactory.builder()
            .setAudioDeviceModule(adm)
            .setVideoEncoderFactory(DefaultVideoEncoderFactory(egl.eglBaseContext, true, true))
            .setVideoDecoderFactory(DefaultVideoDecoderFactory(egl.eglBaseContext))
            .createPeerConnectionFactory()
    }

    private var next = 0
    fun id(prefix: String): String = "$prefix${++next}"

    /** The tracks, local and remote, by their IDs. */
    val tracks = HashMap<String, MediaStreamTrack>()
    /** The local video tracks' capture. */
    val captures = HashMap<String, Capture>()
    val connections = LinkedHashSet<RtcConnection>()
    val views = LinkedHashSet<RtcVideoView>()

    fun register(id: String, t: MediaStreamTrack) {
        tracks[id] = t
        for (v in views.toList()) if (v.trackId == id) v.attach()
    }

    /** Forgets a track: the views showing it let it go first. */
    fun unregister(id: String) {
        for (v in views.toList()) if (v.trackId == id) v.detach()
        tracks.remove(id)
    }
}

/** A local video track's source: the camera, or a test pattern. */
private class Capture(
    val capturer: VideoCapturer,
    val source: VideoSource,
    val helper: SurfaceTextureHelper,
    var facing: Facing,
) {
    fun stop() {
        try { capturer.stopCapture() } catch (_: InterruptedException) {}
        capturer.dispose()
        helper.dispose()
        source.dispose()
    }
}

// ---------------------------------------------------------------------
// The call's audio: communication mode, the focus, the route

private object CallAudio {
    private val am get() = AsukaModules.context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var holders = 0
    private var savedMode = AudioManager.MODE_NORMAL
    private var focus: AudioFocusRequest? = null
    private val focusListener = AudioManager.OnAudioFocusChangeListener {}
    /** The route the app chose, else none: the default (the speaker with video). */
    private var chosen: AudioRoute? = null

    /** A call holds the audio while it goes: a connection, the microphone. */
    fun hold() {
        if (holders++ > 0) {
            apply()
            return
        }
        val a = am
        savedMode = a.mode
        a.mode = AudioManager.MODE_IN_COMMUNICATION
        val attrs = AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build()
        if (Build.VERSION.SDK_INT >= 26) {
            val r = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN_TRANSIENT).setAudioAttributes(attrs).setOnAudioFocusChangeListener(focusListener).build()
            focus = r
            a.requestAudioFocus(r)
        } else {
            @Suppress("DEPRECATION")
            a.requestAudioFocus(focusListener, AudioManager.STREAM_VOICE_CALL, AudioManager.AUDIOFOCUS_GAIN_TRANSIENT)
        }
        apply()
    }

    fun release() {
        if (holders == 0 || --holders > 0) return
        val a = am
        if (Build.VERSION.SDK_INT >= 31) {
            a.clearCommunicationDevice()
        } else {
            @Suppress("DEPRECATION")
            a.isSpeakerphoneOn = false
            @Suppress("DEPRECATION")
            if (a.isBluetoothScoOn) { a.isBluetoothScoOn = false; a.stopBluetoothSco() }
        }
        if (Build.VERSION.SDK_INT >= 26) focus?.let { a.abandonAudioFocusRequest(it) }
        else @Suppress("DEPRECATION") a.abandonAudioFocus(focusListener)
        focus = null
        a.mode = savedMode
        chosen = null
    }

    /** The route: the one chosen, if it's there, else the speaker with video and the earpiece without (a headset or Bluetooth's when one's connected). */
    private fun apply() {
        val rs = routes()
        val want = chosen?.takeIf { it in rs } ?: when {
            AudioRoute.BLUETOOTH in rs -> AudioRoute.BLUETOOTH
            AudioRoute.HEADSET in rs -> AudioRoute.HEADSET
            Rtc.captures.isNotEmpty() || AudioRoute.EARPIECE !in rs -> AudioRoute.SPEAKER
            else -> AudioRoute.EARPIECE
        }
        route(want)
    }

    private fun kindOf(d: AudioDeviceInfo): AudioRoute? = when (d.type) {
        AudioDeviceInfo.TYPE_BUILTIN_EARPIECE -> AudioRoute.EARPIECE
        AudioDeviceInfo.TYPE_BUILTIN_SPEAKER -> AudioRoute.SPEAKER
        AudioDeviceInfo.TYPE_WIRED_HEADSET, AudioDeviceInfo.TYPE_WIRED_HEADPHONES, AudioDeviceInfo.TYPE_USB_HEADSET -> AudioRoute.HEADSET
        AudioDeviceInfo.TYPE_BLUETOOTH_SCO -> AudioRoute.BLUETOOTH
        else -> if (Build.VERSION.SDK_INT >= 31 && d.type == AudioDeviceInfo.TYPE_BLE_HEADSET) AudioRoute.BLUETOOTH else null
    }

    fun routes(): List<AudioRoute> {
        val a = am
        val found = LinkedHashSet<AudioRoute>()
        if (Build.VERSION.SDK_INT >= 31) {
            for (d in a.availableCommunicationDevices) kindOf(d)?.let { found.add(it) }
        } else {
            if (AsukaModules.context.packageManager.hasSystemFeature(PackageManager.FEATURE_TELEPHONY)) found.add(AudioRoute.EARPIECE)
            found.add(AudioRoute.SPEAKER)
            for (d in a.getDevices(AudioManager.GET_DEVICES_OUTPUTS)) kindOf(d)?.let { if (it != AudioRoute.SPEAKER && it != AudioRoute.EARPIECE) found.add(it) }
        }
        return listOf(AudioRoute.EARPIECE, AudioRoute.SPEAKER, AudioRoute.HEADSET, AudioRoute.BLUETOOTH).filter { it in found }
    }

    fun current(): AudioRoute {
        val a = am
        if (Build.VERSION.SDK_INT >= 31) {
            return a.communicationDevice?.let { kindOf(it) } ?: AudioRoute.SPEAKER
        }
        @Suppress("DEPRECATION")
        return when {
            a.isBluetoothScoOn -> AudioRoute.BLUETOOTH
            a.isSpeakerphoneOn -> AudioRoute.SPEAKER
            AudioRoute.HEADSET in routes() -> AudioRoute.HEADSET
            AudioRoute.EARPIECE in routes() -> AudioRoute.EARPIECE
            else -> AudioRoute.SPEAKER
        }
    }

    fun choose(r: AudioRoute) {
        if (r !in routes()) return
        chosen = r
        route(r)
    }

    private fun route(r: AudioRoute) {
        val a = am
        if (Build.VERSION.SDK_INT >= 31) {
            a.availableCommunicationDevices.firstOrNull { kindOf(it) == r }?.let { a.setCommunicationDevice(it) }
            return
        }
        @Suppress("DEPRECATION")
        when (r) {
            AudioRoute.BLUETOOTH -> { a.isSpeakerphoneOn = false; a.startBluetoothSco(); a.isBluetoothScoOn = true }
            AudioRoute.SPEAKER -> { if (a.isBluetoothScoOn) { a.isBluetoothScoOn = false; a.stopBluetoothSco() }; a.isSpeakerphoneOn = true }
            else -> { if (a.isBluetoothScoOn) { a.isBluetoothScoOn = false; a.stopBluetoothSco() }; a.isSpeakerphoneOn = false }
        }
    }
}

// ---------------------------------------------------------------------
// Permissions

private fun isGranted(p: String) = ContextCompat.checkSelfPermission(AsukaModules.context, p) == PackageManager.PERMISSION_GRANTED

/** Asks for a permission (if not granted): whether it is. */
private suspend fun ask(p: String): Boolean {
    if (isGranted(p)) return true
    return suspendCancellableCoroutine { c ->
        AsukaModules.requestPermissions(arrayOf(p)) { ok -> if (c.isActive) c.resume(ok && isGranted(p)) }
    }
}

// ---------------------------------------------------------------------
// The engine

private class RtcEngine : Engine {
    override suspend fun capture(audio: Audio?, video: Video?): Media {
        val ctx = AsukaModules.context
        if (audio != null && !ask(Manifest.permission.RECORD_AUDIO)) throw WebRTCException(WebRTCError.NOT_ALLOWED)
        if (video != null && !video.pattern && !ask(Manifest.permission.CAMERA)) throw WebRTCException(WebRTCError.NOT_ALLOWED)
        val f = Rtc.factory
        var videoTrack: Track? = null
        if (video != null) {
            val w = video.width.toInt().coerceIn(160, 3840)
            val h = video.height.toInt().coerceIn(120, 2160)
            val fps = video.frameRate.toInt().coerceIn(1, 60)
            var facing = video.facing
            val capturer: VideoCapturer = if (video.pattern) PatternCapturer() else {
                val e: CameraEnumerator = if (Camera2Enumerator.isSupported(ctx)) Camera2Enumerator(ctx) else Camera1Enumerator(true)
                val names = e.deviceNames
                val name = names.firstOrNull { if (facing == Facing.FRONT) e.isFrontFacing(it) else e.isBackFacing(it) } ?: names.firstOrNull()
                    ?: throw WebRTCException(WebRTCError.NO_DEVICE)
                facing = if (e.isFrontFacing(name)) Facing.FRONT else Facing.BACK
                e.createCapturer(name, null) ?: throw WebRTCException(WebRTCError.NO_DEVICE)
            }
            val source = f.createVideoSource(false)
            val helper = SurfaceTextureHelper.create("asuka-webrtc-capture", Rtc.egl.eglBaseContext)
            capturer.initialize(helper, ctx, source.capturerObserver)
            capturer.startCapture(w, h, fps)
            val id = Rtc.id("l")
            val t = f.createVideoTrack(id, source)
            Rtc.captures[id] = Capture(capturer, source, helper, facing)
            Rtc.register(id, t)
            videoTrack = Track(id = id, kind = TrackKind.VIDEO, remote = false, facing = facing)
        }
        var audioTrack: Track? = null
        if (audio != null) {
            val c = MediaConstraints()
            fun on(k: String, v: Boolean) = c.mandatory.add(MediaConstraints.KeyValuePair(k, if (v) "true" else "false"))
            on("googEchoCancellation", audio.echoCancellation)
            on("googNoiseSuppression", audio.noiseSuppression)
            on("googAutoGainControl", audio.autoGainControl)
            on("googHighpassFilter", audio.noiseSuppression)
            val source = f.createAudioSource(c)
            val id = Rtc.id("l")
            val t = f.createAudioTrack(id, source)
            audioSources[id] = source
            Rtc.register(id, t)
            CallAudio.hold()
            audioTrack = Track(id = id, kind = TrackKind.AUDIO, remote = false)
        }
        return Media(audio = audioTrack, video = videoTrack)
    }

    private val audioSources = HashMap<String, AudioSource>()

    override fun setEnabled(track: String, enabled: Boolean) {
        Rtc.tracks[track]?.let { if (!it.isDisposed) it.setEnabled(enabled) }
    }

    override fun stopTrack(track: String) {
        val t = Rtc.tracks[track] ?: return
        if (track.startsWith("r")) {
            t.setEnabled(false) // the other peer's: it stops showing and playing
            return
        }
        for (c in Rtc.connections) c.forget(track)
        Rtc.unregister(track)
        Rtc.captures.remove(track)?.stop()
        val audio = audioSources.remove(track)
        t.dispose()
        if (audio != null) {
            audio.dispose()
            CallAudio.release()
        }
    }

    override suspend fun switchCamera(track: String): Facing {
        val c = Rtc.captures[track] ?: throw AsukaUnsupportedException("not a camera's track")
        val cam = c.capturer as? CameraVideoCapturer ?: return c.facing
        return suspendCancellableCoroutine { k ->
            cam.switchCamera(object : CameraVideoCapturer.CameraSwitchHandler {
                override fun onCameraSwitchDone(front: Boolean) = onMain {
                    c.facing = if (front) Facing.FRONT else Facing.BACK
                    if (k.isActive) k.resume(c.facing)
                }
                override fun onCameraSwitchError(error: String) = onMain {
                    if (k.isActive) k.resumeWithException(IllegalStateException("webrtc: $error"))
                }
            })
        }
    }

    override fun connect(config: Config): Connection = RtcConnection(config)

    override fun audioRoutes(): List<AudioRoute> = CallAudio.routes()

    override fun audioRoute(): AudioRoute = CallAudio.current()

    override fun setAudioRoute(route: AudioRoute) = CallAudio.choose(route)
}

private val engine by lazy { RtcEngine() }

fun makeEngine(): Engine = engine

fun makeConnection(): Connection = throw AsukaUnsupportedException("a connection is made by Engine.connect")

fun makeDataChannel(): DataChannel = throw AsukaUnsupportedException("a data channel is made by a connection")

/**
 * A test pattern, for a device without a camera and for tests: colored
 * bars that move, and a white square that crosses them, at the size and
 * rate asked.
 */
private class PatternCapturer : VideoCapturer {
    private var observer: CapturerObserver? = null
    private var thread: HandlerThread? = null
    private var handler: Handler? = null
    private var w = 640
    private var h = 480
    private var interval = 33L
    private var n = 0

    override fun initialize(helper: SurfaceTextureHelper, ctx: Context, observer: CapturerObserver) { this.observer = observer }

    override fun startCapture(width: Int, height: Int, fps: Int) {
        // Even sizes for I420, no bigger than 640×480 and 15 frames a
        // second: it's drawn on the CPU, and a test needs no more.
        val s = minOf(1.0, 640.0 / maxOf(width, height), 480.0 / minOf(width, height))
        w = maxOf(2, ((width * s).toInt() / 2) * 2)
        h = maxOf(2, ((height * s).toInt() / 2) * 2)
        rows = emptyArray()
        interval = 1000L / fps.coerceIn(1, 15)
        val t = HandlerThread("asuka-webrtc-pattern").also { it.start() }
        thread = t
        handler = Handler(t.looper).also { it.post(::frame) }
        observer?.onCapturerStarted(true)
    }

    private val bars = arrayOf(
        intArrayOf(235, 128, 128), intArrayOf(210, 16, 146), intArrayOf(170, 166, 16), intArrayOf(145, 54, 34),
        intArrayOf(106, 202, 222), intArrayOf(81, 90, 240), intArrayOf(41, 240, 110), intArrayOf(16, 128, 128),
    )

    // A row of each plane, without the square and with it: the frame is
    // those rows copied, so drawing one costs a few copies a row.
    private var rows: Array<ByteArray> = emptyArray()

    private fun frame() {
        val o = observer ?: return
        val b = JavaI420Buffer.allocate(w, h)
        val shift = (n * 4) % w
        val box = h / 4
        val bx = (n * 6) % (w + box) - box
        val by = (h - box) / 2
        val cw = w / 2
        if (rows.isEmpty()) rows = Array(6) { ByteArray(if (it < 2) w else cw) }
        val yPlain = rows[0]; val yBox = rows[1]; val uPlain = rows[2]; val uBox = rows[3]; val vPlain = rows[4]; val vBox = rows[5]
        for (col in 0 until w) {
            val bar = bars[((col + shift) % w) * 8 / w]
            yPlain[col] = bar[0].toByte()
            yBox[col] = if (col >= bx && col < bx + box) 235.toByte() else bar[0].toByte()
        }
        for (col in 0 until cw) {
            val bar = bars[((col * 2 + shift) % w) * 8 / w]
            val inBox = col * 2 >= bx && col * 2 < bx + box
            uPlain[col] = bar[1].toByte(); uBox[col] = if (inBox) 128.toByte() else bar[1].toByte()
            vPlain[col] = bar[2].toByte(); vBox[col] = if (inBox) 128.toByte() else bar[2].toByte()
        }
        val y = b.dataY; val u = b.dataU; val v = b.dataV
        for (row in 0 until h) {
            y.position(row * b.strideY)
            y.put(if (row >= by && row < by + box) yBox else yPlain)
        }
        for (row in 0 until h / 2) {
            val inBox = row * 2 >= by && row * 2 < by + box
            u.position(row * b.strideU); u.put(if (inBox) uBox else uPlain)
            v.position(row * b.strideV); v.put(if (inBox) vBox else vPlain)
        }
        y.rewind(); u.rewind(); v.rewind()
        n++
        val f = VideoFrame(b, 0, SystemClock.elapsedRealtimeNanos())
        o.onFrameCaptured(f)
        f.release()
        handler?.postDelayed(::frame, interval)
    }

    override fun stopCapture() {
        val t = thread ?: return
        thread = null
        val done = CountDownLatch(1)
        handler?.post { handler?.removeCallbacksAndMessages(null); done.countDown() }
        done.await(1, TimeUnit.SECONDS)
        handler = null
        t.quitSafely()
        observer?.onCapturerStopped()
    }

    override fun changeCaptureFormat(width: Int, height: Int, fps: Int) {
        stopCapture()
        startCapture(width, height, fps)
    }

    override fun dispose() { observer = null }

    override fun isScreencast() = false
}

// ---------------------------------------------------------------------
// Connections

private fun sdpType(k: SdpKind): SessionDescription.Type = when (k) {
    SdpKind.ANSWER -> SessionDescription.Type.ANSWER
    SdpKind.PRANSWER -> SessionDescription.Type.PRANSWER
    SdpKind.ROLLBACK -> SessionDescription.Type.ROLLBACK
    else -> SessionDescription.Type.OFFER
}

private fun sdpKind(t: SessionDescription.Type): SdpKind = when (t) {
    SessionDescription.Type.ANSWER -> SdpKind.ANSWER
    SessionDescription.Type.PRANSWER -> SdpKind.PRANSWER
    SessionDescription.Type.ROLLBACK -> SdpKind.ROLLBACK
    else -> SdpKind.OFFER
}

private fun connectionState(s: PeerConnection.PeerConnectionState): ConnectionState = when (s) {
    PeerConnection.PeerConnectionState.CONNECTING -> ConnectionState.CONNECTING
    PeerConnection.PeerConnectionState.CONNECTED -> ConnectionState.CONNECTED
    PeerConnection.PeerConnectionState.DISCONNECTED -> ConnectionState.DISCONNECTED
    PeerConnection.PeerConnectionState.FAILED -> ConnectionState.FAILED
    PeerConnection.PeerConnectionState.CLOSED -> ConnectionState.CLOSED
    else -> ConnectionState.NEW
}

private fun iceState(s: PeerConnection.IceConnectionState): IceState = when (s) {
    PeerConnection.IceConnectionState.CHECKING -> IceState.CHECKING
    PeerConnection.IceConnectionState.CONNECTED -> IceState.CONNECTED
    PeerConnection.IceConnectionState.COMPLETED -> IceState.COMPLETED
    PeerConnection.IceConnectionState.DISCONNECTED -> IceState.DISCONNECTED
    PeerConnection.IceConnectionState.FAILED -> IceState.FAILED
    PeerConnection.IceConnectionState.CLOSED -> IceState.CLOSED
    else -> IceState.NEW
}

private fun signalingState(s: PeerConnection.SignalingState): SignalingState = when (s) {
    PeerConnection.SignalingState.HAVE_LOCAL_OFFER -> SignalingState.HAVE_LOCAL_OFFER
    PeerConnection.SignalingState.HAVE_REMOTE_OFFER -> SignalingState.HAVE_REMOTE_OFFER
    PeerConnection.SignalingState.HAVE_LOCAL_PRANSWER -> SignalingState.HAVE_LOCAL_PRANSWER
    PeerConnection.SignalingState.HAVE_REMOTE_PRANSWER -> SignalingState.HAVE_REMOTE_PRANSWER
    PeerConnection.SignalingState.CLOSED -> SignalingState.CLOSED
    else -> SignalingState.STABLE
}

private fun gatheringState(s: PeerConnection.IceGatheringState): GatheringState = when (s) {
    PeerConnection.IceGatheringState.GATHERING -> GatheringState.GATHERING
    PeerConnection.IceGatheringState.COMPLETE -> GatheringState.COMPLETE
    else -> GatheringState.NEW
}

/** An SdpObserver that answers a coroutine once. */
private open class Answer<T>(private val k: kotlinx.coroutines.CancellableContinuation<T>) : SdpObserver {
    override fun onCreateSuccess(d: SessionDescription?) {}
    override fun onSetSuccess() {}
    override fun onCreateFailure(e: String?) = fail(e)
    override fun onSetFailure(e: String?) = fail(e)
    fun done(v: T) = onMain { if (k.isActive) k.resume(v) }
    fun fail(e: String?) = onMain { if (k.isActive) k.resumeWithException(IllegalStateException("webrtc: ${e ?: "failed"}")) }
}

private class RtcConnection(config: Config) : Connection, PeerConnection.Observer {
    private val pc: PeerConnection
    private var sink: ((PeerEvent) -> kotlin.Unit)? = null
    private val pending = ArrayList<PeerEvent>()
    private var waiting: kotlinx.coroutines.CancellableContinuation<kotlin.Unit>? = null
    private var closed = false
    /** What sends each local track, by its ID. */
    private val senders = HashMap<String, RtpSender>()
    /** The other peer's tracks: our IDs, by their receivers'. */
    private val remote = HashMap<String, String>()
    /** Channels the other peer opened, until Asuka takes them. */
    private val offered = HashMap<Long, RtcDataChannel>()
    private val channels = ArrayList<RtcDataChannel>()
    private var handles = 0L

    init {
        val servers = config.iceServers.map { s ->
            PeerConnection.IceServer.builder(s.urls).setUsername(s.username).setPassword(s.credential).createIceServer()
        }
        val c = PeerConnection.RTCConfiguration(servers).apply {
            sdpSemantics = PeerConnection.SdpSemantics.UNIFIED_PLAN
            iceTransportsType = if (config.policy == IcePolicy.RELAY) PeerConnection.IceTransportsType.RELAY else PeerConnection.IceTransportsType.ALL
            bundlePolicy = PeerConnection.BundlePolicy.MAXBUNDLE
            rtcpMuxPolicy = PeerConnection.RtcpMuxPolicy.REQUIRE
        }
        pc = Rtc.factory.createPeerConnection(c, this) ?: throw IllegalStateException("webrtc: couldn't make a connection")
        Rtc.connections.add(this)
        CallAudio.hold()
    }

    private fun emit(e: PeerEvent) = onMain {
        if (closed) return@onMain
        val s = sink
        if (s != null) s(e) else pending.add(e)
    }

    override suspend fun events(send: (PeerEvent) -> kotlin.Unit) {
        if (closed) return
        suspendCancellableCoroutine { k ->
            sink = send
            val p = pending.toList()
            pending.clear()
            for (e in p) send(e)
            waiting = k
            k.invokeOnCancellation { onMain { if (waiting === k) { waiting = null; sink = null } } }
        }
    }

    private fun live() {
        if (closed) throw WebRTCException(WebRTCError.CLOSED)
    }

    override fun addTrack(track: String) {
        live()
        if (senders.containsKey(track)) return
        val t = Rtc.tracks[track] ?: throw AsukaUnsupportedException("webrtc: no track $track")
        senders[track] = pc.addTrack(t, listOf("asuka")) ?: throw IllegalStateException("webrtc: couldn't add the track")
    }

    override fun removeTrack(track: String) {
        live()
        senders.remove(track)?.let { pc.removeTrack(it) }
    }

    /** A local track that stopped: it's sent no more. */
    fun forget(track: String) {
        if (closed) return
        senders.remove(track)?.let { it.setTrack(null, false); pc.removeTrack(it) }
    }

    private suspend fun create(offer: Boolean, iceRestart: Boolean): Description {
        live()
        val c = MediaConstraints()
        if (iceRestart) c.mandatory.add(MediaConstraints.KeyValuePair("IceRestart", "true"))
        return suspendCancellableCoroutine { k ->
            val o = object : Answer<Description>(k) {
                override fun onCreateSuccess(d: SessionDescription?) {
                    if (d == null) fail("no description") else done(Description(sdpKind(d.type), d.description))
                }
            }
            if (offer) pc.createOffer(o, c) else pc.createAnswer(o, c)
        }
    }

    override suspend fun createOffer(iceRestart: Boolean): Description = create(true, iceRestart)

    override suspend fun createAnswer(): Description = create(false, false)

    override suspend fun setLocal(description: Description) {
        live()
        suspendCancellableCoroutine { k ->
            pc.setLocalDescription(object : Answer<kotlin.Unit>(k) { override fun onSetSuccess() = done(kotlin.Unit) },
                SessionDescription(sdpType(description.kind), description.sdp))
        }
    }

    override suspend fun setRemote(description: Description) {
        live()
        suspendCancellableCoroutine { k ->
            pc.setRemoteDescription(object : Answer<kotlin.Unit>(k) { override fun onSetSuccess() = done(kotlin.Unit) },
                SessionDescription(sdpType(description.kind), description.sdp))
        }
    }

    override suspend fun addCandidate(candidate: Candidate) {
        live()
        if (candidate.candidate.isEmpty()) return // the end of the other's candidates
        suspendCancellableCoroutine { k ->
            pc.addIceCandidate(IceCandidate(candidate.sdpMid, candidate.sdpMLineIndex.toInt(), candidate.candidate), object : AddIceObserver {
                override fun onAddSuccess() = onMain { if (k.isActive) k.resume(kotlin.Unit) }
                override fun onAddFailure(e: String?) = onMain { if (k.isActive) k.resumeWithException(IllegalStateException("webrtc: ${e ?: "the candidate wasn't added"}")) }
            })
        }
    }

    override fun createChannel(label: String, options: ChannelOptions): DataChannel {
        live()
        val init = RtcChannel.Init().apply {
            ordered = options.ordered
            options.maxRetransmits?.let { maxRetransmits = it.toInt() }
            options.maxPacketLifeTime?.let { maxRetransmitTimeMs = it.inWholeMilliseconds.toInt() }
            protocol = options.protocol
            options.id?.let { negotiated = true; id = it.toInt() }
        }
        val dc = pc.createDataChannel(label, init) ?: throw IllegalStateException("webrtc: couldn't open the channel")
        return RtcDataChannel(dc, 0, options.ordered, options.protocol).also { channels.add(it) }
    }

    override fun remoteChannel(handle: Long): DataChannel =
        offered.remove(handle) ?: throw AsukaUnsupportedException("webrtc: no channel $handle")

    override suspend fun stats(): Stats {
        live()
        return suspendCancellableCoroutine { k ->
            pc.getStats { report -> onMain { if (k.isActive) k.resume(statsOf(report.statsMap)) } }
        }
    }

    override fun close() {
        if (closed) return
        closed = true
        Rtc.connections.remove(this)
        for (id in remote.values) Rtc.unregister(id)
        remote.clear()
        pc.close()
        for (c in channels + offered.values) c.release()
        channels.clear()
        offered.clear()
        senders.clear()
        pc.dispose()
        CallAudio.release()
        sink = null
        pending.clear()
        waiting?.let { waiting = null; if (it.isActive) it.resume(kotlin.Unit) }
    }

    // What the library says, on its signaling thread.

    override fun onSignalingChange(s: PeerConnection.SignalingState) = emit(PeerEvent(kind = EventKind.SIGNALING, signaling = signalingState(s)))
    override fun onIceConnectionChange(s: PeerConnection.IceConnectionState) = emit(PeerEvent(kind = EventKind.ICE, ice = iceState(s)))
    override fun onConnectionChange(s: PeerConnection.PeerConnectionState) = emit(PeerEvent(kind = EventKind.CONNECTION, connection = connectionState(s)))
    override fun onIceConnectionReceivingChange(receiving: Boolean) {}
    override fun onIceGatheringChange(s: PeerConnection.IceGatheringState) = emit(PeerEvent(kind = EventKind.GATHERING, gathering = gatheringState(s)))
    override fun onIceCandidate(c: IceCandidate) =
        emit(PeerEvent(kind = EventKind.CANDIDATE, candidate = Candidate(c.sdp, c.sdpMid ?: "", c.sdpMLineIndex.toLong())))
    override fun onIceCandidatesRemoved(cs: Array<out IceCandidate>?) {}
    override fun onAddStream(s: MediaStream?) {}
    override fun onRemoveStream(s: MediaStream?) {}
    override fun onRenegotiationNeeded() {}

    override fun onTrack(t: RtpTransceiver) {
        val receiver = t.receiver
        val track = receiver.track() ?: return
        val rid = receiver.id()
        onMain {
            if (closed || remote.containsKey(rid)) return@onMain
            val id = Rtc.id("r")
            remote[rid] = id
            Rtc.register(id, track)
            val kind = if (track.kind() == MediaStreamTrack.VIDEO_TRACK_KIND) TrackKind.VIDEO else TrackKind.AUDIO
            emit(PeerEvent(kind = EventKind.TRACK, track = Track(id = id, kind = kind, remote = true)))
        }
    }

    override fun onRemoveTrack(receiver: RtpReceiver) {
        val rid = receiver.id()
        onMain {
            val id = remote.remove(rid) ?: return@onMain
            val kind = if (Rtc.tracks[id]?.kind() == MediaStreamTrack.VIDEO_TRACK_KIND) TrackKind.VIDEO else TrackKind.AUDIO
            Rtc.unregister(id)
            emit(PeerEvent(kind = EventKind.TRACK_ENDED, track = Track(id = id, kind = kind, remote = true)))
        }
    }

    override fun onDataChannel(dc: RtcChannel) {
        // Its observer at once (on this thread), so nothing it says is lost.
        val ch = RtcDataChannel(dc, 0)
        onMain {
            if (closed) { ch.release(); return@onMain }
            val h = ++handles
            ch.handle = h
            offered[h] = ch
            channels.add(ch)
            emit(PeerEvent(kind = EventKind.CHANNEL, channel = ch.info()))
        }
    }
}

private fun num(m: Map<String, Any>, k: String): Long = when (val v = m[k]) {
    is Number -> v.toLong()
    else -> 0L
}

private fun dbl(m: Map<String, Any>, k: String): Double = (m[k] as? Number)?.toDouble() ?: 0.0

/** Stats from a report: the selected candidate pair's, and the media's. */
private fun statsOf(all: Map<String, RTCStats>): Stats {
    val values = all.values
    val transport = values.firstOrNull { it.type == "transport" && it.members["selectedCandidatePairId"] != null }
    val pair = transport?.let { all[it.members["selectedCandidatePairId"] as? String] }
        ?: values.firstOrNull { it.type == "candidate-pair" && it.members["nominated"] == true && it.members["state"] == "succeeded" }
    var s = Stats()
    if (pair != null) {
        val m = pair.members
        fun type(id: Any?): String = (all[id as? String]?.members?.get("candidateType") as? String) ?: ""
        s = s.copy(
            bytesSent = num(m, "bytesSent"), bytesReceived = num(m, "bytesReceived"),
            roundTripTime = dbl(m, "currentRoundTripTime").seconds,
            availableOutgoingBitrate = dbl(m, "availableOutgoingBitrate"),
            localCandidate = type(m["localCandidateId"]), remoteCandidate = type(m["remoteCandidateId"]),
        )
    }
    var sent = 0L; var received = 0L; var lost = 0L
    for (r in values) {
        val m = r.members
        when (r.type) {
            "outbound-rtp" -> sent += num(m, "packetsSent")
            "inbound-rtp" -> {
                received += num(m, "packetsReceived")
                lost += num(m, "packetsLost")
                if (m["kind"] == "video" && num(m, "frameWidth") > 0) {
                    s = s.copy(frameWidth = num(m, "frameWidth"), frameHeight = num(m, "frameHeight"), framesPerSecond = dbl(m, "framesPerSecond"))
                }
            }
        }
    }
    return s.copy(packetsSent = sent, packetsReceived = received, packetsLost = lost)
}

// ---------------------------------------------------------------------
// Data channels

/**
 * A data channel. org.webrtc doesn't say a channel's ordering or protocol:
 * one this side made has its options', one the other peer opened is
 * taken as ordered, with no protocol.
 */
private class RtcDataChannel(
    private val dc: RtcChannel,
    var handle: Long,
    private val ordered: Boolean = true,
    private val protocol: String = "",
) : DataChannel, RtcChannel.Observer {
    private var sink: ((ChannelEvent) -> kotlin.Unit)? = null
    private val pending = ArrayList<ChannelEvent>()
    private var waiting: kotlinx.coroutines.CancellableContinuation<kotlin.Unit>? = null
    private var dead = false
    private var ended = false

    init { dc.registerObserver(this) }

    private fun emit(e: ChannelEvent) = onMain {
        if (ended) return@onMain
        val s = sink
        if (s != null) s(e) else pending.add(e)
        if (e.kind == ChannelEventKind.CLOSED) finish()
    }

    /** The channel closed: what listens hears no more. */
    private fun finish() {
        ended = true
        val w = waiting
        waiting = null
        sink = null
        if (w != null && w.isActive) w.resume(kotlin.Unit)
    }

    override suspend fun events(send: (ChannelEvent) -> kotlin.Unit) {
        val p = pending.toList()
        pending.clear()
        for (e in p) send(e)
        if (ended || dead) return
        suspendCancellableCoroutine { k ->
            sink = send
            waiting = k
            k.invokeOnCancellation { onMain { if (waiting === k) { waiting = null; sink = null } } }
        }
    }

    override fun info(): ChannelInfo =
        if (dead) ChannelInfo(handle = handle, id = -1) else ChannelInfo(handle = handle, label = dc.label(), ordered = ordered, protocol = protocol, id = dc.id().toLong())

    override fun state(): ChannelState = if (dead) ChannelState.CLOSED else when (dc.state()) {
        RtcChannel.State.OPEN -> ChannelState.OPEN
        RtcChannel.State.CLOSING -> ChannelState.CLOSING
        RtcChannel.State.CLOSED -> ChannelState.CLOSED
        else -> ChannelState.CONNECTING
    }

    private fun send(b: ByteArray, binary: Boolean) {
        if (dead || dc.state() != RtcChannel.State.OPEN) throw WebRTCException(WebRTCError.NOT_OPEN)
        if (!dc.send(RtcChannel.Buffer(ByteBuffer.wrap(b), binary))) throw IllegalStateException("webrtc: couldn't send")
    }

    override fun sendText(text: String) = send(text.toByteArray(Charsets.UTF_8), false)

    override fun sendBytes(data: ByteArray) = send(data, true)

    override fun bufferedAmount(): Long = if (dead) 0 else dc.bufferedAmount()

    override fun close() {
        if (dead) return
        dc.close()
        release()
    }

    /** Lets the channel go: its connection closed, or Asuka let it go. */
    fun release() {
        if (dead) return
        dead = true
        dc.unregisterObserver()
        dc.dispose()
        if (!ended) {
            val s = sink
            val e = ChannelEvent(kind = ChannelEventKind.CLOSED)
            if (s != null) s(e) else pending.add(e)
            finish()
        }
    }

    // On the signaling thread.
    override fun onBufferedAmountChange(previous: Long) {}

    override fun onStateChange() {
        val e = when (dc.state()) {
            RtcChannel.State.OPEN -> ChannelEventKind.OPEN
            RtcChannel.State.CLOSING -> ChannelEventKind.CLOSING
            RtcChannel.State.CLOSED -> ChannelEventKind.CLOSED
            else -> return
        }
        emit(ChannelEvent(kind = e))
    }

    override fun onMessage(b: RtcChannel.Buffer) {
        val bytes = ByteArray(b.data.remaining())
        b.data.get(bytes)
        emit(if (b.binary) ChannelEvent(kind = ChannelEventKind.MESSAGE, binary = true, data = bytes)
             else ChannelEvent(kind = ChannelEventKind.MESSAGE, text = String(bytes, Charsets.UTF_8)))
    }
}

// ---------------------------------------------------------------------
// VideoView: a TextureView drawn by WebRTC's EglRenderer, so one video
// over another (a preview in a corner) composes as views do.

private class RtcVideoView(private val events: VideoViewEvents) : VideoView, TextureView.SurfaceTextureListener, VideoSink {
    private val tv = object : TextureView(events.context) {
        override fun onAttachedToWindow() { super.onAttachedToWindow(); attached() }
        override fun onDetachedFromWindow() { detached(); super.onDetachedFromWindow() }
        override fun onSizeChanged(w: Int, h: Int, ow: Int, oh: Int) { super.onSizeChanged(w, h, ow, oh); layout() }
    }
    override val view: android.view.View get() = tv

    private var renderer: EglRenderer? = null
    private var surface: SurfaceTexture? = null
    var trackId = ""
    private var track: VideoTrack? = null
    private var mirror = false
    private var fit = Fit.COVER
    // The frame's size as it's shown (rotated), from the render thread.
    @Volatile private var frameW = 0
    @Volatile private var frameH = 0

    init { tv.surfaceTextureListener = this }

    private fun attached() {
        if (renderer != null) return
        val r = EglRenderer("asuka-webrtc-view")
        r.init(Rtc.egl.eglBaseContext, EglBase.CONFIG_PLAIN, GlRectDrawer())
        r.setMirror(mirror)
        renderer = r
        surface?.let { r.createEglSurface(it) }
        Rtc.views.add(this)
        attach()
        layout()
    }

    private fun detached() {
        detach()
        Rtc.views.remove(this)
        renderer?.release()
        renderer = null
    }

    /** Shows the track its ID names, once there's one. */
    fun attach() {
        if (renderer == null) return
        val t = Rtc.tracks[trackId] as? VideoTrack ?: return
        if (t === track) return
        detach()
        track = t
        t.addSink(this)
    }

    fun detach() {
        track?.let { if (!it.isDisposed) it.removeSink(this) }
        track = null
        renderer?.clearImage()
    }

    override fun setTrack(value: Track) {
        if (value.id == trackId) return
        detach()
        trackId = value.id
        attach()
    }

    override fun setMirror(value: Boolean) {
        mirror = value
        renderer?.setMirror(value)
    }

    override fun setFit(value: Fit) {
        fit = value
        layout()
    }

    /** Cover crops the frame to the view; contain scales the view's drawing to the frame's shape. */
    private fun layout() {
        val vw = tv.width.toFloat()
        val vh = tv.height.toFloat()
        val r = renderer ?: return
        if (vw <= 0 || vh <= 0) return
        val m = Matrix()
        if (fit == Fit.CONTAIN) {
            r.setLayoutAspectRatio(0f)
            if (frameW > 0 && frameH > 0) {
                val fa = frameW.toFloat() / frameH
                val va = vw / vh
                if (fa > va) m.setScale(1f, va / fa, vw / 2, vh / 2) else m.setScale(fa / va, 1f, vw / 2, vh / 2)
            }
        } else {
            r.setLayoutAspectRatio(vw / vh)
        }
        tv.setTransform(m)
    }

    // On the decoder's or the capturer's thread.
    override fun onFrame(frame: VideoFrame) {
        val w = frame.rotatedWidth
        val h = frame.rotatedHeight
        if (w != frameW || h != frameH) {
            frameW = w
            frameH = h
            main.post {
                layout()
                events.resize(VideoSize(w.toLong(), h.toLong()))
            }
        }
        renderer?.onFrame(frame)
    }

    override fun onSurfaceTextureAvailable(st: SurfaceTexture, width: Int, height: Int) {
        surface = st
        renderer?.createEglSurface(st)
        layout()
    }

    override fun onSurfaceTextureSizeChanged(st: SurfaceTexture, width: Int, height: Int) = layout()

    override fun onSurfaceTextureDestroyed(st: SurfaceTexture): Boolean {
        surface = null
        val r = renderer ?: return true
        val done = CountDownLatch(1)
        r.releaseEglSurface { done.countDown() }
        done.await(1, TimeUnit.SECONDS)
        return true
    }

    override fun onSurfaceTextureUpdated(st: SurfaceTexture) {}
}

fun makeVideoView(events: VideoViewEvents): VideoView = RtcVideoView(events)
