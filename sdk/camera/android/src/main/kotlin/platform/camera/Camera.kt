// platform/camera on Android: the View is CameraX's PreviewView, bound to the
// activity's lifecycle with a preview, an image capture (capture), a
// video capture in video mode (record) and, with scan on or frames asked
// for, one analysis that reads codes with ML Kit and samples the frames;
// the camera is open while the view is in a window. A pinch zooms
// (setZoomRatio), a tap focuses and meters (FocusMeteringAction, through
// the PreviewView's metering points); the torch, the flash, the exposure
// bias and the zoom are the camera control's. take opens the system's
// camera app to a file of the app's, through its FileProvider.
package platform.camera

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Size
import android.view.GestureDetector
import android.view.MotionEvent
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ExperimentalGetImage
import androidx.camera.core.ExperimentalZeroShutterLag
import androidx.camera.core.FocusMeteringAction
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.camera.core.ImageProxy
import androidx.camera.core.MirrorMode
import androidx.camera.core.Preview
import androidx.camera.core.UseCase
import androidx.camera.core.resolutionselector.AspectRatioStrategy
import androidx.camera.core.resolutionselector.ResolutionSelector
import androidx.camera.core.resolutionselector.ResolutionStrategy
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.video.FallbackStrategy
import androidx.camera.video.FileOutputOptions
import androidx.camera.video.Quality
import androidx.camera.video.QualitySelector
import androidx.camera.video.Recorder
import androidx.camera.video.Recording
import androidx.camera.video.VideoCapture
import androidx.camera.video.VideoRecordEvent
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import androidx.core.content.edit
import androidx.exifinterface.media.ExifInterface
import androidx.lifecycle.Observer
import com.google.mlkit.vision.barcode.BarcodeScanner
import com.google.mlkit.vision.barcode.BarcodeScannerOptions
import com.google.mlkit.vision.barcode.BarcodeScanning
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.common.InputImage
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt
import kotlin.time.Duration.Companion.nanoseconds
import android.view.View as AndroidView

/** Where photos and videos go: the app's cache's asuka-camera. */
private fun cameraDir(): File = File(AsukaModules.context.cacheDir, "asuka-camera").apply { mkdirs() }

private fun cameraFile(ext: String): File = File(cameraDir(), UUID.randomUUID().toString() + "." + ext)

/**
 * Whether the app has asked for a permission: Android says only whether
 * it's granted, so each ask is kept in the app's preferences, by the
 * permission's name (platform/record's file too: both ask for the
 * microphone).
 */
private val asked get() = AsukaModules.context.getSharedPreferences("asuka.permissions", Context.MODE_PRIVATE)

private fun isGranted(permission: String) =
    ContextCompat.checkSelfPermission(AsukaModules.context, permission) == PackageManager.PERMISSION_GRANTED

/** Asks for a permission (if not granted), noting the ask when there's a screen to ask on. Main thread. */
private fun requestPermission(permission: String, done: (Boolean) -> kotlin.Unit) {
    val shown = AsukaModules.activity != null
    AsukaModules.requestPermissions(arrayOf(permission)) { ok ->
        if (shown) asked.edit { putBoolean(permission, true) }
        done(ok)
    }
}

/**
 * A permission now, asking nothing: "allowed"; "denied" once asked (the
 * user said no, now or for good); "notAsked" before.
 */
private fun permissionAccess(permission: String): String {
    val a = asked
    if (isGranted(permission)) {
        // Granted (here, or in Settings): taken away later, it's denied.
        if (!a.getBoolean(permission, false)) a.edit { putBoolean(permission, true) }
        return "allowed"
    }
    return if (a.getBoolean(permission, false)) "denied" else "notAsked"
}

/** The kinds of code ML Kit reads, by platform/scan's names. */
internal val formats = mapOf(
    "qr" to Barcode.FORMAT_QR_CODE, "aztec" to Barcode.FORMAT_AZTEC,
    "datamatrix" to Barcode.FORMAT_DATA_MATRIX, "pdf417" to Barcode.FORMAT_PDF417,
    "ean13" to Barcode.FORMAT_EAN_13, "ean8" to Barcode.FORMAT_EAN_8,
    "upca" to Barcode.FORMAT_UPC_A, "upce" to Barcode.FORMAT_UPC_E,
    "code128" to Barcode.FORMAT_CODE_128, "code39" to Barcode.FORMAT_CODE_39,
    "code93" to Barcode.FORMAT_CODE_93, "codabar" to Barcode.FORMAT_CODABAR,
    "itf" to Barcode.FORMAT_ITF,
)
private val kindOf = formats.entries.associate { (k, v) -> v to k }

private fun scanner(kinds: Set<String>): BarcodeScanner {
    val fs = kinds.mapNotNull { formats[it] }.ifEmpty { listOf(Barcode.FORMAT_ALL_FORMATS) }
    return BarcodeScanning.getClient(BarcodeScannerOptions.Builder().setBarcodeFormats(fs[0], *fs.drop(1).toIntArray()).build())
}

/** Repeats of a code in view, dropped: a code is reported as it comes into view, and again once it's been out of it for a second. */
private class CodeSeen {
    private val last = HashMap<String, Long>()

    fun isNew(kind: String, value: String, now: Long = SystemClock.uptimeMillis()): Boolean {
        val key = kind + "\u0000" + value
        val new = last[key]?.let { now - it > 1000 } ?: true
        last[key] = now
        if (last.size > 32) last.entries.removeAll { now - it.value > 1000 }
        return new
    }
}

/** How many frame files a View writes in turn. */
private const val FRAME_FILES = 4

/**
 * The View's one analysis: every frame it gets (the analysis keeps only
 * the latest while one is being looked at) is sampled for frame at the
 * View's frame rate, and read for codes while scan is on. Its frames are
 * written on its own thread, one at a time.
 */
private class FrameAnalyzer(
    private val owner: CameraView,
    kinds: Set<String>?,
    val found: (String, String) -> kotlin.Unit,
) : ImageAnalysis.Analyzer, AutoCloseable {
    private val client = kinds?.let { scanner(it) }
    private var last = 0L
    private var written = 0

    @ExperimentalGetImage
    override fun analyze(p: ImageProxy) {
        val now = SystemClock.elapsedRealtimeNanos()
        val rate = min(owner.frameRate, 15.0)
        if (rate > 0 && now - last >= (1e9 / rate).toLong()) {
            last = now
            runCatching { writeFrame(p, now) }
        }
        val c = client
        val img = p.image
        if (c == null || img == null) { p.close(); return }
        // Its listeners run on the main thread; the next frame waits for
        // this one (the analysis keeps only the latest).
        c.process(InputImage.fromMediaImage(img, p.imageInfo.rotationDegrees))
            .addOnSuccessListener { list ->
                for (b in list) {
                    val k = kindOf[b.format] ?: continue
                    val v = b.rawValue ?: continue
                    found(k, v)
                }
            }
            .addOnCompleteListener { p.close() }
    }

    /** The frame upright (the front camera's mirrored only with the View's mirror), its long side at most frameSize, as the next of the View's files. */
    private fun writeFrame(p: ImageProxy, now: Long) {
        val src = p.toBitmap()
        val long = max(src.width, src.height)
        val k = if (long > owner.frameSize && owner.frameSize > 0) owner.frameSize.toFloat() / long else 1f
        val m = Matrix()
        if (k < 1f) m.postScale(k, k)
        val deg = p.imageInfo.rotationDegrees
        if (deg != 0) m.postRotate(deg.toFloat())
        if (owner.front && owner.mirror) m.postScale(-1f, 1f)
        val out = if (m.isIdentity) src else Bitmap.createBitmap(src, 0, 0, src.width, src.height, m, true)
        val n = written++ % FRAME_FILES
        val f = File(cameraDir(), "frame-${owner.id}-$n.jpg")
        val tmp = File(cameraDir(), "frame-${owner.id}-$n.tmp")
        tmp.outputStream().use { out.compress(Bitmap.CompressFormat.JPEG, 80, it) }
        tmp.renameTo(f)
        val frame = Frame(path = f.absolutePath, width = out.width.toLong(), height = out.height.toLong(),
            time = (now - owner.started).nanoseconds)
        if (out !== src) out.recycle()
        src.recycle()
        owner.main.post { owner.events.frame(frame) }
    }

    override fun close() { client?.close() }
}

/**
 * A photo's file written again (capture's maxSize, quality, mirror):
 * decoded at the smallest sample still at least maxSize, turned upright
 * as its EXIF says (and flipped, a mirrored front camera's), scaled so its
 * long side is at most maxSize, a JPEG of quality.
 */
internal fun rewriteJpeg(f: File, maxSize: Int, quality: Double) {
    val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
    BitmapFactory.decodeFile(f.path, bounds)
    val long = max(bounds.outWidth, bounds.outHeight)
    var sample = 1
    if (maxSize > 0) while (long / (sample * 2) >= maxSize) sample *= 2
    var b = BitmapFactory.decodeFile(f.path, BitmapFactory.Options().apply { inSampleSize = sample }) ?: throw Exception("camera: couldn't read the photo")
    val m = Matrix()
    val k = if (maxSize > 0 && max(b.width, b.height) > maxSize) maxSize.toFloat() / max(b.width, b.height) else 1f
    if (k < 1f) m.postScale(k, k)
    when (ExifInterface(f.path).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL)) {
        ExifInterface.ORIENTATION_FLIP_HORIZONTAL -> m.postScale(-1f, 1f)
        ExifInterface.ORIENTATION_ROTATE_180 -> m.postRotate(180f)
        ExifInterface.ORIENTATION_FLIP_VERTICAL -> m.postScale(1f, -1f)
        ExifInterface.ORIENTATION_TRANSPOSE -> { m.postRotate(90f); m.postScale(-1f, 1f) }
        ExifInterface.ORIENTATION_ROTATE_90 -> m.postRotate(90f)
        ExifInterface.ORIENTATION_TRANSVERSE -> { m.postRotate(-90f); m.postScale(-1f, 1f) }
        ExifInterface.ORIENTATION_ROTATE_270 -> m.postRotate(-90f)
    }
    if (!m.isIdentity) {
        val t = Bitmap.createBitmap(b, 0, 0, b.width, b.height, m, true)
        if (t !== b) b.recycle()
        b = t
    }
    val q = if (quality > 0) (quality * 100).roundToInt().coerceIn(1, 100) else 90
    val tmp = File(f.path + ".tmp")
    tmp.outputStream().use { b.compress(Bitmap.CompressFormat.JPEG, q, it) }
    b.recycle()
    if (!tmp.renameTo(f)) throw Exception("camera: couldn't write the photo")
}

/**
 * An MP4 written again without its top-level free (and skip) boxes, the
 * room Android's muxer keeps for the index (hundreds of KB for a short
 * video): the chunk offsets (stco, co64) moved back by the bytes dropped
 * before them. Left as it is if it has none, or isn't laid out as known.
 */
internal fun compactMp4(f: File) {
    class Box(val type: String, val start: Long, val size: Long)
    val boxes = ArrayList<Box>()
    java.io.RandomAccessFile(f, "r").use { r ->
        val len = r.length()
        var at = 0L
        while (at + 8 <= len) {
            r.seek(at)
            var size = r.readInt().toLong() and 0xffffffffL
            val t = ByteArray(4).also { r.readFully(it) }
            if (size == 1L) size = r.readLong() else if (size == 0L) size = len - at
            if (size < 8 || at + size > len) return
            boxes += Box(String(t, Charsets.ISO_8859_1), at, size)
            at += size
        }
        if (at != len) return
    }
    val dropped = boxes.filter { it.type == "free" || it.type == "skip" }
    val moov = boxes.singleOrNull { it.type == "moov" } ?: return
    if (dropped.isEmpty() || moov.size > 64L * 1024 * 1024) return
    fun shift(o: Long) = o - dropped.filter { it.start < o }.sumOf { it.size }
    val index = ByteArray(moov.size.toInt())
    java.io.RandomAccessFile(f, "r").use { it.seek(moov.start); it.readFully(index) }
    val b = java.nio.ByteBuffer.wrap(index)
    /** Moves the chunk offsets in the boxes from..to of the index, looking into its containers. */
    fun patch(from: Int, to: Int) {
        var at = from
        while (at + 8 <= to) {
            val size = b.getInt(at).toLong() and 0xffffffffL
            val type = String(index, at + 4, 4, Charsets.ISO_8859_1)
            if (size < 8 || at + size > to) return
            val end = (at + size).toInt()
            when (type) {
                "moov", "trak", "mdia", "minf", "stbl", "edts" -> patch(at + 8, end)
                "stco" -> {
                    val n = b.getInt(at + 12)
                    for (i in 0 until n) {
                        val p = at + 16 + i * 4
                        b.putInt(p, shift(b.getInt(p).toLong() and 0xffffffffL).toInt())
                    }
                }
                "co64" -> {
                    val n = b.getInt(at + 12)
                    for (i in 0 until n) {
                        val p = at + 16 + i * 8
                        b.putLong(p, shift(b.getLong(p)))
                    }
                }
            }
            at = end
        }
    }
    patch(0, index.size)
    val tmp = File(f.path + ".tmp")
    java.io.RandomAccessFile(f, "r").use { r ->
        tmp.outputStream().buffered().use { o ->
            val buf = ByteArray(64 * 1024)
            for (x in boxes) {
                if (x in dropped) continue
                if (x === moov) { o.write(index); continue }
                r.seek(x.start)
                var left = x.size
                while (left > 0) {
                    val n = r.read(buf, 0, min(left, buf.size.toLong()).toInt())
                    if (n <= 0) throw java.io.EOFException()
                    o.write(buf, 0, n)
                    left -= n
                }
            }
        }
    }
    if (!tmp.renameTo(f)) tmp.delete()
}

internal class CameraView(internal val events: ViewEvents) : View {
    companion object {
        /** The View on screen, which capture and record take from. */
        var current: CameraView? = null
        private var made = 0
    }

    /** Its number, in its frames' files' names. */
    internal val id = ++made
    internal val main = Handler(Looper.getMainLooper())
    /** When it was made: its frames' times are from it. */
    internal val started = SystemClock.elapsedRealtimeNanos()

    private val preview = PreviewView(events.context).apply { scaleType = PreviewView.ScaleType.FILL_CENTER }
    override val view: AndroidView get() = preview
    private var capture: ImageCapture? = null
    private var camera: androidx.camera.core.Camera? = null
    private var provider: ProcessCameraProvider? = null
    @Volatile internal var front = false
    private var scan = false
    private var kinds = emptySet<String>()
    private var analyzer: FrameAnalyzer? = null
    private var analysis: ExecutorService? = null
    private val seen = CodeSeen()
    private var binding = false
    private var video = false
    private var videoCapture: VideoCapture<Recorder>? = null
    private var recording: Recording? = null

    private var zoom = 1.0
    private var pinch = true
    private var torch = false
    private var flash = Flash.OFF
    private var exposure = 0.0
    private var tapToFocus = true
    private var videoQuality = VideoQuality.FULL_HD
    private var photoQuality = PhotoQuality.BALANCED
    @Volatile internal var mirror = false
    @Volatile internal var frameRate = 0.0
    @Volatile internal var frameSize = 480

    /** ready is said once the preview streams after each bind that changed the camera. */
    private var readyFor: Boolean? = null
    private var readyPending = false
    private val streaming = Observer<PreviewView.StreamState> { s -> if (s == PreviewView.StreamState.STREAMING) sayReady() }

    /**
     * A pinch: the zoom it started at and its fingers' first distance.
     * Measured here rather than by ScaleGestureDetector, which ignores
     * fingers closer than about 27 mm: a camera zooms with a small pinch
     * too, as Android's camera app does.
     */
    private var pinchSpan = 0f
    private var pinchZoom = 1.0
    private var pinching = false
    private val slop = android.view.ViewConfiguration.get(events.context).scaledTouchSlop

    private fun span(e: MotionEvent): Float {
        if (e.pointerCount < 2) return 0f
        return kotlin.math.hypot(e.getX(0) - e.getX(1), e.getY(0) - e.getY(1))
    }

    private fun pinched(e: MotionEvent) {
        when (e.actionMasked) {
            MotionEvent.ACTION_POINTER_DOWN -> if (e.pointerCount == 2) {
                pinchSpan = span(e)
                pinchZoom = zoom
                pinching = false
            }
            MotionEvent.ACTION_MOVE -> if (e.pointerCount >= 2 && pinchSpan > 0) {
                val s = span(e)
                if (!pinching && kotlin.math.abs(s - pinchSpan) < slop) return
                pinching = true
                val z = clampZoom(pinchZoom * s / pinchSpan)
                if (kotlin.math.abs(z - zoom) >= 0.005) {
                    zoom = z
                    camera?.cameraControl?.setZoomRatio(z.toFloat())
                    events.zoomChange(z)
                }
            }
            MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> { pinchSpan = 0f; pinching = false }
        }
    }

    private val taps = GestureDetector(events.context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(e: MotionEvent) = tapToFocus
        override fun onSingleTapUp(e: MotionEvent): Boolean {
            if (!tapToFocus) return false
            focusAt(e.x, e.y, true)
            return true
        }
    })

    init {
        // The camera is open while the view is in a window: leaving the
        // screen closes it.
        preview.addOnAttachStateChangeListener(object : AndroidView.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: AndroidView) {
                current = this@CameraView
                (AsukaModules.activity as? ComponentActivity)?.let { preview.previewStreamState.observe(it, streaming) }
                if (!binding) bind()
            }

            override fun onViewDetachedFromWindow(v: AndroidView) = unbind()
        })
        preview.setOnTouchListener { v, e ->
            if (!pinch && !tapToFocus) return@setOnTouchListener false
            if (pinch && camera != null) pinched(e)
            // A second finger cancels the tap (GestureDetector's own rule).
            taps.onTouchEvent(e)
            if (e.actionMasked == MotionEvent.ACTION_UP) v.performClick()
            true
        }
    }

    private val analyzing get() = scan || frameRate > 0

    override fun setFacing(value: Facing) { if (front != (value == Facing.FRONT)) { front = value == Facing.FRONT; changed() } }
    override fun setMode(value: Mode) { if (video != (value == Mode.VIDEO)) { video = value == Mode.VIDEO; changed() } }
    override fun setScan(value: Boolean) { if (scan != value) { scan = value; changed() } }
    override fun setCodes(value: List<String>) { if (kinds != value.toSet()) { kinds = value.toSet(); changed() } }

    override fun setZoom(value: Double) {
        zoom = value
        val c = camera ?: return
        zoom = clampZoom(value)
        c.cameraControl.setZoomRatio(zoom.toFloat())
    }

    override fun setPinchToZoom(value: Boolean) { pinch = value }
    override fun setTorch(value: Boolean) { torch = value; applyTorch() }

    override fun setFlash(value: Flash) {
        flash = value
        capture?.flashMode = flashMode()
    }

    override fun setExposure(value: Double) { exposure = value; applyExposure() }
    override fun setTapToFocus(value: Boolean) { tapToFocus = value }
    override fun setVideoQuality(value: VideoQuality) { if (videoQuality != value) { videoQuality = value; if (video) changed() } }
    override fun setPhotoQuality(value: PhotoQuality) { if (photoQuality != value) { photoQuality = value; changed() } }
    override fun setMirror(value: Boolean) { if (mirror != value) { mirror = value; if (video) changed() } }

    override fun setFrameRate(value: Double) {
        val was = analyzing
        frameRate = if (value.isNaN() || value < 0) 0.0 else min(value, 15.0)
        if (analyzing != was) changed()
    }

    override fun setFrameSize(value: Long) {
        val v = value.coerceIn(16, 4096).toInt()
        if (v == frameSize) return
        frameSize = v
        if (frameRate > 0) changed()
    }

    private fun clampZoom(z: Double): Double {
        val s = camera?.cameraInfo?.zoomState?.value ?: return max(z, 0.0)
        return z.coerceIn(s.minZoomRatio.toDouble(), s.maxZoomRatio.toDouble())
    }

    private fun flashMode() = when (flash) {
        Flash.ON -> ImageCapture.FLASH_MODE_ON
        Flash.AUTO -> ImageCapture.FLASH_MODE_AUTO
        else -> ImageCapture.FLASH_MODE_OFF
    }

    private fun applyTorch() {
        val c = camera ?: return
        if (c.cameraInfo.hasFlashUnit()) c.cameraControl.enableTorch(torch)
    }

    private fun applyExposure() {
        val c = camera ?: return
        val es = c.cameraInfo.exposureState
        if (!es.isExposureCompensationSupported) return
        val step = es.exposureCompensationStep.toDouble()
        if (step <= 0) return
        val i = (exposure / step).roundToInt().coerceIn(es.exposureCompensationRange.lower, es.exposureCompensationRange.upper)
        if (i != es.exposureCompensationIndex) c.cameraControl.setExposureCompensationIndex(i)
    }

    /** Focuses and meters at a point of the view (its pixels): a tap's, which says it with focusPoint, or camera.focus's. */
    internal fun focusAt(x: Float, y: Float, report: Boolean) {
        val c = camera ?: return
        if (preview.width <= 0 || preview.height <= 0) return
        val pt = preview.meteringPointFactory.createPoint(x, y)
        // Its regions are dropped after 5 seconds (CameraX's default), and
        // the continuous focus the camera had resumes, as in Android's
        // camera apps.
        val action = FocusMeteringAction.Builder(pt, FocusMeteringAction.FLAG_AF or FocusMeteringAction.FLAG_AE or FocusMeteringAction.FLAG_AWB).build()
        if (!c.cameraInfo.isFocusMeteringSupported(action)) return
        c.cameraControl.startFocusAndMetering(action)
        if (report) events.focusPoint(Point(x = (x / preview.width).toDouble().coerceIn(0.0, 1.0), y = (y / preview.height).toDouble().coerceIn(0.0, 1.0)))
    }

    fun focus(x: Double, y: Double) = focusAt((x * preview.width).toFloat(), (y * preview.height).toFloat(), false)

    /** Binds once for the props set together. */
    private fun changed() {
        if (binding) return
        binding = true
        preview.post {
            binding = false
            if (preview.isAttachedToWindow) bind()
        }
    }

    private fun unbind() {
        if (current === this) current = null
        preview.previewStreamState.removeObserver(streaming)
        provider?.unbindAll()
        provider = null
        capture = null
        camera = null
        videoCapture = null
        readyFor = null
        readyPending = false
        analyzer?.close()
        analyzer = null
        analysis?.shutdown()
        analysis = null
    }

    @androidx.annotation.OptIn(ExperimentalZeroShutterLag::class)
    private fun bind() {
        val act = AsukaModules.activity as? ComponentActivity ?: return
        requestPermission(Manifest.permission.CAMERA) { ok ->
            if (!ok) {
                events.error("not allowed")
                return@requestPermission
            }
            val future = ProcessCameraProvider.getInstance(act)
            future.addListener({
                if (!preview.isAttachedToWindow) return@addListener // gone meanwhile
                try {
                    val provider = future.get()
                    this.provider = provider
                    val selector = if (front) CameraSelector.DEFAULT_FRONT_CAMERA else CameraSelector.DEFAULT_BACK_CAMERA
                    if (!provider.hasCamera(selector)) {
                        events.error("no camera")
                        return@addListener
                    }
                    val info = provider.getCameraInfo(selector)
                    // In video mode the preview (and the frames) are the
                    // video's shape, 16:9; photos' 4:3 otherwise.
                    val ratio = if (video) AspectRatioStrategy.RATIO_16_9_FALLBACK_AUTO_STRATEGY else AspectRatioStrategy.RATIO_4_3_FALLBACK_AUTO_STRATEGY
                    val p = Preview.Builder().setResolutionSelector(ResolutionSelector.Builder().setAspectRatioStrategy(ratio).build()).build()
                        .also { it.surfaceProvider = preview.surfaceProvider }
                    val mode = when (photoQuality) {
                        PhotoQuality.QUALITY -> ImageCapture.CAPTURE_MODE_MAXIMIZE_QUALITY
                        // Zero shutter lag where the camera has it (CameraX
                        // turns it off itself with the flash, or a video).
                        PhotoQuality.SPEED -> if (info.isZslSupported) ImageCapture.CAPTURE_MODE_ZERO_SHUTTER_LAG else ImageCapture.CAPTURE_MODE_MINIMIZE_LATENCY
                        else -> ImageCapture.CAPTURE_MODE_MINIMIZE_LATENCY
                    }
                    val c = ImageCapture.Builder().setCaptureMode(mode).setFlashMode(flashMode()).build()
                    provider.unbindAll()
                    camera = null
                    analyzer?.close()
                    analyzer = null
                    val cases = mutableListOf<UseCase>(p, c)
                    if (analyzing) {
                        val a = FrameAnalyzer(this, if (scan) kinds else null) { k, v -> if (seen.isNew(k, v)) events.code(Code(kind = k, value = v)) }
                        val ex = analysis ?: Executors.newSingleThreadExecutor().also { analysis = it }
                        val rs = ResolutionSelector.Builder().setAspectRatioStrategy(ratio)
                        // Frames larger than the analysis's 640×480 ask for more.
                        if (frameRate > 0 && frameSize > 640) {
                            val long = frameSize
                            val short = if (video) long * 9 / 16 else long * 3 / 4
                            rs.setResolutionStrategy(ResolutionStrategy(Size(long, short), ResolutionStrategy.FALLBACK_RULE_CLOSEST_HIGHER_THEN_LOWER))
                        }
                        val ia = ImageAnalysis.Builder().setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST)
                            .setResolutionSelector(rs.build()).build()
                        ia.setAnalyzer(ex, a)
                        analyzer = a
                        cases += ia
                    }
                    val vc = if (video) {
                        val q = when (videoQuality) {
                            VideoQuality.SD -> Quality.SD
                            VideoQuality.HD -> Quality.HD
                            VideoQuality.UHD -> Quality.UHD
                            else -> Quality.FHD
                        }
                        val rec = Recorder.Builder().setQualitySelector(QualitySelector.from(q, FallbackStrategy.lowerQualityOrHigherThan(q))).build()
                        VideoCapture.Builder(rec).setMirrorMode(if (mirror) MirrorMode.MIRROR_MODE_ON_FRONT_ONLY else MirrorMode.MIRROR_MODE_OFF).build()
                    } else null
                    if (vc != null) cases += vc
                    val cam = provider.bindToLifecycle(act, selector, *cases.toTypedArray())
                    camera = cam
                    capture = c
                    videoCapture = vc
                    // What the props asked for, on this camera.
                    zoom = clampZoom(zoom)
                    cam.cameraControl.setZoomRatio(zoom.toFloat())
                    applyTorch()
                    applyExposure()
                    if (readyFor != front) {
                        readyFor = front
                        readyPending = true
                        if (preview.previewStreamState.value == PreviewView.StreamState.STREAMING) sayReady()
                    }
                } catch (e: Exception) {
                    events.error(e.message ?: "no camera")
                }
            }, ContextCompat.getMainExecutor(act))
        }
    }

    /** ready, with what the camera can do: once it streams. */
    private fun sayReady() {
        if (!readyPending) return
        val c = camera ?: return
        readyPending = false
        val i = c.cameraInfo
        val z = i.zoomState.value
        val es = i.exposureState
        val step = es.exposureCompensationStep.toDouble()
        val ev = es.isExposureCompensationSupported && step > 0
        val f = if (preview.width > 0) preview.meteringPointFactory.createPoint(preview.width / 2f, preview.height / 2f) else null
        events.ready(Info(
            minZoom = z?.minZoomRatio?.toDouble() ?: 1.0, maxZoom = z?.maxZoomRatio?.toDouble() ?: 1.0,
            minExposure = if (ev) es.exposureCompensationRange.lower * step else 0.0,
            maxExposure = if (ev) es.exposureCompensationRange.upper * step else 0.0,
            torch = i.hasFlashUnit(), flash = i.hasFlashUnit(),
            focus = f != null && i.isFocusMeteringSupported(FocusMeteringAction.Builder(f, FocusMeteringAction.FLAG_AF).build()),
        ))
    }

    /** Records to a file until stopRecording (or maxMs or maxBytes, if not 0), with the microphone unless mute (or not allowed). */
    fun record(mute: Boolean, maxMs: Long, maxBytes: Long, done: (Result<String>) -> kotlin.Unit) {
        if (capture == null) return done(Result.failure(CameraException(CameraError.NO_VIEW)))
        val vc = videoCapture ?: return done(Result.failure(Exception("the camera view isn't in video mode")))
        if (recording != null) return done(Result.failure(Exception("already recording")))
        val mic = Manifest.permission.RECORD_AUDIO
        if (mute || !AsukaModules.declares(mic)) startRecording(vc, false, maxMs, maxBytes, done)
        else requestPermission(mic) { ok -> startRecording(vc, ok, maxMs, maxBytes, done) }
    }

    // The permission is checked: withAudioEnabled is only called with it granted.
    @SuppressLint("MissingPermission")
    private fun startRecording(vc: VideoCapture<Recorder>, sound: Boolean, maxMs: Long, maxBytes: Long, done: (Result<String>) -> kotlin.Unit) {
        if (videoCapture !== vc) return done(Result.failure(CameraException(CameraError.NO_VIEW)))
        val app = AsukaModules.context
        val f = cameraFile("mp4")
        val opts = FileOutputOptions.Builder(f).apply {
            if (maxMs > 0) setDurationLimitMillis(maxMs)
            // CameraX counts the media's bytes; the movie's index (moov)
            // comes on top, a few bytes per frame: kept out of maxBytes.
            if (maxBytes > 0) setFileSizeLimit(max(maxBytes / 2, maxBytes - 16 * 1024 - maxBytes / 50))
        }.build()
        var pending = vc.output.prepareRecording(app, opts)
        val granted = ContextCompat.checkSelfPermission(app, Manifest.permission.RECORD_AUDIO) == PackageManager.PERMISSION_GRANTED
        if (sound && granted) pending = pending.withAudioEnabled()
        recording = pending.start(ContextCompat.getMainExecutor(app)) { ev ->
            if (ev is VideoRecordEvent.Finalize) {
                recording = null
                // Its limit reached, or the camera closed under it (the
                // screen left), the file is still whole.
                val e = ev.error
                val ok = !ev.hasError() || (f.length() > 0 && (e == VideoRecordEvent.Finalize.ERROR_DURATION_LIMIT_REACHED ||
                    e == VideoRecordEvent.Finalize.ERROR_FILE_SIZE_LIMIT_REACHED || e == VideoRecordEvent.Finalize.ERROR_SOURCE_INACTIVE))
                if (!ok) return@start done(Result.failure(Exception(ev.cause?.message ?: "couldn't record ($e)")))
                if (maxBytes <= 0) return@start done(Result.success(f.absolutePath))
                // The muxer's room kept for the index, dropped: the file is
                // what maxBytes counts.
                photoWork.execute {
                    runCatching { compactMp4(f) }
                    done(Result.success(f.absolutePath))
                }
            }
        }
    }

    fun stopRecording() { recording?.stop() }

    fun capture(maxSize: Long, quality: Double, done: (Result<String>) -> kotlin.Unit) {
        val c = capture ?: return done(Result.failure(CameraException(CameraError.NO_VIEW)))
        c.flashMode = flashMode()
        val f = cameraFile("jpg")
        // The front camera's photo mirrored as the preview showed it
        // (CameraX says so in its EXIF; rewritten, it's flipped).
        val meta = ImageCapture.Metadata().apply { isReversedHorizontal = front && mirror }
        val size = maxSize.coerceIn(0, Int.MAX_VALUE.toLong()).toInt()
        c.takePicture(ImageCapture.OutputFileOptions.Builder(f).setMetadata(meta).build(), photoWork,
            object : ImageCapture.OnImageSavedCallback {
                override fun onImageSaved(r: ImageCapture.OutputFileResults) {
                    try {
                        if (size > 0 || quality > 0) rewriteJpeg(f, size, quality)
                        done(Result.success(f.absolutePath))
                    } catch (e: Exception) {
                        done(Result.failure(e))
                    }
                }

                override fun onError(e: ImageCaptureException) = done(Result.failure(e))
            })
    }
}

/** Where photos are saved and written again: off the main thread. */
private val photoWork by lazy { Executors.newSingleThreadExecutor() }

fun makeView(events: ViewEvents): View = CameraView(events)

private var shots = 0

/** take's screen: the system's camera app, after the camera permission if the app declares it (then the camera app needs it granted). */
private fun takePhoto(done: (Result<String>) -> kotlin.Unit) {
    val launch = {
        val act = AsukaModules.activity as? ComponentActivity
        val app = AsukaModules.context
        if (act == null) {
            done(Result.failure(Exception("no screen to show the camera on")))
        } else if (!app.packageManager.hasSystemFeature(PackageManager.FEATURE_CAMERA_ANY)) {
            done(Result.failure(CameraException(CameraError.NO_CAMERA)))
        } else {
            val f = cameraFile("jpg")
            var launcher: ActivityResultLauncher<android.net.Uri>? = null
            launcher = act.activityResultRegistry.register("asuka-camera-${shots++}", ActivityResultContracts.TakePicture()) { _ ->
                launcher?.unregister()
                // The file is the answer: some camera apps write it and
                // still report a failure.
                done(Result.success(if (f.length() > 0) f.absolutePath else ""))
            }
            try {
                launcher.launch(AsukaModules.fileUri(f))
            } catch (e: android.content.ActivityNotFoundException) {
                launcher.unregister()
                done(Result.failure(CameraException(CameraError.NO_CAMERA)))
            }
        }
    }
    if (AsukaModules.declares(Manifest.permission.CAMERA)) {
        requestPermission(Manifest.permission.CAMERA) { ok ->
            if (ok) launch() else done(Result.failure(CameraException(CameraError.NOT_ALLOWED)))
        }
    } else launch()
}

/** A callback's result, on the main thread, as a suspend fun's. */
private suspend fun <T> onMain(start: ((Result<T>) -> kotlin.Unit) -> kotlin.Unit): T = withContext(Dispatchers.Main) {
    suspendCancellableCoroutine { c -> start { r -> if (c.isActive) r.fold({ c.resume(it) }, { c.resumeWithException(it) }) } }
}

private class CameraXCamera : Camera {
    override suspend fun take(): String = onMain { takePhoto(it) }

    override suspend fun capture(maxSize: Long, quality: Double): String = onMain { done ->
        CameraView.current?.capture(maxSize, quality, done) ?: done(Result.failure(CameraException(CameraError.NO_VIEW)))
    }

    override suspend fun record(mute: Boolean, maxDuration: kotlin.time.Duration, maxBytes: Long): String = onMain { done ->
        CameraView.current?.record(mute, maxDuration.inWholeMilliseconds, maxBytes, done) ?: done(Result.failure(CameraException(CameraError.NO_VIEW)))
    }

    override fun stopRecording() {
        Handler(Looper.getMainLooper()).post { CameraView.current?.stopRecording() }
    }

    override fun focus(x: Double, y: Double) {
        CameraView.current?.focus(x.coerceIn(0.0, 1.0), y.coerceIn(0.0, 1.0))
    }

    override fun access(): String = permissionAccess(Manifest.permission.CAMERA)

    override suspend fun requestAccess(): String = withContext(Dispatchers.Main) {
        val p = Manifest.permission.CAMERA
        if (!isGranted(p)) suspendCancellableCoroutine { c -> requestPermission(p) { if (c.isActive) c.resume(kotlin.Unit) } }
        permissionAccess(p)
    }
}

fun makeCamera(): Camera = CameraXCamera()
