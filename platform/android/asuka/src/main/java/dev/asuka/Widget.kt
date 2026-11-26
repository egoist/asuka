package dev.asuka

import android.app.AlarmManager
import android.app.PendingIntent
import android.appwidget.AppWidgetManager
import android.appwidget.AppWidgetProvider
import android.appwidget.AppWidgetProviderInfo
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.content.res.ColorStateList
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.SystemClock
import android.util.SizeF
import android.view.View
import android.widget.RemoteViews
import org.json.JSONArray
import org.json.JSONObject

/**
 * The App Widget of a kind asuka.json declares: the CLI makes a subclass
 * for each (dev.asuka.app.widgets), a receiver in the manifest with the
 * kind in its meta-data (dev.asuka.widget.kind). It shows the kind's
 * entries, which platform/widget's "set" keeps in the app's preferences: the
 * one whose time has come, laid out for the widget's size (small, medium,
 * large, as responsive layouts) by the template or the app's own layout
 * (WidgetLayout.kt), and wakes for the next one, and for a countdown's
 * end, with an alarm.
 */
open class AsukaWidgetProvider(private val kind: String, private val sizes: String) : AppWidgetProvider() {
    override fun onUpdate(ctx: Context, mgr: AppWidgetManager, ids: IntArray) {
        for (id in ids) mgr.updateAppWidget(id, views(ctx, mgr, id))
        schedule(ctx, ids)
    }

    override fun onAppWidgetOptionsChanged(ctx: Context, mgr: AppWidgetManager, id: Int, options: Bundle) {
        mgr.updateAppWidget(id, views(ctx, mgr, id))
    }

    private fun views(ctx: Context, mgr: AppWidgetManager, id: Int): RemoteViews {
        val entry = AsukaWidgets.current(AsukaWidgets.entries(ctx, kind))
        val ss = sizes.split(',')
        fun layout(size: String) = AsukaWidgets.render(ctx, kind, entry, size)
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            // Each size it comes in, from the smallest box that takes it.
            val m = LinkedHashMap<SizeF, RemoteViews>()
            if ("small" in ss) m[SizeF(40f, 40f)] = layout("small")
            if ("medium" in ss) m[SizeF(200f, 40f)] = layout("medium")
            if ("large" in ss) m[SizeF(200f, 200f)] = layout("large")
            if (m.isEmpty()) m[SizeF(40f, 40f)] = layout("small")
            return RemoteViews(m)
        }
        // Before Android 12 there are no responsive layouts: the one for the
        // widget's size now (in dp), as the boxes above choose it; a resize
        // lays it out again (onAppWidgetOptionsChanged).
        val o = mgr.getAppWidgetOptions(id)
        val w = o.getInt(AppWidgetManager.OPTION_APPWIDGET_MIN_WIDTH)
        val h = o.getInt(AppWidgetManager.OPTION_APPWIDGET_MIN_HEIGHT)
        val have = listOf("small", "medium", "large").filter { it in ss }.ifEmpty { listOf("small") }
        val fits = have.filter { when (it) { "large" -> w >= 200 && h >= 200; "medium" -> w >= 200 && h >= 40; else -> true } }
        return layout(fits.lastOrNull() ?: have.first())
    }

    /** An alarm for the next entry's time, or a countdown's end: then it updates again. */
    private fun schedule(ctx: Context, ids: IntArray) {
        val now = System.currentTimeMillis()
        val es = AsukaWidgets.entries(ctx, kind)
        val next = listOfNotNull(
            es.map { it.optLong("atMs") }.filter { it > now }.minOrNull(),
            AsukaWidgets.current(es)?.optLong("untilMs")?.takeIf { it > now },
        ).minOrNull()
        val am = ctx.getSystemService(Context.ALARM_SERVICE) as AlarmManager
        val pi = PendingIntent.getBroadcast(ctx, kind.hashCode(),
            Intent(ctx, javaClass).setAction(AppWidgetManager.ACTION_APPWIDGET_UPDATE).putExtra(AppWidgetManager.EXTRA_APPWIDGET_IDS, ids),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
        if (next == null) am.cancel(pi) else am.set(AlarmManager.RTC, next, pi)
    }
}

/** platform/widget's entries, and the template. */
object AsukaWidgets {
    private const val PREFS = "asuka.widgets"

    fun entries(ctx: Context, kind: String): List<JSONObject> {
        val s = ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(kind, null) ?: return emptyList()
        val a = runCatching { JSONArray(s) }.getOrNull() ?: return emptyList()
        return List(a.length()) { a.getJSONObject(it) }.sortedBy { it.optLong("atMs") }
    }

    /** The entry whose time has come: the last at or before now (or the first). */
    fun current(es: List<JSONObject>): JSONObject? {
        val now = System.currentTimeMillis()
        return es.lastOrNull { it.optLong("atMs") <= now } ?: es.firstOrNull()
    }

    /** "#rrggbbaa" as an ARGB int. */
    private fun color(s: String): Int? {
        if (s.length != 9 || !s.startsWith("#")) return null
        val v = s.substring(1).toLongOrNull(16) ?: return null
        return ((v and 0xff) shl 24 or (v ushr 8)).toInt()
    }

    fun render(ctx: Context, kind: String, e: JSONObject?, size: String): RemoteViews {
        val layout = when (size) { "large" -> R.layout.asuka_widget_large; "medium" -> R.layout.asuka_widget_medium; else -> R.layout.asuka_widget_small }
        val v = RemoteViews(ctx.packageName, layout)
        fun text(id: Int, s: String?) {
            if (s.isNullOrEmpty()) v.setViewVisibility(id, View.GONE)
            else { v.setViewVisibility(id, View.VISIBLE); v.setTextViewText(id, s) }
        }
        if (e == null) {
            // Nothing set yet: the widget's name, as the gallery shows it.
            val label = runCatching {
                providers(ctx, AppWidgetManager.getInstance(ctx))
                    .firstOrNull { kindOf(ctx, it.provider) == kind }?.loadLabel(ctx.packageManager)
            }.getOrNull()
            text(R.id.asuka_widget_title, label ?: kind)
            text(R.id.asuka_widget_value, "—")
            for (id in listOf(R.id.asuka_widget_body, R.id.asuka_widget_caption, R.id.asuka_widget_symbol, R.id.asuka_widget_progress, R.id.asuka_widget_until)) {
                v.setViewVisibility(id, View.GONE)
            }
            v.setOnClickPendingIntent(R.id.asuka_widget_root, open(ctx, kind, ""))
            return v
        }
        // The app's own layout, for this size or every size.
        val node = e.optJSONObject("sizes")?.optJSONObject(size) ?: e.optJSONObject("layout")
        if (node != null) {
            val lv = AsukaWidgetLayout.render(ctx, node) { widgetOpen(ctx, "asuka.widget.$kind", it) }
            if (node.optString("url").isEmpty()) lv.setOnClickPendingIntent(R.id.asuka_wl, open(ctx, kind, e.optString("url")))
            return lv
        }
        text(R.id.asuka_widget_title, e.optString("title"))
        text(R.id.asuka_widget_value, e.optString("value"))
        text(R.id.asuka_widget_body, e.optString("body"))
        text(R.id.asuka_widget_caption, e.optString("caption"))
        val tint = color(e.optString("tint")) ?: ctx.getColor(R.color.asuka_widget_accent)
        v.setTextColor(R.id.asuka_widget_title, tint)
        val sym = symbolDrawables[e.optString("symbol")]
        if (sym == null) v.setViewVisibility(R.id.asuka_widget_symbol, View.GONE)
        else {
            v.setViewVisibility(R.id.asuka_widget_symbol, View.VISIBLE)
            v.setImageViewResource(R.id.asuka_widget_symbol, sym)
            v.setInt(R.id.asuka_widget_symbol, "setColorFilter", tint)
        }
        if (e.optBoolean("showProgress")) {
            v.setViewVisibility(R.id.asuka_widget_progress, View.VISIBLE)
            v.setProgressBar(R.id.asuka_widget_progress, 1000, (e.optDouble("progress", 0.0).coerceIn(0.0, 1.0) * 1000).toInt(), false)
            // Before Android 12 a widget's bar can't be tinted: it's the theme's accent.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                v.setColorStateList(R.id.asuka_widget_progress, "setProgressTintList", ColorStateList.valueOf(tint))
            }
        } else v.setViewVisibility(R.id.asuka_widget_progress, View.GONE)
        val until = e.optLong("untilMs")
        if (until > 0) {
            // Down to it, then up from it (the alarm at its end switches).
            val left = until - System.currentTimeMillis()
            v.setViewVisibility(R.id.asuka_widget_until, View.VISIBLE)
            v.setChronometer(R.id.asuka_widget_until, SystemClock.elapsedRealtime() + left, null, true)
            v.setChronometerCountDown(R.id.asuka_widget_until, left > 0)
        } else v.setViewVisibility(R.id.asuka_widget_until, View.GONE)
        v.setOnClickPendingIntent(R.id.asuka_widget_root, open(ctx, kind, e.optString("url")))
        return v
    }

    /** Opens the app, with the entry's URL (ui.OnOpenURL) if it has one. */
    private fun open(ctx: Context, kind: String, url: String): PendingIntent {
        val i = Intent(ctx, AsukaActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
        if (url.isNotEmpty()) i.setAction(Intent.ACTION_VIEW).setData(Uri.parse(url)) else i.setAction(Intent.ACTION_MAIN)
        return PendingIntent.getActivity(ctx, ("asuka.widget." + kind).hashCode(), i, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
    }

    /** The app's widget providers (before Android 8, picked from every app's). */
    private fun providers(ctx: Context, mgr: AppWidgetManager): List<AppWidgetProviderInfo> =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) mgr.getInstalledProvidersForPackage(ctx.packageName, null)
        else mgr.installedProviders.filter { it.provider.packageName == ctx.packageName }

    /** A provider's kind: its receiver's dev.asuka.widget.kind. */
    fun kindOf(ctx: Context, provider: ComponentName): String? = runCatching {
        ctx.packageManager.getReceiverInfo(provider, PackageManager.GET_META_DATA).metaData?.getString("dev.asuka.widget.kind")
    }.getOrNull()

    /** Keeps kind's entries and updates its widgets. */
    fun set(ctx: Context, kind: String, entries: JSONArray) {
        ctx.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(kind, entries.toString()).apply()
        update(ctx, kind)
    }

    /** Updates kind's widgets (null: every kind's). */
    fun update(ctx: Context, kind: String?) {
        val mgr = AppWidgetManager.getInstance(ctx)
        for (p in providers(ctx, mgr)) {
            if (kind != null && kindOf(ctx, p.provider) != kind) continue
            val ids = mgr.getAppWidgetIds(p.provider)
            ctx.sendBroadcast(Intent(AppWidgetManager.ACTION_APPWIDGET_UPDATE).setComponent(p.provider)
                .putExtra(AppWidgetManager.EXTRA_APPWIDGET_IDS, ids))
        }
    }
}

/**
 * Keeps an image for the widgets' layouts (a name, a 0 byte, its data; the
 * name alone deletes it), no larger than the screen's width, which is
 * what a widget's bitmaps may take.
 */
private fun keepWidgetImage(ctx: Context, args: ByteArray) {
    val zero = args.indexOf(0.toByte()).let { if (it < 0) args.size else it }
    val file = AsukaWidgetLayout.imageFile(ctx, String(args, 0, zero, Charsets.UTF_8))
    if (zero == args.size) {
        file.delete()
    } else {
        var b = android.graphics.BitmapFactory.decodeByteArray(args, zero + 1, args.size - zero - 1) ?: throw Exception("widget: not an image")
        val most = ctx.resources.displayMetrics.widthPixels
        val k = most.toFloat() / maxOf(b.width, b.height)
        if (k < 1) b = android.graphics.Bitmap.createScaledBitmap(b, (b.width * k).toInt().coerceAtLeast(1), (b.height * k).toInt().coerceAtLeast(1), true)
        file.parentFile?.mkdirs()
        val tmp = java.io.File(file.path + ".tmp")
        tmp.outputStream().use { b.compress(android.graphics.Bitmap.CompressFormat.PNG, 100, it) }
        if (!tmp.renameTo(file)) throw Exception("widget: can't keep the image")
    }
    AsukaWidgets.update(ctx, null)
}

/** platform/widget's module: "set" (JSON: kind, entries), "image". */
fun AsukaRuntime.widgetMethods(): Map<String, AsukaModuleMethod> = mapOf(
    "set" to { args, reply ->
        val o = runCatching { JSONObject(String(args, Charsets.UTF_8)) }.getOrNull()
        if (o == null) reply(Result.failure(Exception("bad args")))
        else {
            AsukaWidgets.set(app, o.getString("kind"), o.optJSONArray("entries") ?: JSONArray())
            reply(Result.success(ByteArray(0)))
        }
    },
    "image" to { args, reply ->
        Thread { reply(runCatching { keepWidgetImage(app, args); ByteArray(0) }) }.start()
    },
)
