// platform/car for an audio app on Android Auto: Android Auto shows media
// apps with its own browser and player, from a MediaBrowserService. Its
// tree is the templates': the root's rows (a tab bar's tabs, as Android
// Auto's own tabs), and a browsable row's children are the rows of what
// choosing it pushes. Choosing a playable row runs its handler, which
// plays: Android Auto's player shows the app's MediaSession, the host's,
// which platform/nowplaying fills and takes the player's commands with.
package platform.car

import android.content.ContentProvider
import android.content.ContentValues
import android.content.pm.ApplicationInfo
import android.database.Cursor
import android.graphics.Bitmap
import android.media.MediaDescription
import android.media.browse.MediaBrowser
import android.media.session.MediaSession
import android.net.Uri
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.ParcelFileDescriptor
import android.service.media.MediaBrowserService
import dev.asuka.AsukaMediaSession

private const val ROOT = "asuka.root"
private const val GEARHEAD = "com.google.android.projection.gearhead"
private const val CONTENT_STYLE_SUPPORTED = "android.media.browse.CONTENT_STYLE_SUPPORTED"
private const val CONTENT_STYLE_BROWSABLE = "android.media.browse.CONTENT_STYLE_BROWSABLE_HINT"
private const val CONTENT_STYLE_PLAYABLE = "android.media.browse.CONTENT_STYLE_PLAYABLE_HINT"
private const val CONTENT_STYLE_LIST = 1
private const val CONTENT_STYLE_GRID = 2

/** Where a row is: its template's place on the stack, its tab (-1: none), its place among the template's rows. */
private data class Where(val depth: Int, val tab: Int, val index: Int) {
    val id get() = "asuka.$depth.$tab.$index"

    companion object {
        fun parse(id: String): Where? {
            val p = id.removePrefix("asuka.").split('.')
            if (p.size != 3) return null
            return Where(p[0].toIntOrNull() ?: return null, p[1].toIntOrNull() ?: return null, p[2].toIntOrNull() ?: return null)
        }
    }
}

/** The rows a template shows in a media browser: a list's, a grid's, places'. */
private fun rows(s: Shown): List<ShownItem> = s.sections.flatMap { it.items } + s.places.map { ShownItem(run = it.run, title = it.title, detail = it.detail) }

class AsukaCarMediaService : MediaBrowserService() {
    private val tree by lazy { MediaTree(packageName) }
    private val playing = object : MediaSession.Callback() {
        override fun onPlayFromMediaId(mediaId: String?, extras: Bundle?) {
            tree.play(mediaId ?: return)
        }
    }

    override fun onCreate() {
        super.onCreate()
        CarHub.start(this)
        val s = AsukaMediaSession.get(this)
        AsukaMediaSession.addCallback(playing)
        sessionToken = s.sessionToken
    }

    override fun onDestroy() {
        AsukaMediaSession.removeCallback(playing)
        CarHub.media = false
        super.onDestroy()
    }

    override fun onGetRoot(clientPackageName: String, clientUid: Int, rootHints: Bundle?): BrowserRoot {
        // Android Auto's (and, in a debug build, any browser's: a test's).
        if (clientPackageName == GEARHEAD || applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE != 0) CarHub.media = true
        val extras = Bundle().apply {
            putBoolean(CONTENT_STYLE_SUPPORTED, true)
            putInt(CONTENT_STYLE_BROWSABLE, if (CarHub.stack.firstOrNull()?.kind == Kind.GRID) CONTENT_STYLE_GRID else CONTENT_STYLE_LIST)
            putInt(CONTENT_STYLE_PLAYABLE, CONTENT_STYLE_LIST)
        }
        return BrowserRoot(ROOT, extras)
    }

    override fun onLoadChildren(parentId: String, result: Result<List<MediaBrowser.MediaItem>>) {
        result.detach()
        tree.load(parentId) { result.sendResult(it) }
    }
}

/** The media browser's tree, from Asuka's stack. Main thread. */
internal class MediaTree(private val packageName: String) {
    private val main = Handler(Looper.getMainLooper())

    /** The template at depth (on tab, if it's a tab bar's). */
    private fun template(depth: Int, tab: Int): Shown? {
        val s = CarHub.stack.getOrNull(depth) ?: return null
        return if (tab >= 0) s.tabs.getOrNull(tab) else s
    }

    private fun item(w: Where): ShownItem? = template(w.depth, w.tab)?.let { rows(it).getOrNull(w.index) }

    /** Android Auto plays a row: its handler runs. */
    fun play(mediaId: String) {
        val w = Where.parse(mediaId) ?: return
        item(w)?.let { CarHub.chose(it.run) }
    }

    /** Answers with parentId's children, now or once Asuka shows them. */
    fun load(parentId: String, answer: (List<MediaBrowser.MediaItem>) -> kotlin.Unit) {
        if (parentId == ROOT) {
            // Before the app's Screen shows (its code is starting), once it does.
            waitFor({ CarHub.stack.isNotEmpty() }) {
                val root = CarHub.stack.firstOrNull()
                if (root?.kind == Kind.TABS) {
                    answer(root.tabs.mapIndexed { n, t ->
                        MediaBrowser.MediaItem(MediaDescription.Builder().setMediaId("asuka.tab.$n").setTitle(t.text.ifEmpty { t.title })
                            .setExtras(Bundle().apply { putInt(CONTENT_STYLE_BROWSABLE, if (t.kind == Kind.GRID) CONTENT_STYLE_GRID else CONTENT_STYLE_LIST) })
                            .apply { iconUri(t.symbol, symbol = true)?.let { setIconUri(it) } }.build(), MediaBrowser.MediaItem.FLAG_BROWSABLE)
                    })
                } else {
                    answer(children(0, -1))
                }
            }
            return
        }
        if (parentId.startsWith("asuka.tab.")) {
            answer(children(0, parentId.removePrefix("asuka.tab.").toIntOrNull() ?: 0))
            return
        }
        // A browsable row: what choosing it pushes, over the template it's on.
        val w = Where.parse(parentId)
        val i = w?.let { item(it) }
        if (w == null || i == null || i.run <= 0) {
            answer(emptyList())
            return
        }
        if (CarHub.stack.size > w.depth + 1) CarHub.back(w.depth + 1)
        val before = CarHub.stack.getOrNull(w.depth + 1)?.id
        CarHub.chose(i.run)
        val pushed = { CarHub.stack.size > w.depth + 1 && CarHub.stack[w.depth + 1].id != before }
        waitFor(pushed) { answer(if (pushed()) children(w.depth + 1, -1) else emptyList()) }
    }

    /** Runs then once ready says so (as Asuka shows a stack), or after 5 seconds. */
    private fun waitFor(ready: () -> Boolean, then: () -> kotlin.Unit) {
        if (ready()) return then()
        var done = false
        lateinit var l: () -> kotlin.Unit
        val finish = {
            if (!done) {
                done = true
                CarHub.offShown(l)
                then()
            }
        }
        l = { if (ready()) finish() }
        CarHub.onShown(l)
        main.postDelayed({ finish() }, 5000)
    }

    private fun children(depth: Int, tab: Int): List<MediaBrowser.MediaItem> {
        val s = template(depth, tab) ?: return emptyList()
        return rows(s).mapIndexed { n, i ->
            val d = MediaDescription.Builder().setMediaId(Where(depth, tab, n).id).setTitle(i.title)
            if (i.detail.isNotEmpty()) d.setSubtitle(i.detail)
            (iconUri(i.image, symbol = false) ?: iconUri(i.symbol, symbol = true))?.let { d.setIconUri(it) }
            MediaBrowser.MediaItem(d.build(), if (i.browsable) MediaBrowser.MediaItem.FLAG_BROWSABLE else MediaBrowser.MediaItem.FLAG_PLAYABLE)
        }
    }

    /** An image for Android Auto to read: the package's provider's (AsukaCarImages). */
    private fun iconUri(source: String, symbol: Boolean): Uri? {
        if (source.isEmpty()) return null
        return Uri.Builder().scheme("content").authority("$packageName.asuka.car").path(if (symbol) "symbol" else "image")
            .appendQueryParameter("s", source).build()
    }
}

/**
 * The car's images, for Android Auto's media browser, which reads its
 * rows' images from URIs: an image of the app's (an asset, a file, an
 * http(s) URL) or a symbol, as a PNG.
 */
class AsukaCarImages : ContentProvider() {
    override fun onCreate() = true

    override fun openFile(uri: Uri, mode: String): ParcelFileDescriptor? {
        val ctx = context ?: return null
        val source = uri.getQueryParameter("s") ?: return null
        val symbol = uri.path == "/symbol"
        val main = Handler(Looper.getMainLooper())
        val got = java.util.concurrent.LinkedBlockingQueue<Any>(1)
        main.post {
            if (symbol) {
                got.offer(CarImages.symbol(ctx, source, 128) ?: Unit)
            } else {
                CarImages.cached(source)?.let { got.offer(it); return@post }
                CarImages.get(ctx, source) { got.offer(CarImages.cached(source) ?: Unit) }
            }
        }
        val b = got.poll(10, java.util.concurrent.TimeUnit.SECONDS) as? Bitmap ?: return null
        val (read, write) = ParcelFileDescriptor.createPipe()
        Thread {
            ParcelFileDescriptor.AutoCloseOutputStream(write).use { b.compress(Bitmap.CompressFormat.PNG, 100, it) }
        }.start()
        return read
    }

    override fun getType(uri: Uri) = "image/png"
    override fun query(uri: Uri, projection: Array<out String>?, selection: String?, selectionArgs: Array<out String>?, sortOrder: String?): Cursor? = null
    override fun insert(uri: Uri, values: ContentValues?): Uri? = null
    override fun delete(uri: Uri, selection: String?, selectionArgs: Array<out String>?) = 0
    override fun update(uri: Uri, values: ContentValues?, selection: String?, selectionArgs: Array<out String>?) = 0
}
