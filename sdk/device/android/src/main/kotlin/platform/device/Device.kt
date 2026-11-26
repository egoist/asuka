// platform/device on Android: Build and the package's info, Custom Tabs, the
// Photo Picker and the storage picker (no permission), ClipboardManager
// (images through the app's FileProvider, the host's), the window's
// FLAG_KEEP_SCREEN_ON.
package platform.device

import android.content.ClipData
import android.content.ClipDescription
import android.content.ClipboardManager
import android.content.Context
import android.net.Uri
import android.os.Build
import android.provider.OpenableColumns
import android.view.WindowManager
import android.view.textclassifier.TextClassifier
import android.webkit.MimeTypeMap
import androidx.activity.result.PickVisualMediaRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.browser.customtabs.CustomTabsIntent
import androidx.core.content.FileProvider
import dev.asuka.AsukaModules
import dev.asuka.AsukaUnsupportedException
import java.io.File as JFile
import java.util.UUID
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.withContext

class AndroidDevice(private val app: Context) : Platform {
    private val cm get() = app.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager

    override suspend fun info(): NativeInfo {
        val p = app.packageManager.getPackageInfo(app.packageName, 0)
        return NativeInfo(model = Build.MODEL, system = "Android", systemVersion = Build.VERSION.RELEASE,
            appVersion = p.versionName ?: "", build = p.longVersionCode.toString(),
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

    override suspend fun pickPhotos(max: Long): List<String> {
        val request = PickVisualMediaRequest(ActivityResultContracts.PickVisualMedia.ImageOnly)
        val uris = if (max > 1) AsukaModules.launch(ActivityResultContracts.PickMultipleVisualMedia(max.toInt()), request)
            else listOfNotNull(AsukaModules.launch(ActivityResultContracts.PickVisualMedia(), request))
        return withContext(Dispatchers.IO) {
            val dir = JFile(app.cacheDir, "asuka-photos").apply { mkdirs() }
            uris.mapNotNull { u ->
                runCatching {
                    val ext = MimeTypeMap.getSingleton().getExtensionFromMimeType(app.contentResolver.getType(u)) ?: "jpg"
                    val f = JFile(dir, UUID.randomUUID().toString() + "." + ext)
                    app.contentResolver.openInputStream(u)!!.use { i -> f.outputStream().use { o -> i.copyTo(o) } }
                    f.absolutePath
                }.getOrNull()
            }
        }
    }

    override suspend fun pickFiles(types: List<String>, multiple: Boolean): List<File> {
        val mimes = types.map { it.trim() }.filter { it.isNotEmpty() }.ifEmpty { listOf("*/*") }.toTypedArray()
        val uris = if (multiple) AsukaModules.launch(ActivityResultContracts.OpenMultipleDocuments(), mimes)
            else listOfNotNull(AsukaModules.launch(ActivityResultContracts.OpenDocument(), mimes))
        return withContext(Dispatchers.IO) { uris.mapNotNull { copyPicked(it) } }
    }

    /** Copies a picked document to the caches, in a folder of its own (its name stays its own). */
    private fun copyPicked(u: Uri): File? = runCatching {
        val cr = app.contentResolver
        var name = "file"
        cr.query(u, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst() && !c.isNull(0)) name = c.getString(0)
        }
        name = name.replace('/', '_')
        val dir = JFile(app.cacheDir, "asuka-files/" + UUID.randomUUID()).apply { mkdirs() }
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
        val url = d.hasMimeType(ClipDescription.MIMETYPE_TEXT_URILIST) ||
            d.classificationStatus == ClipDescription.CLASSIFICATION_COMPLETE && d.getConfidenceScore(TextClassifier.TYPE_URL) >= 0.5f
        return ClipboardKinds(text = text, image = image, url = url)
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
        val authority = app.packageName + ".asuka.files"
        val uri = withContext(Dispatchers.IO) {
            // A file outside the app's folders goes to its caches first.
            runCatching { FileProvider.getUriForFile(app, authority, f) }.getOrNull()
                ?: JFile(JFile(app.cacheDir, "asuka-clipboard/" + UUID.randomUUID()).apply { mkdirs() }, f.name).let { c ->
                    f.copyTo(c, overwrite = true)
                    FileProvider.getUriForFile(app, authority, c)
                }
        }
        withContext(Dispatchers.Main) { cm.setPrimaryClip(ClipData.newUri(app.contentResolver, "image", uri)) }
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
}

fun makePlatform(): Platform = AndroidDevice(AsukaModules.context)
