package dev.asuka

import android.Manifest
import android.content.ContentUris
import android.content.pm.PackageManager
import android.database.Cursor
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.provider.MediaStore
import android.util.Size
import androidx.core.content.ContextCompat
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID

/**
 * platform/photos' module, on MediaStore: "current" and "request" answer what's
 * allowed, "manage" asks again for which photos, "list" answers JSON (id,
 * width, height, created in ms, screenshot), "thumbnail" a JPEG's path and
 * "export" JSON (path, name). Off the main thread where they wait.
 */
class AsukaPhotoLibrary(private val rt: AsukaRuntime) {
    private val all = when {
        Build.VERSION.SDK_INT >= 34 -> arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU -> arrayOf(Manifest.permission.READ_MEDIA_IMAGES)
        // Before Android 13, photos are read with the storage permission.
        else -> arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)
    }

    private fun granted(p: String) = ContextCompat.checkSelfPermission(rt.app, p) == PackageManager.PERMISSION_GRANTED

    private fun access(asked: Boolean = false): String = when {
        granted(all[0]) -> "allowed"
        all.size > 1 && granted(all[1]) -> "limited"
        asked -> "denied"
        else -> "notAsked"
    }

    private val collection: Uri get() = MediaStore.Images.Media.EXTERNAL_CONTENT_URI

    private fun off(reply: (Result<ByteArray>) -> Unit, f: () -> ByteArray) {
        Thread { reply(runCatching(f)) }.start()
    }

    fun methods(): Map<String, AsukaModuleMethod> = mapOf(
        "current" to { _, reply -> reply(Result.success(access().toByteArray())) },
        "request" to { _, reply ->
            rt.withPermission(all) { reply(Result.success(access(asked = true).toByteArray())) }
        },
        // Android 14 asks again which photos only while the permission isn't fully given.
        "manage" to { _, reply ->
            val act = rt.activity
            if (act == null) reply(Result.failure(Exception("no screen")))
            else {
                var launcher: androidx.activity.result.ActivityResultLauncher<Array<String>>? = null
                launcher = act.activityResultRegistry.register("asuka-photos-manage-${UUID.randomUUID()}",
                    androidx.activity.result.contract.ActivityResultContracts.RequestMultiplePermissions()) {
                    launcher?.unregister()
                    reply(Result.success(ByteArray(0)))
                }
                launcher.launch(all)
            }
        },
        "list" to { args, reply ->
            val o = JSONObject(String(args, Charsets.UTF_8))
            off(reply) { list(o.optBoolean("screenshots"), o.optInt("offset"), o.optInt("limit", 100)).toString().toByteArray() }
        },
        "thumbnail" to { args, reply ->
            val o = JSONObject(String(args, Charsets.UTF_8))
            off(reply) { thumbnail(o.getString("id"), o.optInt("size", 300)).toByteArray() }
        },
        "export" to { args, reply ->
            val o = JSONObject(String(args, Charsets.UTF_8))
            off(reply) { export(o.getString("id")).toString().toByteArray() }
        },
    )

    private fun list(screenshots: Boolean, offset: Int, limit: Int): JSONArray {
        // Before Android 10 there are no relative paths: the file's path says where it is.
        @Suppress("DEPRECATION")
        val path = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) MediaStore.Images.Media.RELATIVE_PATH else MediaStore.Images.Media.DATA
        val projection = arrayOf(
            MediaStore.Images.Media._ID, MediaStore.Images.Media.WIDTH, MediaStore.Images.Media.HEIGHT,
            MediaStore.Images.Media.DATE_TAKEN, MediaStore.Images.Media.DATE_ADDED, path,
        )
        val where = if (screenshots) "$path LIKE ?" else null
        val whereArgs = if (screenshots) arrayOf("%Screenshot%") else null
        val sort = "${MediaStore.Images.Media.DATE_ADDED} DESC"
        val out = JSONArray()
        val cursor = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val q: Bundle = Bundle().apply {
                putString(android.content.ContentResolver.QUERY_ARG_SQL_SELECTION, where)
                putStringArray(android.content.ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, whereArgs)
                putString(android.content.ContentResolver.QUERY_ARG_SQL_SORT_ORDER, sort)
                putInt(android.content.ContentResolver.QUERY_ARG_LIMIT, limit)
                putInt(android.content.ContentResolver.QUERY_ARG_OFFSET, offset)
            }
            rt.app.contentResolver.query(collection, projection, q, null)
        } else {
            // Before Android 11 MediaStore takes no limit or offset: the
            // cursor starts at offset, and limit rows are read from it.
            rt.app.contentResolver.query(collection, projection, where, whereArgs, sort)?.also { it.moveToPosition(offset - 1) }
        }
        cursor?.use { c: Cursor ->
            while (out.length() < limit && c.moveToNext()) {
                val taken = c.getLong(3)
                out.put(JSONObject()
                    .put("id", c.getLong(0).toString()).put("width", c.getInt(1)).put("height", c.getInt(2))
                    .put("created", if (taken > 0) taken else c.getLong(4) * 1000)
                    .put("screenshot", (c.getString(5) ?: "").contains("Screenshot", ignoreCase = true)))
            }
        }
        return out
    }

    private fun uri(id: String) = ContentUris.withAppendedId(collection, id.toLongOrNull() ?: throw Exception("no such photo"))

    private fun thumbnail(id: String, size: Int): String {
        val dir = File(rt.app.cacheDir, "asuka-photo-thumbs").apply { mkdirs() }
        val f = File(dir, "$id-$size.jpg")
        if (f.isFile) return f.path
        val u = uri(id)
        // It fits in the box it's given, and the short side is the one
        // asked for: a long screenshot's box is as long as it is.
        var long = size
        var turn = 0
        rt.app.contentResolver.query(u, arrayOf(MediaStore.Images.Media.WIDTH, MediaStore.Images.Media.HEIGHT, MediaStore.Images.Media.ORIENTATION), null, null, null)?.use { c ->
            if (c.moveToFirst()) {
                if (minOf(c.getInt(0), c.getInt(1)) > 0) long = minOf(maxOf(c.getInt(0), c.getInt(1)) * size / minOf(c.getInt(0), c.getInt(1)), size * 8)
                turn = c.getInt(2)
            }
        }
        val bmp: Bitmap =
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) rt.app.contentResolver.loadThumbnail(u, Size(long, long), null)
            else decodeThumbnail(u, long, turn)
        val tmp = File(dir, UUID.randomUUID().toString())
        tmp.outputStream().use { bmp.compress(Bitmap.CompressFormat.JPEG, 80, it) }
        bmp.recycle()
        tmp.renameTo(f)
        return f.path
    }

    /**
     * Before Android 10 there's no loadThumbnail: the photo decoded at the
     * largest sample that still fills box, scaled to fit it and turned as
     * it was taken (MediaStore's orientation), as loadThumbnail gives it.
     */
    private fun decodeThumbnail(u: Uri, box: Int, turn: Int): Bitmap {
        val r = rt.app.contentResolver
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        r.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, bounds) } ?: throw Exception("no such photo")
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= box) sample *= 2
        var b = r.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample }) }
            ?: throw Exception("not an image")
        val k = box.toFloat() / maxOf(b.width, b.height)
        if (k < 1 || turn != 0) {
            val m = Matrix()
            if (k < 1) m.postScale(k, k)
            if (turn != 0) m.postRotate(turn.toFloat())
            val t = Bitmap.createBitmap(b, 0, 0, b.width, b.height, m, true)
            if (t !== b) b.recycle()
            b = t
        }
        return b
    }

    private fun export(id: String): JSONObject {
        val u = uri(id)
        var name = ""
        rt.app.contentResolver.query(u, arrayOf(MediaStore.Images.Media.DISPLAY_NAME), null, null, null)?.use { c ->
            if (c.moveToFirst()) name = c.getString(0) ?: ""
        }
        val ext = name.substringAfterLast('.', "jpg")
        val dir = File(rt.app.cacheDir, "asuka-photo-exports").apply { mkdirs() }
        val f = File(dir, "${UUID.randomUUID()}.$ext")
        val input = rt.app.contentResolver.openInputStream(u) ?: throw Exception("no such photo")
        input.use { i -> f.outputStream().use { o -> i.copyTo(o) } }
        return JSONObject().put("path", f.path).put("name", name)
    }
}
