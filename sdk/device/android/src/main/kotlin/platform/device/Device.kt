// platform/device on Android: Build and the package's info, Custom Tabs, the
// Photo Picker (images, videos or both; numbered where it numbers them)
// and the storage picker (no permission), ClipboardManager (images through
// the app's FileProvider, the host's; rich text as an HTML clip; files as
// content URIs, copied in as they're read; an app's own formats as files of
// its caches, the clip's description naming their types), the
// window's FLAG_KEEP_SCREEN_ON.
package platform.device

import android.annotation.SuppressLint
import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.MediaMetadataRetriever
import android.net.Uri
import android.os.Build
import android.os.PersistableBundle
import android.os.ext.SdkExtensions
import android.provider.MediaStore
import android.provider.OpenableColumns
import android.view.WindowManager
import android.view.textclassifier.TextClassifier
import android.webkit.MimeTypeMap
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.browser.customtabs.CustomTabsIntent
import androidx.core.content.FileProvider
import androidx.core.content.pm.PackageInfoCompat
import androidx.exifinterface.media.ExifInterface
import dev.asuka.AsukaModules
import dev.asuka.AsukaUnsupportedException
import java.io.File as JFile
import java.util.UUID
import kotlin.math.roundToInt
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.withContext

class AndroidDevice(private val app: Context) : Platform {
    private val cm get() = app.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager

    override suspend fun info(): NativeInfo {
        val p = app.packageManager.getPackageInfo(app.packageName, 0)
        return NativeInfo(model = Build.MODEL, system = "Android", systemVersion = Build.VERSION.RELEASE,
            appVersion = p.versionName ?: "", build = PackageInfoCompat.getLongVersionCode(p).toString(),
            source = installSource(), installedMs = p.firstInstallTime, updatedMs = p.lastUpdateTime)
    }

    /**
     * Where the app came from: the package that installed it (Play's,
     * another store's), else a sideloaded copy, or a developer's
     * (debuggable: asuka dev's, Android Studio's).
     */
    private fun installSource(): InstallSource {
        if (app.applicationInfo.flags and android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE != 0) return InstallSource.DEVELOPMENT
        val by = runCatching {
            if (Build.VERSION.SDK_INT >= 30) app.packageManager.getInstallSourceInfo(app.packageName).installingPackageName
            else @Suppress("DEPRECATION") app.packageManager.getInstallerPackageName(app.packageName)
        }.getOrNull()
        return when (by) {
            null, "", "com.android.shell", "com.google.android.packageinstaller", "com.android.packageinstaller" -> InstallSource.DIRECT
            "com.android.vending" -> InstallSource.PLAY_STORE
            else -> InstallSource.OTHER_STORE
        }
    }

    override suspend fun browse(url: String) = withContext(Dispatchers.Main) {
        val u = Uri.parse(url)
        if (u.scheme != "https" && u.scheme != "http") throw IllegalArgumentException("device: not a web URL: $url")
        val act = AsukaModules.activity ?: throw AsukaUnsupportedException("device: no activity shows the app")
        CustomTabsIntent.Builder().build().launchUrl(act, u)
    }

    override suspend fun pickMedia(kind: MediaKind, max: Long, ordered: Boolean, maxSize: Long, quality: Double): List<NativeMedia> {
        val type = when (kind) {
            MediaKind.VIDEOS -> ActivityResultContracts.PickVisualMedia.VideoOnly
            MediaKind.ALL -> ActivityResultContracts.PickVisualMedia.ImageAndVideo
            else -> ActivityResultContracts.PickVisualMedia.ImageOnly
        }
        // Numbered as they're picked, in that order (the photo picker's;
        // a fallback picker ignores it).
        val b = PickVisualMediaRequest.Builder().setMediaType(type).setOrderedSelection(ordered)
        // 0: as many as the picker takes (the photo picker's limit).
        val limit = pickLimit()
        val n = if (max <= 0) limit else minOf(max, limit.toLong()).toInt()
        val uris = if (n <= 1) listOfNotNull(AsukaModules.launch(ActivityResultContracts.PickVisualMedia(), b.build()))
            else AsukaModules.launch(ActivityResultContracts.PickMultipleVisualMedia(n), b.setMaxItems(n).build())
        val size = maxSize.coerceIn(0, Int.MAX_VALUE.toLong()).toInt()
        return withContext(Dispatchers.IO) { uris.mapNotNull { copyMedia(it, size, quality) } }
    }

    /**
     * The most the photo picker lets the user pick (MediaStore's, where
     * Android's own picker is: Android 13, or 11 and 12 with its update);
     * a fallback picker (a gallery app's) has none.
     */
    private fun pickLimit(): Int =
        if (ActivityResultContracts.PickVisualMedia.isPhotoPickerAvailable(app) && hasPickLimit()) pickImagesMaxLimit() else Int.MAX_VALUE

    private fun hasPickLimit() = Build.VERSION.SDK_INT >= 33 ||
        (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && SdkExtensions.getExtensionVersion(Build.VERSION_CODES.R) >= 2)

    @SuppressLint("NewApi") // Called only where hasPickLimit: Android 13, or R's extension 2.
    private fun pickImagesMaxLimit(): Int = MediaStore.getPickImagesMaxLimit()

    /**
     * A picked photo or video copied to the caches' asuka-photos: a video,
     * or an image unless maxSize or quality is asked, as it is; an image
     * otherwise decoded, turned upright (its EXIF), scaled down to maxSize
     * on its long side and written as a JPEG of quality (0: 0.9). Its
     * size in pixels is upright (a video's after its rotation).
     */
    private fun copyMedia(u: Uri, maxSize: Int, quality: Double): NativeMedia? = runCatching {
        val cr = app.contentResolver
        var mime = cr.getType(u) ?: ""
        var name = ""
        cr.query(u, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) name = c.getString(0)
        }
        if (mime.isEmpty()) mime = MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substringAfterLast('.', "").lowercase()) ?: ""
        val video = mime.startsWith("video/")
        val dir = JFile(app.cacheDir, "asuka-photos").apply { mkdirs() }
        var w = 0
        var h = 0
        var ms = 0L
        val f: JFile
        if (!video && (maxSize > 0 || quality > 0)) {
            f = JFile(dir, UUID.randomUUID().toString() + ".jpg")
            val b = uprightBitmap(u, maxSize)
            val q = if (quality > 0) (quality * 100).roundToInt().coerceIn(1, 100) else 90
            f.outputStream().use { b.compress(Bitmap.CompressFormat.JPEG, q, it) }
            w = b.width
            h = b.height
            b.recycle()
            mime = "image/jpeg"
        } else {
            val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(mime)
                ?: name.substringAfterLast('.', "").ifEmpty { if (video) "mp4" else "jpg" }
            f = JFile(dir, UUID.randomUUID().toString() + "." + ext)
            cr.openInputStream(u)!!.use { i -> f.outputStream().use { o -> i.copyTo(o) } }
            if (video) {
                val r = MediaMetadataRetriever()
                try {
                    r.setDataSource(f.path)
                    w = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_WIDTH)?.toIntOrNull() ?: 0
                    h = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_HEIGHT)?.toIntOrNull() ?: 0
                    val turn = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_VIDEO_ROTATION)?.toIntOrNull() ?: 0
                    if (turn == 90 || turn == 270) w = h.also { h = w }
                    ms = r.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toLongOrNull() ?: 0
                } catch (_: Exception) {
                } finally {
                    r.release()
                }
            } else {
                val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(f.path, o)
                w = maxOf(o.outWidth, 0)
                h = maxOf(o.outHeight, 0)
                if (runCatching { ExifInterface(f.path).rotationDegrees }.getOrDefault(0) % 180 != 0) w = h.also { h = w }
            }
        }
        NativeMedia(path = f.absolutePath, name = name, type = mime, video = video, width = w.toLong(), height = h.toLong(),
            durationMs = ms, size = f.length())
    }.getOrNull()

    /** An image decoded at the smallest sample still at least maxSize (0: whole), turned upright as its EXIF says and scaled to at most maxSize on its long side. */
    private fun uprightBitmap(u: Uri, maxSize: Int): Bitmap {
        val cr = app.contentResolver
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        cr.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, bounds) }
        val long = maxOf(bounds.outWidth, bounds.outHeight)
        var sample = 1
        if (maxSize > 0) while (long / (sample * 2) >= maxSize) sample *= 2
        var b = cr.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample }) }
            ?: throw IllegalStateException("device: not an image")
        val orientation = cr.openInputStream(u)?.use {
            runCatching { ExifInterface(it).getAttributeInt(ExifInterface.TAG_ORIENTATION, ExifInterface.ORIENTATION_NORMAL) }.getOrNull()
        } ?: ExifInterface.ORIENTATION_NORMAL
        val m = Matrix()
        val big = maxOf(b.width, b.height)
        if (maxSize in 1 until big) m.postScale(maxSize.toFloat() / big, maxSize.toFloat() / big)
        when (orientation) {
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
        return b
    }

    override suspend fun pickFiles(types: List<String>, multiple: Boolean): List<File> {
        val mimes = types.map { it.trim() }.filter { it.isNotEmpty() }.ifEmpty { listOf("*/*") }.toTypedArray()
        val uris = if (multiple) AsukaModules.launch(ActivityResultContracts.OpenMultipleDocuments(), mimes)
            else listOfNotNull(AsukaModules.launch(ActivityResultContracts.OpenDocument(), mimes))
        return withContext(Dispatchers.IO) { uris.mapNotNull { copyPicked(it) } }
    }

    /** Copies a picked document (or a file on the clipboard) to the caches' folder, in a folder of its own (its name stays its own). */
    private fun copyPicked(u: Uri, folder: String = "asuka-files"): File? = runCatching {
        val cr = app.contentResolver
        var name = "file"
        cr.query(u, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) name = c.getString(0)
        }
        name = name.replace('/', '_')
        val dir = JFile(app.cacheDir, folder + "/" + UUID.randomUUID()).apply { mkdirs() }
        val f = JFile(dir, name)
        cr.openInputStream(u)!!.use { i -> f.outputStream().use { o -> i.copyTo(o) } }
        File(path = f.absolutePath, name = name, type = cr.getType(u) ?: "", size = f.length())
    }.getOrNull()

    override suspend fun clipboardText(): String? = withContext(Dispatchers.Main) {
        cm.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.coerceToText(app)?.toString()
    }

    override suspend fun clipboardHas(): ClipboardKinds = withContext(Dispatchers.Main) { kinds() }

    private fun kinds(): ClipboardKinds {
        val d = cm.primaryClipDescription ?: return ClipboardKinds()
        val text = d.hasMimeType(ClipDescription.MIMETYPE_TEXT_PLAIN) || d.hasMimeType(ClipDescription.MIMETYPE_TEXT_HTML)
        val image = d.hasMimeType("image/*")
        // A link: a URI list, or text the system classified as a URL.
        // Before Android 12 the system doesn't classify it: a URI list only.
        val url = d.hasMimeType(ClipDescription.MIMETYPE_TEXT_URILIST) || Build.VERSION.SDK_INT >= 31 &&
            d.classificationStatus == ClipDescription.CLASSIFICATION_COMPLETE && d.getConfidenceScore(TextClassifier.TYPE_URL) >= 0.5f
        return ClipboardKinds(text = text, image = image, url = url, html = d.hasMimeType(ClipDescription.MIMETYPE_TEXT_HTML), files = hasFiles(d))
    }

    /**
     * Whether a clip is files: copyFiles' (it says so), or content of a
     * type other than text's, a link's or an intent's (an image, a PDF),
     * which only a content URI holds; not an app's own data (copyFormats').
     */
    private fun hasFiles(d: ClipDescription): Boolean {
        val extras = d.extras
        if (extras?.getBoolean(FILES) == true) return true
        if (extras?.containsKey(DATA) == true) return false
        return (0 until d.mimeTypeCount).any { d.getMimeType(it).substringBefore(';').trim().lowercase() !in plainTypes }
    }

    override suspend fun clipboardFiles(): List<String> {
        val clip = withContext(Dispatchers.Main) { cm.primaryClip } ?: return emptyList()
        if (!hasFiles(clip.description)) return emptyList()
        val uris = (0 until clip.itemCount).mapNotNull { clip.getItemAt(it).uri }.filter { it.scheme == "content" || it.scheme == "file" }
        return withContext(Dispatchers.IO) { uris.mapNotNull { copyPicked(it, "asuka-clipboard")?.path } }
    }

    /** Each file as an item's content URI, through the app's FileProvider (what the app that pastes is let read). */
    override suspend fun copyFiles(paths: List<String>) {
        val uris = withContext(Dispatchers.IO) {
            paths.map { p ->
                val f = JFile(p)
                if (!f.isFile) throw java.io.FileNotFoundException("device: no file at $p")
                shareable(f)
            }
        }
        withContext(Dispatchers.Main) {
            val cr = app.contentResolver
            val d = ClipDescription("files", uris.map { cr.getType(it) ?: "application/octet-stream" }.distinct().toTypedArray())
            d.extras = PersistableBundle().apply { putBoolean(FILES, true) }
            val clip = ClipData(d, ClipData.Item(uris[0]))
            for (u in uris.drop(1)) clip.addItem(ClipData.Item(u))
            cm.setPrimaryClip(clip)
        }
    }

    override suspend fun clipboardFormats(): List<String> = withContext(Dispatchers.Main) {
        val d = cm.primaryClipDescription ?: return@withContext emptyList()
        (0 until d.mimeTypeCount).map { d.getMimeType(it) }.distinct()
    }

    override suspend fun clipboardData(type: String): ByteArray? {
        val clip = withContext(Dispatchers.Main) { cm.primaryClip } ?: return null
        val d = clip.description
        if (clip.itemCount == 0 || (0 until d.mimeTypeCount).none { d.getMimeType(it).equals(type, ignoreCase = true) }) return null
        val first = clip.getItemAt(0)
        when (type) {
            ClipDescription.MIMETYPE_TEXT_PLAIN ->
                return (first.text ?: withContext(Dispatchers.Main) { first.coerceToText(app) })?.toString()?.toByteArray()
            ClipDescription.MIMETYPE_TEXT_HTML -> return first.htmlText?.toByteArray()
            ClipDescription.MIMETYPE_TEXT_URILIST ->
                return (0 until clip.itemCount).mapNotNull { clip.getItemAt(it).uri?.toString() }.joinToString("\r\n").toByteArray()
        }
        // The item copyFormats put it in (the extras say which), else the
        // first whose content is of the type, else the first with content.
        val cr = app.contentResolver
        val items = (0 until clip.itemCount).map { clip.getItemAt(it) }
        val at = d.extras?.getStringArray(DATA)?.indexOfFirst { it.equals(type, ignoreCase = true) } ?: -1
        return withContext(Dispatchers.IO) {
            val uri = items.getOrNull(at)?.uri
                ?: items.mapNotNull { it.uri }.let { us -> us.firstOrNull { cr.getType(it) == type } ?: us.firstOrNull() }
                ?: return@withContext null
            cr.openInputStream(uri)?.use { it.readBytes() }
        }
    }

    /**
     * Text and HTML go as the clip's text and HTML; every other format as
     * a file of the app's caches, an item's content URI through the app's
     * FileProvider (what the app that pastes is let read), the
     * description naming the types and its extras which item has which.
     */
    override suspend fun copyFormats(formats: List<ClipboardFormat>) {
        val text = formats.firstOrNull { it.type == ClipDescription.MIMETYPE_TEXT_PLAIN }?.data?.toString(Charsets.UTF_8)
        val html = formats.firstOrNull { it.type == ClipDescription.MIMETYPE_TEXT_HTML }?.data?.toString(Charsets.UTF_8)
        val data = formats.filter { it.type != ClipDescription.MIMETYPE_TEXT_PLAIN && it.type != ClipDescription.MIMETYPE_TEXT_HTML }
        val uris = withContext(Dispatchers.IO) {
            data.map { f ->
                val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(f.type) ?: "bin"
                val file = JFile(JFile(app.cacheDir, "asuka-clipboard/" + UUID.randomUUID()).apply { mkdirs() }, "data.$ext")
                file.writeBytes(f.data)
                FileProvider.getUriForFile(app, app.packageName + ".asuka.files", file)
            }
        }
        withContext(Dispatchers.Main) {
            val clip = if (data.isEmpty()) {
                if (html != null) ClipData.newHtmlText("text", text ?: "", html) else ClipData.newPlainText("text", text ?: "")
            } else {
                val d = ClipDescription("data", formats.map { it.type }.toTypedArray())
                d.extras = PersistableBundle().apply { putStringArray(DATA, data.map { it.type }.toTypedArray()) }
                ClipData(d, ClipData.Item(text, html, null, uris[0])).apply { for (u in uris.drop(1)) addItem(ClipData.Item(u)) }
            }
            cm.setPrimaryClip(clip)
        }
    }

    override suspend fun clipboardHTML(): String? = withContext(Dispatchers.Main) {
        cm.primaryClip?.takeIf { it.itemCount > 0 }?.getItemAt(0)?.htmlText
    }

    override suspend fun copyHTML(html: String, text: String) = withContext(Dispatchers.Main) {
        cm.setPrimaryClip(ClipData.newHtmlText("text", text, html))
    }

    override suspend fun clipboardImage(): String? {
        val clip = withContext(Dispatchers.Main) { cm.primaryClip } ?: return null
        if (!clip.description.hasMimeType("image/*")) return null
        val item = (0 until clip.itemCount).map { clip.getItemAt(it) }.firstOrNull { it.uri != null } ?: return null
        return withContext(Dispatchers.IO) {
            val cr = app.contentResolver
            val type = cr.getType(item.uri) ?: clip.description.getMimeType(0)
            val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(type) ?: "png"
            val f = JFile(JFile(app.cacheDir, "asuka-clipboard").apply { mkdirs() }, UUID.randomUUID().toString() + "." + ext)
            cr.openInputStream(item.uri)!!.use { i -> f.outputStream().use { o -> i.copyTo(o) } }
            f.absolutePath
        }
    }

    override suspend fun copyImage(path: String) {
        val f = JFile(path)
        if (!f.isFile) throw java.io.FileNotFoundException("device: no file at $path")
        val uri = withContext(Dispatchers.IO) { shareable(f) }
        withContext(Dispatchers.Main) { cm.setPrimaryClip(ClipData.newUri(app.contentResolver, "image", uri)) }
    }

    /** A file's content URI, through the app's FileProvider: a file outside the app's folders is copied to its caches first. */
    private fun shareable(f: JFile): Uri {
        val authority = app.packageName + ".asuka.files"
        return runCatching { FileProvider.getUriForFile(app, authority, f) }.getOrNull()
            ?: JFile(JFile(app.cacheDir, "asuka-clipboard/" + UUID.randomUUID()).apply { mkdirs() }, f.name).let { c ->
                f.copyTo(c, overwrite = true)
                FileProvider.getUriForFile(app, authority, c)
            }
    }

    override suspend fun watchClipboard(changed: (ClipboardKinds) -> kotlin.Unit) = withContext(Dispatchers.Main) {
        val l = ClipboardManager.OnPrimaryClipChangedListener { changed(kinds()) }
        cm.addPrimaryClipChangedListener(l)
        try {
            awaitCancellation()
        } finally {
            cm.removePrimaryClipChangedListener(l)
        }
    }

    override fun keepAwake(on: Boolean) {
        val w = AsukaModules.activity?.window ?: return
        if (on) w.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else w.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
    }

    /** The app's App info page: its permissions, notifications, storage. */
    override fun openSettings(): Boolean {
        val i = android.content.Intent(android.provider.Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
            android.net.Uri.fromParts("package", app.packageName, null))
        val act = AsukaModules.activity
        if (act == null) i.addFlags(android.content.Intent.FLAG_ACTIVITY_NEW_TASK)
        return try {
            (act ?: app).startActivity(i)
            true
        } catch (_: android.content.ActivityNotFoundException) {
            false
        }
    }
}

/** What a clip's description's extras say: copyFiles' files, copyFormats' items' types. */
private const val FILES = "dev.asuka.files"
private const val DATA = "dev.asuka.data"

/** The types that aren't a file's content: text, HTML, a link, an intent. */
private val plainTypes = setOf(ClipDescription.MIMETYPE_TEXT_PLAIN, ClipDescription.MIMETYPE_TEXT_HTML, ClipDescription.MIMETYPE_TEXT_URILIST,
    ClipDescription.MIMETYPE_TEXT_INTENT)

fun makePlatform(): Platform = AndroidDevice(AsukaModules.context)
