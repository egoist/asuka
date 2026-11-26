// platform/camera on Android: the View is CameraX's PreviewView, bound to the
// activity's lifecycle with a preview, an image capture (capture), a
// video capture in video mode (record) and, with scan on, an analysis
// that reads codes with ML Kit; the camera is open while the view is in a
// window. take opens the system's camera app to a file of the app's,
// through its FileProvider.
package platform.camera

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import androidx.activity.result.contract.ActivityResultContracts
import androidx.camera.core.CameraSelector
import androidx.camera.core.ExperimentalGetImage
import androidx.camera.core.ImageAnalysis
import androidx.camera.core.ImageCapture
import androidx.camera.core.ImageCaptureException
import androidx.camera.core.ImageProxy
import androidx.camera.core.Preview
import androidx.camera.core.UseCase
import androidx.camera.lifecycle.ProcessCameraProvider
import androidx.camera.video.FileOutputOptions
import androidx.camera.video.Recorder
import androidx.camera.video.Recording
import androidx.camera.video.VideoCapture
import androidx.camera.video.VideoRecordEvent
import androidx.camera.view.PreviewView
import androidx.core.content.ContextCompat
import androidx.core.content.edit
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
import android.view.View as AndroidView

/** Where photos and videos go: the app's cache's asuka-camera. */
private fun cameraFile(ext: String): File {
    val dir = File(AsukaModules.context.cacheDir, "asuka-camera").apply { mkdirs() }
    return File(dir, UUID.randomUUID().toString() + "." + ext)
}

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

    fun isNew(kind: String, value: String, now: Long = android.os.SystemClock.uptimeMillis()): Boolean {
        val key = kind + "\u0000" + value
        val new = last[key]?.let { now - it > 1000 } ?: true
        last[key] = now
        if (last.size > 32) last.entries.removeAll { now - it.value > 1000 }
        return new
    }
}

/** Reads codes in the camera's frames, until closed. */
private class CodeAnalyzer(kinds: Set<String>, val found: (String, String) -> kotlin.Unit) : ImageAnalysis.Analyzer, AutoCloseable {
    private val client = scanner(kinds)

    @ExperimentalGetImage
    override fun analyze(p: ImageProxy) {
        val img = p.image ?: run { p.close(); return }
        // Its listeners run on the main thread; the next frame waits for
        // this one (the analysis keeps only the latest).
        client.process(InputImage.fromMediaImage(img, p.imageInfo.rotationDegrees))
            .addOnSuccessListener { list ->
                for (b in list) {
                    val k = kindOf[b.format] ?: continue
                    val v = b.rawValue ?: continue
                    found(k, v)
                }
            }
            .addOnCompleteListener { p.close() }
    }

    override fun close() = client.close()
}

internal class CameraView(private val events: ViewEvents) : View {
    companion object {
        /** The View on screen, which capture and record take from. */
        var current: CameraView? = null
    }

    private val preview = PreviewView(events.context).apply { scaleType = PreviewView.ScaleType.FILL_CENTER }
    override val view: AndroidView get() = preview
    private var capture: ImageCapture? = null
    private var provider: ProcessCameraProvider? = null
    private var front = false
    private var scan = false
    private var kinds = emptySet<String>()
    private var analyzer: CodeAnalyzer? = null
    private var analysis: ExecutorService? = null
    private val seen = CodeSeen()
    private var binding = false
    private var video = false
    private var videoCapture: VideoCapture<Recorder>? = null
    private var recording: Recording? = null

    init {
        // The camera is open while the view is in a window: leaving the
        // screen closes it.
        preview.addOnAttachStateChangeListener(object : AndroidView.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: AndroidView) {
                current = this@CameraView
                if (!binding) bind()
            }

            override fun onViewDetachedFromWindow(v: AndroidView) = unbind()
        })
    }

    override fun setFacing(value: Facing) { front = value == Facing.FRONT; changed() }
    override fun setMode(value: Mode) { video = value == Mode.VIDEO; changed() }
    override fun setScan(value: Boolean) { scan = value; changed() }
    override fun setCodes(value: List<String>) { kinds = value.toSet(); changed() }

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
        provider?.unbindAll()
        provider = null
        capture = null
        videoCapture = null
        analyzer?.close()
        analyzer = null
        analysis?.shutdown()
        analysis = null
    }

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
                    val p = Preview.Builder().build().also { it.surfaceProvider = preview.surfaceProvider }
                    val c = ImageCapture.Builder().build()
                    val selector = if (front) CameraSelector.DEFAULT_FRONT_CAMERA else CameraSelector.DEFAULT_BACK_CAMERA
                    provider.unbindAll()
                    analyzer?.close()
                    analyzer = null
                    val cases = mutableListOf<UseCase>(p, c)
                    if (scan) {
                        val a = CodeAnalyzer(kinds) { k, v -> if (seen.isNew(k, v)) events.code(Code(kind = k, value = v)) }
                        val ex = analysis ?: Executors.newSingleThreadExecutor().also { analysis = it }
                        val ia = ImageAnalysis.Builder().setBackpressureStrategy(ImageAnalysis.STRATEGY_KEEP_ONLY_LATEST).build()
                        ia.setAnalyzer(ex, a)
                        analyzer = a
                        cases += ia
                    }
                    val vc = if (video) VideoCapture.withOutput(Recorder.Builder().build()) else null
                    if (vc != null) cases += vc
                    provider.bindToLifecycle(act, selector, *cases.toTypedArray())
                    capture = c
                    videoCapture = vc
                } catch (e: Exception) {
                    events.error(e.message ?: "no camera")
                }
            }, ContextCompat.getMainExecutor(act))
        }
    }

    /** Records to a file until stopRecording (or maxMs, if not 0), with the microphone unless mute (or not allowed). */
    fun record(mute: Boolean, maxMs: Long, done: (Result<String>) -> kotlin.Unit) {
        if (capture == null) return done(Result.failure(CameraException(CameraError.NO_VIEW)))
        val vc = videoCapture ?: return done(Result.failure(Exception("the camera view isn't in video mode")))
        if (recording != null) return done(Result.failure(Exception("already recording")))
        val mic = Manifest.permission.RECORD_AUDIO
        if (mute || !AsukaModules.declares(mic)) startRecording(vc, false, maxMs, done)
        else requestPermission(mic) { ok -> startRecording(vc, ok, maxMs, done) }
    }

    // The permission is checked: withAudioEnabled is only called with it granted.
    @SuppressLint("MissingPermission")
    private fun startRecording(vc: VideoCapture<Recorder>, sound: Boolean, maxMs: Long, done: (Result<String>) -> kotlin.Unit) {
        if (videoCapture !== vc) return done(Result.failure(CameraException(CameraError.NO_VIEW)))
        val app = AsukaModules.context
        val f = cameraFile("mp4")
        val opts = FileOutputOptions.Builder(f).apply { if (maxMs > 0) setDurationLimitMillis(maxMs) }.build()
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
                    e == VideoRecordEvent.Finalize.ERROR_SOURCE_INACTIVE))
                done(if (ok) Result.success(f.absolutePath) else Result.failure(Exception(ev.cause?.message ?: "couldn't record ($e)")))
            }
        }
    }

    fun stopRecording() { recording?.stop() }

    fun capture(done: (Result<String>) -> kotlin.Unit) {
        val c = capture ?: return done(Result.failure(CameraException(CameraError.NO_VIEW)))
        val f = cameraFile("jpg")
        c.takePicture(ImageCapture.OutputFileOptions.Builder(f).build(), ContextCompat.getMainExecutor(AsukaModules.context),
            object : ImageCapture.OnImageSavedCallback {
                override fun onImageSaved(r: ImageCapture.OutputFileResults) = done(Result.success(f.absolutePath))
                override fun onError(e: ImageCaptureException) = done(Result.failure(e))
            })
    }
}

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

    override suspend fun capture(): String = onMain { done ->
        CameraView.current?.capture(done) ?: done(Result.failure(CameraException(CameraError.NO_VIEW)))
    }

    override suspend fun record(mute: Boolean, maxDuration: kotlin.time.Duration): String = onMain { done ->
        CameraView.current?.record(mute, maxDuration.inWholeMilliseconds, done) ?: done(Result.failure(CameraException(CameraError.NO_VIEW)))
    }

    override fun stopRecording() {
        android.os.Handler(android.os.Looper.getMainLooper()).post { CameraView.current?.stopRecording() }
    }

    override fun access(): String = permissionAccess(Manifest.permission.CAMERA)

    override suspend fun requestAccess(): String = withContext(Dispatchers.Main) {
        val p = Manifest.permission.CAMERA
        if (!isGranted(p)) suspendCancellableCoroutine { c -> requestPermission(p) { if (c.isActive) c.resume(kotlin.Unit) } }
        permissionAccess(p)
    }
}

fun makeCamera(): Camera = CameraXCamera()
