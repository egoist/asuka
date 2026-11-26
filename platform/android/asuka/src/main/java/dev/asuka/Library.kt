package dev.asuka

import android.Manifest
import android.content.ContentValues
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.provider.MediaStore
import androidx.annotation.RequiresApi
import org.json.JSONObject
import java.io.File
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

private val copier = Executors.newSingleThreadExecutor()

/**
 * platform/library's module: "save" (args: JSON path, collection 0 downloads,
 * 1 movies, 2 music, 3 pictures, name, folder, mime, move) copies a file
 * into a MediaStore collection, off the main thread, and answers with its
 * content:// URI.
 */
fun AsukaRuntime.libraryMethods(): Map<String, AsukaModuleMethod> = mapOf(
    "save" to { args, reply ->
        val o = JSONObject(String(args, Charsets.UTF_8))
        val src = File(o.getString("path"))
        val collection = o.optInt("collection")
        val name = o.optString("name").ifEmpty { src.name }
        val folder = o.optString("folder")
        val mime = o.optString("mime").ifEmpty { "application/octet-stream" }
        val move = o.optBoolean("move")
        val dir = when (collection) {
            1 -> Environment.DIRECTORY_MOVIES
            2 -> Environment.DIRECTORY_MUSIC
            3 -> Environment.DIRECTORY_PICTURES
            else -> Environment.DIRECTORY_DOWNLOADS
        }
        fun done(r: Result<String>) = onMain { reply(r.map { it.toByteArray() }) }
        if (!src.isFile) reply(Result.failure(Exception("no file at ${src.path}")))
        else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) copier.execute {
            done(runCatching { saveToMediaStore(src, collection, dir, folder, name, mime) }.onSuccess { if (move) src.delete() })
        }
        // Before Android 10, writing to the shared folders takes the storage permission.
        else withPermission(arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE)) { ok ->
            if (!ok) reply(Result.failure(Exception("not allowed: storage")))
            else copier.execute {
                done(runCatching { saveToSharedFolder(src, dir, folder, name, mime) }.onSuccess { if (move) src.delete() })
            }
        }
    },
)

/** Copies src into a MediaStore collection, pending until it's whole; its URI. Android 10 and later. */
@RequiresApi(Build.VERSION_CODES.Q)
private fun AsukaRuntime.saveToMediaStore(src: File, collection: Int, dir: String, folder: String, name: String, mime: String): String {
    val volume = MediaStore.VOLUME_EXTERNAL_PRIMARY
    val uri = when (collection) {
        1 -> MediaStore.Video.Media.getContentUri(volume)
        2 -> MediaStore.Audio.Media.getContentUri(volume)
        3 -> MediaStore.Images.Media.getContentUri(volume)
        else -> MediaStore.Downloads.getContentUri(volume)
    }
    val values = ContentValues().apply {
        put(MediaStore.MediaColumns.DISPLAY_NAME, name)
        put(MediaStore.MediaColumns.MIME_TYPE, mime)
        put(MediaStore.MediaColumns.RELATIVE_PATH, if (folder.isEmpty()) dir else "$dir/$folder")
        put(MediaStore.MediaColumns.IS_PENDING, 1)
    }
    val resolver = app.contentResolver
    val item = resolver.insert(uri, values) ?: error("couldn't add to the library")
    try {
        resolver.openOutputStream(item)?.use { out -> src.inputStream().use { it.copyTo(out, 1 shl 16) } } ?: error("couldn't write to the library")
        resolver.update(item, ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }, null, null)
    } catch (e: Exception) {
        resolver.delete(item, null, null)
        throw e
    }
    return item.toString()
}

/**
 * Before Android 10 (no relative paths, no pending items): copies src into
 * the shared folder, a taken name numbered as MediaStore numbers it, and
 * has the media scanner add it; its URI (the file's, if the scanner gives
 * none).
 */
private fun AsukaRuntime.saveToSharedFolder(src: File, dir: String, folder: String, name: String, mime: String): String {
    @Suppress("DEPRECATION")
    val top = Environment.getExternalStoragePublicDirectory(dir)
    val parent = if (folder.isEmpty()) top else File(top, folder)
    if (!parent.isDirectory && !parent.mkdirs()) error("couldn't add to the library")
    val base = name.substringBeforeLast('.')
    val ext = name.substringAfterLast('.', "")
    var f = File(parent, name)
    var n = 1
    while (f.exists()) f = File(parent, if (ext.isEmpty()) "$base (${n++})" else "$base (${n++}).$ext")
    try {
        f.outputStream().use { out -> src.inputStream().use { it.copyTo(out, 1 shl 16) } }
    } catch (e: Exception) {
        f.delete()
        throw e
    }
    val scanned = CountDownLatch(1)
    var uri: Uri? = null
    MediaScannerConnection.scanFile(app, arrayOf(f.path), arrayOf(mime)) { _, u -> uri = u; scanned.countDown() }
    scanned.await(10, TimeUnit.SECONDS)
    return (uri ?: Uri.fromFile(f)).toString()
}
