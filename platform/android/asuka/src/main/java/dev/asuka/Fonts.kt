package dev.asuka

import android.content.Context
import android.graphics.Typeface
import android.graphics.fonts.Font
import android.graphics.fonts.FontFamily
import android.graphics.fonts.FontStyle
import android.os.Build
import android.util.Log
import androidx.annotation.RequiresApi
import androidx.core.graphics.TypefaceCompat
import java.io.File
import java.net.HttpURLConnection
import java.net.URL

/**
 * The app's fonts: the files in its assets/fonts (or, under asuka dev,
 * those pushed to its files), a family of them (Inter: Inter-Regular,
 * Inter-Bold...), the system's families (serif), and under asuka dev
 * fonts by URL. Main thread.
 */
object AsukaFonts {
    private val cache = HashMap<String, Typeface?>()
    private val urls = HashMap<String, Typeface>()
    private val loading = HashSet<String>()
    private var sources: Map<String, String>? = null
    /** Counts the fonts by URL loaded, for caches of text laid out in them. */
    var version = 0
        private set
    /** Runs when a font by URL has loaded (its URL). */
    var loaded: ((String) -> Unit)? = null

    /** A typeface of family in weight (100-900; 0: the family's regular), or null: none such, or not loaded yet. */
    fun typeface(ctx: Context, family: String, weight: Int, italic: Boolean = false): Typeface? {
        if ("://" in family) {
            urls[family]?.let { return if (italic) Typeface.create(it, Typeface.ITALIC) else it }
            load(ctx, family)
            return null
        }
        val key = "$family|$weight|$italic"
        if (cache.containsKey(key)) return cache[key]
        val t = try { resolve(ctx, family, weight, italic) } catch (e: Exception) { Log.w("asuka", "font $family: $e"); null }
        cache[key] = t
        return t
    }

    /**
     * t in weight (1-1000). Before Android 9 a typeface takes no weight:
     * TypefaceCompat picks the system family's nearest (bold from 600 when
     * it can't).
     */
    fun weighted(ctx: Context, t: Typeface?, weight: Int, italic: Boolean): Typeface =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) Typeface.create(t, weight, italic)
        else TypefaceCompat.create(ctx, t, weight, italic)

    private fun resolve(ctx: Context, family: String, weight: Int, italic: Boolean): Typeface? {
        val src = sources(ctx)
        src[family]?.let { p ->
            val t = if (p.startsWith("/")) Typeface.createFromFile(p) else Typeface.createFromAsset(ctx.assets, p)
            return if (italic) Typeface.create(t, Typeface.ITALIC) else t
        }
        val faces = src.filterKeys { it.startsWith("$family-") }
        if (faces.isNotEmpty()) {
            return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) faceFamily(ctx, faces, weight, italic)
            else nearestFace(ctx, family, faces, weight, italic)
        }
        // A system family; an unknown one is the default.
        val t = Typeface.create(family, Typeface.NORMAL)
        if (t == Typeface.DEFAULT) return null
        return if (weight > 0) weighted(ctx, t, weight, italic)
        else Typeface.create(t, if (italic) Typeface.ITALIC else Typeface.NORMAL)
    }

    /** The faces as one family, in weight and slant. */
    @RequiresApi(Build.VERSION_CODES.Q)
    private fun faceFamily(ctx: Context, faces: Map<String, String>, weight: Int, italic: Boolean): Typeface {
        var b: FontFamily.Builder? = null
        for (p in faces.values) {
            val f = (if (p.startsWith("/")) Font.Builder(File(p)) else Font.Builder(ctx.assets, p)).build()
            if (b == null) b = FontFamily.Builder(f)
            else try { b.addFont(f) } catch (_: IllegalArgumentException) {} // a style it has
        }
        val style = FontStyle(if (weight > 0) weight else 400, if (italic) FontStyle.FONT_SLANT_ITALIC else FontStyle.FONT_SLANT_UPRIGHT)
        return Typeface.CustomFallbackBuilder(b!!.build()).setSystemFallback("sans-serif").setStyle(style).build()
    }

    /**
     * Before Android 10 there's no Font.Builder to make a family of the
     * faces: the face its file's name says is nearest (Inter-SemiBold:
     * 600), slanted or emboldened by Android when there's none such.
     */
    private fun nearestFace(ctx: Context, family: String, faces: Map<String, String>, weight: Int, italic: Boolean): Typeface {
        val want = if (weight > 0) weight else 400
        val (name, p) = faces.entries.minBy { (k, _) ->
            val (w, i) = faceStyle(k.removePrefix("$family-"))
            (if (i != italic) 10000 else 0) + kotlin.math.abs(w - want)
        }
        val (w, i) = faceStyle(name.removePrefix("$family-"))
        val t = if (p.startsWith("/")) Typeface.createFromFile(p) else Typeface.createFromAsset(ctx.assets, p)
        val bold = want >= 600 && w < 600
        val slant = italic && !i
        return when {
            bold && slant -> Typeface.create(t, Typeface.BOLD_ITALIC)
            bold -> Typeface.create(t, Typeface.BOLD)
            slant -> Typeface.create(t, Typeface.ITALIC)
            else -> t
        }
    }

    /** A face's weight and slant from its file's name: SemiBoldItalic is 600, italic. */
    private fun faceStyle(suffix: String): Pair<Int, Boolean> {
        val s = suffix.lowercase().filter { it.isLetter() }
        val italic = "italic" in s || "oblique" in s
        val weight = when (s.replace("italic", "").replace("oblique", "")) {
            "thin", "hairline" -> 100
            "extralight", "ultralight" -> 200
            "light" -> 300
            "medium" -> 500
            "semibold", "demibold" -> 600
            "bold" -> 700
            "extrabold", "ultrabold" -> 800
            "black", "heavy" -> 900
            else -> 400 // Regular, Book, Normal
        }
        return weight to italic
    }

    /** A font file's name (no extension) → its path: a pushed file's, or an asset's ("fonts/Inter-Bold.ttf"). */
    private fun sources(ctx: Context): Map<String, String> {
        sources?.let { return it }
        val out = HashMap<String, String>()
        fun font(name: String) = name.endsWith(".ttf", true) || name.endsWith(".otf", true)
        ctx.assets.list("fonts")?.filter(::font)?.forEach { out[it.substringBeforeLast('.')] = "fonts/$it" }
        File(ctx.filesDir, "asuka/assets/fonts").listFiles()?.filter { font(it.name) }?.forEach {
            out[it.name.substringBeforeLast('.')] = it.path
        }
        sources = out
        return out
    }

    private fun load(ctx: Context, url: String) {
        if (!loading.add(url)) return // a failed load isn't tried again
        val dir = File(ctx.cacheDir, "asuka-fonts").apply { mkdirs() }
        Thread {
            try {
                val c = URL(url).openConnection() as HttpURLConnection
                val bytes = try {
                    if (c.responseCode !in 200..299) error("HTTP ${c.responseCode}")
                    c.inputStream.use { it.readBytes() }
                } finally { c.disconnect() }
                val f = File(dir, Integer.toHexString(url.hashCode()) + "." + url.substringAfterLast('.'))
                f.writeBytes(bytes)
                val t = Typeface.createFromFile(f)
                android.os.Handler(android.os.Looper.getMainLooper()).post {
                    urls[url] = t
                    version++
                    loaded?.invoke(url)
                }
            } catch (e: Exception) {
                Log.w("asuka", "font $url: $e")
            }
        }.start()
    }
}
