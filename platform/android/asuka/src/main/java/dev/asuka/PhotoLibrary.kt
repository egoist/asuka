package dev.asuka

import android.Manifest
import android.app.Activity
import android.app.RecoverableSecurityException
import android.content.ContentResolver
import android.content.ContentUris
import android.content.ContentValues
import android.content.IntentSender
import android.content.pm.PackageManager
import android.database.ContentObserver
import android.database.Cursor
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Matrix
import android.media.MediaMetadataRetriever
import android.media.MediaScannerConnection
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Environment
import android.os.Handler
import android.os.Looper
import android.provider.MediaStore
import android.util.Size
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.util.UUID
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/**
 * platform/photos' module, on MediaStore's files (its images and videos,
 * an asset's id the row's): "current" and "request" answer what's allowed,
 * "manage" asks again for which photos and videos, "list" answers JSON
 * (id, width, height, created in ms, screenshot, video, duration in ms,
 * favorite) and "count" a number, "thumbnail" a JPEG's path and "export"
 * JSON (path, name). An album is a folder (its path from the storage's
 * root, "Pictures/Trip"): "albums" lists those that hold something,
 * "createAlbum" names one under Pictures, "addToAlbum" copies into it.
 * "delete" asks the system (Android 11: to the trash), "nextChange" waits
 * for what a ContentObserver sees. Off the main thread where they wait.
 */
class AsukaPhotoLibrary(private val rt: AsukaRuntime) {
    private val all = when {
        Build.VERSION.SDK_INT >= 34 -> arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VIDEO, Manifest.permission.READ_MEDIA_VISUAL_USER_SELECTED)
        Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU -> arrayOf(Manifest.permission.READ_MEDIA_IMAGES, Manifest.permission.READ_MEDIA_VIDEO)
        // Before Android 13, photos and videos are read with the storage permission.
        else -> arrayOf(Manifest.permission.READ_EXTERNAL_STORAGE)
    }

    private fun granted(p: String) = ContextCompat.checkSelfPermission(rt.app, p) == PackageManager.PERMISSION_GRANTED

    /** Allowed: all the photos or all the videos (Android 13 grants both together); limited: only those the user chose (Android 14). */
    private fun access(asked: Boolean = false): String = when {
        granted(all[0]) || (all.size > 1 && granted(all[1])) -> "allowed"
        all.size > 2 && granted(all[2]) -> "limited"
        asked -> "denied"
        else -> "notAsked"
    }

    private val cr: ContentResolver get() = rt.app.contentResolver

    /** Images and videos, together: MediaStore's files. */
    private val files: Uri get() = MediaStore.Files.getContentUri("external")

    /** Where an item's folder is said: its relative path (Android 10), its file's path before. */
    @Suppress("DEPRECATION")
    private val pathColumn = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) MediaStore.MediaColumns.RELATIVE_PATH else MediaStore.MediaColumns.DATA

    /** Albums made in this process that hold nothing yet (an album is a folder, there once something is in it). */
    private val made = HashSet<String>()

    private fun off(reply: (Result<ByteArray>) -> Unit, f: () -> ByteArray) {
        Thread { reply(runCatching(f)) }.start()
    }

    private fun json(args: ByteArray) = if (args.isEmpty()) JSONObject() else JSONObject(String(args, Charsets.UTF_8))

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
                    ActivityResultContracts.RequestMultiplePermissions()) {
                    launcher?.unregister()
                    reply(Result.success(ByteArray(0)))
                }
                launcher.launch(all)
            }
        },
        "list" to { args, reply ->
            val o = json(args)
            off(reply) { list(Query(o), o.optInt("offset"), o.optInt("limit", 100)).toString().toByteArray() }
        },
        "count" to { args, reply ->
            val o = json(args)
            off(reply) { count(Query(o)).toString().toByteArray() }
        },
        "thumbnail" to { args, reply ->
            val o = json(args)
            off(reply) { thumbnail(o.getString("id"), o.optInt("size", 300)).toByteArray() }
        },
        "export" to { args, reply ->
            val o = json(args)
            off(reply) { export(o.getString("id")).toString().toByteArray() }
        },
        "albums" to { _, reply -> off(reply) { albums().toString().toByteArray() } },
        "createAlbum" to { args, reply ->
            val o = json(args)
            off(reply) { createAlbum(o.optString("title")).toString().toByteArray() }
        },
        "addToAlbum" to { args, reply ->
            val o = json(args)
            val ids = o.optJSONArray("ids") ?: JSONArray()
            val album = o.optString("album")
            // Before Android 10 the album's folder is written with the storage permission.
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q) {
                rt.withPermission(arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE)) { ok ->
                    if (!ok) reply(Result.failure(Exception("photos: not allowed")))
                    else off(reply) { addToAlbum(album, ids); ByteArray(0) }
                }
            } else off(reply) { addToAlbum(album, ids); ByteArray(0) }
        },
        "delete" to { args, reply ->
            val ids = json(args).optJSONArray("ids") ?: JSONArray()
            delete(List(ids.length()) { ids.getString(it) }, reply)
        },
        "nextChange" to { _, reply -> nextChange(reply) },
        "stop" to { args, reply ->
            if (String(args, Charsets.UTF_8) == "nextChange") stopChanges()
            reply(Result.success(ByteArray(0)))
        },
    )

    // -----------------------------------------------------------------
    // Listing

    /** What a list or a count keeps: a kind (0 photos, 1 videos, 2 both), an album's folder, screenshots only. */
    private class Query(o: JSONObject) {
        val kind = o.optInt("kind", 0)
        val album = o.optString("album", "")
        val screenshots = o.optBoolean("screenshots")
    }

    private fun mediaTypes(kind: Int) = when (kind) {
        1 -> "${MediaStore.Files.FileColumns.MEDIA_TYPE} = ${MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO}"
        2 -> "${MediaStore.Files.FileColumns.MEDIA_TYPE} IN (${MediaStore.Files.FileColumns.MEDIA_TYPE_IMAGE}, ${MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO})"
        else -> "${MediaStore.Files.FileColumns.MEDIA_TYPE} = ${MediaStore.Files.FileColumns.MEDIA_TYPE_IMAGE}"
    }

    private fun like(s: String) = s.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_")

    /** The storage's root, which an album's folder is said from before Android 10. */
    @Suppress("DEPRECATION")
    private val root: String get() = Environment.getExternalStorageDirectory().absolutePath

    /** An album's folder kept, as a selection and its arguments: its relative path (Android 10), its files' paths before (not those of folders in it). */
    private fun albumWhere(album: String, where: MutableList<String>, args: MutableList<String>) {
        val dir = album.trim('/')
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            where += "${MediaStore.MediaColumns.RELATIVE_PATH} = ?"
            args += "$dir/"
        } else {
            val base = if (album.startsWith("/")) album.trimEnd('/') else "$root/$dir"
            @Suppress("DEPRECATION")
            val data = MediaStore.MediaColumns.DATA
            where += "$data LIKE ? ESCAPE '\\' AND $data NOT LIKE ? ESCAPE '\\'"
            args += like("$base/") + "%"
            args += like("$base/") + "%/%"
        }
    }

    private fun selection(q: Query): Pair<String, Array<String>> {
        val where = mutableListOf(mediaTypes(q.kind))
        val args = mutableListOf<String>()
        if (q.screenshots) {
            where += "$pathColumn LIKE ?"
            args += "%Screenshot%"
        }
        if (q.album.isNotEmpty()) albumWhere(q.album, where, args)
        return where.joinToString(" AND ") { "($it)" } to args.toTypedArray()
    }

    private fun query(projection: Array<String>, q: Query, offset: Int, limit: Int): Cursor? {
        val (where, args) = selection(q)
        val sort = "${MediaStore.MediaColumns.DATE_ADDED} DESC"
        return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            val b = Bundle().apply {
                putString(ContentResolver.QUERY_ARG_SQL_SELECTION, where)
                putStringArray(ContentResolver.QUERY_ARG_SQL_SELECTION_ARGS, args)
                putString(ContentResolver.QUERY_ARG_SQL_SORT_ORDER, sort)
                if (limit > 0) {
                    putInt(ContentResolver.QUERY_ARG_LIMIT, limit)
                    putInt(ContentResolver.QUERY_ARG_OFFSET, offset)
                }
            }
            cr.query(files, projection, b, null)
        } else {
            // Before Android 11 MediaStore takes no limit or offset: the
            // cursor starts at offset, and limit rows are read from it.
            cr.query(files, projection, where, args, sort)?.also { if (limit > 0) it.moveToPosition(offset - 1) }
        }
    }

    private fun list(q: Query, offset: Int, limit: Int): JSONArray {
        val fav = Build.VERSION.SDK_INT >= Build.VERSION_CODES.R
        val projection = mutableListOf(
            MediaStore.MediaColumns._ID, MediaStore.MediaColumns.WIDTH, MediaStore.MediaColumns.HEIGHT,
            MediaStore.Images.ImageColumns.DATE_TAKEN, MediaStore.MediaColumns.DATE_ADDED, pathColumn,
            MediaStore.Files.FileColumns.MEDIA_TYPE, MediaStore.Video.VideoColumns.DURATION, MediaStore.Images.ImageColumns.ORIENTATION,
        )
        if (fav) projection += MediaStore.MediaColumns.IS_FAVORITE
        val out = JSONArray()
        val n = if (limit <= 0) 100 else limit
        query(projection.toTypedArray(), q, offset, n)?.use { c ->
            while (out.length() < n && c.moveToNext()) {
                val taken = c.getLong(3)
                val video = c.getInt(6) == MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO
                var w = c.getInt(1)
                var h = c.getInt(2)
                // Its size upright: MediaStore's is as stored, turned by its orientation.
                if (c.getInt(8) % 180 != 0) w = h.also { h = w }
                out.put(JSONObject()
                    .put("id", c.getLong(0).toString()).put("width", w).put("height", h)
                    .put("created", if (taken > 0) taken else c.getLong(4) * 1000)
                    .put("screenshot", (c.getString(5) ?: "").contains("Screenshot", ignoreCase = true))
                    .put("video", video)
                    .put("duration", if (video) c.getLong(7) else 0L)
                    .put("favorite", fav && c.getInt(9) != 0))
            }
        }
        return out
    }

    private fun count(q: Query): Int = query(arrayOf(MediaStore.MediaColumns._ID), q, 0, 0)?.use { it.count } ?: 0

    // -----------------------------------------------------------------
    // An item

    private class Item(val uri: Uri, val video: Boolean, val width: Int, val height: Int, val turn: Int, val name: String, val mime: String, val taken: Long)

    /** An item by its id: its image's or video's URI, what it is. */
    private fun item(id: String): Item {
        val n = id.toLongOrNull() ?: throw Exception("photos: no such photo")
        val projection = arrayOf(
            MediaStore.Files.FileColumns.MEDIA_TYPE, MediaStore.MediaColumns.WIDTH, MediaStore.MediaColumns.HEIGHT,
            MediaStore.Images.ImageColumns.ORIENTATION, MediaStore.MediaColumns.DISPLAY_NAME, MediaStore.MediaColumns.MIME_TYPE,
            MediaStore.Images.ImageColumns.DATE_TAKEN,
        )
        cr.query(files, projection, "${MediaStore.MediaColumns._ID} = ? AND (${mediaTypes(2)})", arrayOf(n.toString()), null)?.use { c ->
            if (c.moveToFirst()) {
                val video = c.getInt(0) == MediaStore.Files.FileColumns.MEDIA_TYPE_VIDEO
                val base = if (video) MediaStore.Video.Media.EXTERNAL_CONTENT_URI else MediaStore.Images.Media.EXTERNAL_CONTENT_URI
                return Item(ContentUris.withAppendedId(base, n), video, c.getInt(1), c.getInt(2), c.getInt(3),
                    c.getString(4) ?: "", c.getString(5) ?: "", c.getLong(6))
            }
        }
        throw Exception("photos: no such photo")
    }

    private fun thumbnail(id: String, size: Int): String {
        val dir = File(rt.app.cacheDir, "asuka-photo-thumbs").apply { mkdirs() }
        val f = File(dir, "$id-$size.jpg")
        if (f.isFile) return f.path
        val it = item(id)
        // It fits in the box it's given, and the short side is the one
        // asked for: a long screenshot's box is as long as it is.
        var long = size
        if (minOf(it.width, it.height) > 0) long = minOf(maxOf(it.width, it.height) * size / minOf(it.width, it.height), size * 8)
        val bmp: Bitmap = when {
            Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q -> cr.loadThumbnail(it.uri, Size(long, long), null)
            it.video -> videoFrame(it.uri, long)
            else -> decodeThumbnail(it.uri, long, it.turn)
        }
        val tmp = File(dir, UUID.randomUUID().toString())
        tmp.outputStream().use { bmp.compress(Bitmap.CompressFormat.JPEG, 80, it) }
        bmp.recycle()
        tmp.renameTo(f)
        return f.path
    }

    /** Before Android 10, a video's thumbnail: its first frame (turned as it plays), scaled to fit box. */
    private fun videoFrame(u: Uri, box: Int): Bitmap {
        val r = MediaMetadataRetriever()
        try {
            r.setDataSource(rt.app, u)
            var b = r.getFrameAtTime(0, MediaMetadataRetriever.OPTION_CLOSEST_SYNC) ?: throw Exception("photos: no frame in the video")
            val k = box.toFloat() / maxOf(b.width, b.height)
            if (k < 1) {
                val t = Bitmap.createScaledBitmap(b, maxOf(1, (b.width * k).toInt()), maxOf(1, (b.height * k).toInt()), true)
                if (t !== b) b.recycle()
                b = t
            }
            return b
        } finally {
            r.release()
        }
    }

    /**
     * Before Android 10 there's no loadThumbnail: the photo decoded at the
     * largest sample that still fills box, scaled to fit it and turned as
     * it was taken (MediaStore's orientation), as loadThumbnail gives it.
     */
    private fun decodeThumbnail(u: Uri, box: Int, turn: Int): Bitmap {
        val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
        cr.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, bounds) } ?: throw Exception("photos: no such photo")
        var sample = 1
        while (maxOf(bounds.outWidth, bounds.outHeight) / (sample * 2) >= box) sample *= 2
        var b = cr.openInputStream(u)?.use { BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = sample }) }
            ?: throw Exception("photos: not an image")
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

    /** The photo's or the video's file as it is (Android keeps no original beside an edit). */
    private fun export(id: String): JSONObject {
        val it = item(id)
        val ext = it.name.substringAfterLast('.', "").ifEmpty { if (it.video) "mp4" else "jpg" }
        val dir = File(rt.app.cacheDir, "asuka-photo-exports").apply { mkdirs() }
        val f = File(dir, "${UUID.randomUUID()}.$ext")
        val input = cr.openInputStream(it.uri) ?: throw Exception("photos: no such photo")
        input.use { i -> f.outputStream().use { o -> i.copyTo(o) } }
        return JSONObject().put("path", f.path).put("name", it.name)
    }

    // -----------------------------------------------------------------
    // Albums

    /** An item's folder as an album's id: its relative path (Android 10), its file's folder from the storage's root before. */
    private fun albumOf(path: String): String {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) return path.trim('/')
        val dir = path.substringBeforeLast('/', "")
        return if (dir.startsWith("$root/")) dir.removePrefix("$root/") else dir
    }

    /** The folders that hold photos or videos, the one with the newest first. */
    private fun albums(): JSONArray {
        class Folder(val title: String) { var count = 0 }
        val folders = LinkedHashMap<String, Folder>()
        val projection = arrayOf(pathColumn, MediaStore.Images.ImageColumns.BUCKET_DISPLAY_NAME)
        cr.query(files, projection, mediaTypes(2), null, "${MediaStore.MediaColumns.DATE_ADDED} DESC")?.use { c ->
            while (c.moveToNext()) {
                val id = albumOf(c.getString(0) ?: continue)
                if (id.isEmpty()) continue
                folders.getOrPut(id) { Folder(c.getString(1) ?: id.substringAfterLast('/')) }.count++
            }
        }
        val out = JSONArray()
        for ((id, f) in folders) out.put(JSONObject().put("id", id).put("title", f.title).put("count", f.count).put("system", false))
        return out
    }

    /** An album named title: a folder of Pictures, there once something is added to it. */
    private fun createAlbum(title: String): JSONObject {
        val t = title.trim().replace('/', '_').replace('\u0000', '_')
        if (t.isEmpty() || t == "." || t == "..") throw Exception("photos: an album needs a title")
        val id = "${Environment.DIRECTORY_PICTURES}/$t"
        synchronized(made) { made += id }
        val n = count(Query(JSONObject().put("kind", 2).put("album", id)))
        return JSONObject().put("id", id).put("title", t).put("count", n).put("system", false)
    }

    /** Copies each item into the album's folder: new items, the originals left where they were. */
    private fun addToAlbum(album: String, ids: JSONArray) {
        val dir = album.trim('/')
        if (dir.isEmpty()) throw Exception("photos: no such album")
        val known = synchronized(made) { album in made } || count(Query(JSONObject().put("kind", 2).put("album", album))) > 0
        if (!known) throw Exception("photos: no such album")
        val items = List(ids.length()) { item(ids.getString(it)) }
        for (it in items) copy(it, dir)
    }

    private fun copy(it: Item, dir: String) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
            val base = if (it.video) MediaStore.Video.Media.EXTERNAL_CONTENT_URI else MediaStore.Images.Media.EXTERNAL_CONTENT_URI
            val v = ContentValues().apply {
                put(MediaStore.MediaColumns.DISPLAY_NAME, it.name)
                put(MediaStore.MediaColumns.MIME_TYPE, it.mime)
                put(MediaStore.MediaColumns.RELATIVE_PATH, "$dir/")
                if (it.taken > 0) put(MediaStore.MediaColumns.DATE_TAKEN, it.taken)
                put(MediaStore.MediaColumns.IS_PENDING, 1)
            }
            val dest = cr.insert(base, v) ?: throw Exception("photos: couldn't add to the album")
            try {
                val i = cr.openInputStream(it.uri) ?: throw Exception("photos: no such photo")
                i.use { input -> cr.openOutputStream(dest)!!.use { o -> input.copyTo(o) } }
                cr.update(dest, ContentValues().apply { put(MediaStore.MediaColumns.IS_PENDING, 0) }, null, null)
            } catch (e: Exception) {
                cr.delete(dest, null, null)
                throw e
            }
        } else {
            val folder = if (dir.startsWith("/")) File(dir) else File(root, dir)
            if (!folder.isDirectory && !folder.mkdirs()) throw Exception("photos: couldn't make the album's folder")
            var f = File(folder, it.name.ifEmpty { UUID.randomUUID().toString() })
            var k = 1
            while (f.exists()) f = File(folder, "${it.name.substringBeforeLast('.')} ($k).${it.name.substringAfterLast('.', "jpg")}").also { k++ }
            val i = cr.openInputStream(it.uri) ?: throw Exception("photos: no such photo")
            i.use { input -> f.outputStream().use { o -> input.copyTo(o) } }
            // In the library once scanned.
            val scanned = CountDownLatch(1)
            MediaScannerConnection.scanFile(rt.app, arrayOf(f.path), arrayOf(it.mime)) { _, _ -> scanned.countDown() }
            scanned.await(10, TimeUnit.SECONDS)
        }
    }

    // -----------------------------------------------------------------
    // Deleting

    /** Shows an IntentSender's screen (the system's question) and answers whether the user said yes. Main thread. */
    private fun ask(sender: IntentSender, done: (Boolean) -> Unit) {
        val act = rt.activity ?: return done(false)
        var launcher: androidx.activity.result.ActivityResultLauncher<IntentSenderRequest>? = null
        launcher = act.activityResultRegistry.register("asuka-photos-delete-${UUID.randomUUID()}",
            ActivityResultContracts.StartIntentSenderForResult()) { r ->
            launcher?.unregister()
            done(r.resultCode == Activity.RESULT_OK)
        }
        try {
            launcher.launch(IntentSenderRequest.Builder(sender).build())
        } catch (e: Exception) {
            launcher.unregister()
            done(false)
        }
    }

    /**
     * Deletes items once the user says so: Android 11 asks once for them
     * all (createTrashRequest: to the trash, as the doc says, emptied after
     * 30 days); Android 10 asks for each one the app didn't add; before,
     * the storage permission deletes them.
     */
    private fun delete(ids: List<String>, reply: (Result<ByteArray>) -> Unit) {
        if (ids.isEmpty()) return reply(Result.success("true".toByteArray()))
        Thread {
            val uris = try { ids.map { item(it).uri } } catch (e: Exception) { return@Thread reply(Result.failure(e)) }
            when {
                Build.VERSION.SDK_INT >= Build.VERSION_CODES.R -> {
                    val pi = MediaStore.createTrashRequest(cr, uris, true)
                    rt.onMain { ask(pi.intentSender) { ok -> reply(Result.success(ok.toString().toByteArray())) } }
                }
                Build.VERSION.SDK_INT == Build.VERSION_CODES.Q -> {
                    try {
                        for (u in uris) {
                            if (!deleteAsking(u)) return@Thread reply(Result.success("false".toByteArray()))
                        }
                        reply(Result.success("true".toByteArray()))
                    } catch (e: Exception) {
                        reply(Result.failure(e))
                    }
                }
                else -> rt.onMain {
                    rt.withPermission(arrayOf(Manifest.permission.WRITE_EXTERNAL_STORAGE)) { ok ->
                        if (!ok) reply(Result.failure(Exception("photos: not allowed")))
                        else off(reply) {
                            for (u in uris) cr.delete(u, null, null)
                            "true".toByteArray()
                        }
                    }
                }
            }
        }.start()
    }

    /** Android 10: deletes an item, asking the user (its RecoverableSecurityException's screen) for one another app added. Not on the main thread. */
    private fun deleteAsking(u: Uri): Boolean {
        try {
            cr.delete(u, null, null)
            return true
        } catch (e: SecurityException) {
            if (Build.VERSION.SDK_INT < Build.VERSION_CODES.Q || e !is RecoverableSecurityException) throw e
            val answered = CountDownLatch(1)
            var ok = false
            rt.onMain { ask(e.userAction.actionIntent.intentSender) { ok = it; answered.countDown() } }
            answered.await()
            if (!ok) return false
            cr.delete(u, null, null)
            return true
        }
    }

    // -----------------------------------------------------------------
    // Changes

    private val main = Handler(Looper.getMainLooper())
    private var observer: ContentObserver? = null
    private var waiting: ((Result<ByteArray>) -> Unit)? = null
    private var changed = false
    private val inserted = LinkedHashSet<String>()
    private val removed = LinkedHashSet<String>()
    private val updated = LinkedHashSet<String>()
    private var delivering = false

    /** The next change (kept since the last, if any came), as JSON; waits for one. Main thread. */
    private fun nextChange(reply: (Result<ByteArray>) -> Unit) {
        observe()
        waiting?.invoke(Result.failure(Exception("stopped")))
        waiting = reply
        if (changed) deliverSoon()
    }

    private fun stopChanges() {
        waiting?.invoke(Result.failure(Exception("stopped")))
        waiting = null
        observer?.let { cr.unregisterContentObserver(it) }
        observer = null
        changed = false
        inserted.clear(); removed.clear(); updated.clear()
    }

    private fun observe() {
        if (observer != null) return
        val o = object : ContentObserver(main) {
            override fun onChange(selfChange: Boolean) = onChange(selfChange, null as Uri?)

            override fun onChange(selfChange: Boolean, uri: Uri?) = note(listOfNotNull(uri), 0)

            override fun onChange(selfChange: Boolean, uris: Collection<Uri>, flags: Int) = note(uris, flags)
        }
        cr.registerContentObserver(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, true, o)
        cr.registerContentObserver(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, true, o)
        observer = o
    }

    /**
     * A change MediaStore notified: the ids its URIs name, by what it says
     * happened to them (Android 11's flags); before, or without flags,
     * only that something changed.
     */
    private fun note(uris: Collection<Uri>, flags: Int) {
        changed = true
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && flags != 0) {
            for (u in uris) {
                val id = u.lastPathSegment?.takeIf { s -> s.all { it.isDigit() } } ?: continue
                when {
                    flags and ContentResolver.NOTIFY_DELETE != 0 -> {
                        if (!inserted.remove(id)) removed += id
                        updated.remove(id)
                    }
                    flags and ContentResolver.NOTIFY_INSERT != 0 -> { removed.remove(id); inserted += id }
                    flags and ContentResolver.NOTIFY_UPDATE != 0 -> if (id !in inserted) updated += id
                }
            }
        }
        if (waiting != null) deliverSoon()
    }

    /** Answers the waiting call a moment after a change, so one save's several notifications are one change. */
    private fun deliverSoon() {
        if (delivering) return
        delivering = true
        main.postDelayed({
            delivering = false
            val w = waiting ?: return@postDelayed
            if (!changed) return@postDelayed
            val ins = inserted.toList()
            val rem = removed.toList()
            val upd = updated.toList()
            waiting = null
            changed = false
            inserted.clear(); removed.clear(); updated.clear()
            Thread {
                // Moved to the trash (Android 11) is an update to MediaStore,
                // and a removal to the app: those no longer listed are removed.
                val gone = if (upd.isEmpty()) emptySet() else upd.toSet() - visible(upd)
                val c = JSONObject().put("inserted", JSONArray(ins)).put("removed", JSONArray(rem + gone))
                    .put("updated", JSONArray(upd.filter { it !in gone }))
                w(Result.success(c.toString().toByteArray()))
            }.start()
        }, 150)
    }

    /** Of these ids, those a list shows (not trashed, not pending, not gone). Not on the main thread. */
    private fun visible(ids: List<String>): Set<String> {
        val out = HashSet<String>()
        // SQLite takes at most 999 arguments before 3.32.
        for (part in ids.chunked(500)) {
            val marks = part.joinToString(",") { "?" }
            runCatching {
                cr.query(files, arrayOf(MediaStore.MediaColumns._ID), "${MediaStore.MediaColumns._ID} IN ($marks) AND (${mediaTypes(2)})",
                    part.toTypedArray(), null)?.use { c -> while (c.moveToNext()) out += c.getLong(0).toString() }
            }.onFailure { out += part }
        }
        return out
    }
}
