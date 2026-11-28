package dev.asuka

import android.app.PendingIntent
import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Build
import android.os.SystemClock
import android.text.SpannableString
import android.text.Spanned
import android.text.style.StyleSpan
import android.text.style.TypefaceSpan
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.RemoteViews
import org.json.JSONObject
import java.io.File

/**
 * platform/widget's Layout: the app's own layout for a widget, a tree it
 * sends with an entry (its Node on the wire), drawn as RemoteViews: each
 * node one of the asuka_wl_* layouts (its _fill variant when it takes the
 * space its column or row has left), nested with addView, and set with
 * RemoteViews' setters. Semantic colors are the widget's color resources,
 * which the launcher resolves for its light or dark mode; a layer is
 * placed in a cell (asuka_wl_cell) whose gravity says where. A button, a
 * toggle and an action part send the host's receiver their action
 * (AsukaWidgetScope.action), for the app's code.
 */
object AsukaWidgetLayout {
    private const val ID = R.id.asuka_wl

    private fun layoutOf(t: String, mode: String, fill: Boolean): Int = when (t) {
        "column" -> if (fill) R.layout.asuka_wl_column_fill else R.layout.asuka_wl_column
        "row" -> if (fill) R.layout.asuka_wl_row_fill else R.layout.asuka_wl_row
        "layers" -> if (fill) R.layout.asuka_wl_layers_fill else R.layout.asuka_wl_layers
        "text" -> if (fill) R.layout.asuka_wl_text_fill else R.layout.asuka_wl_text
        "symbol" -> if (fill) R.layout.asuka_wl_symbol_fill else R.layout.asuka_wl_symbol
        "image" -> when (mode) {
            "fit" -> if (fill) R.layout.asuka_wl_image_fit_fill else R.layout.asuka_wl_image_fit
            "stretch" -> if (fill) R.layout.asuka_wl_image_stretch_fill else R.layout.asuka_wl_image_stretch
            "center" -> if (fill) R.layout.asuka_wl_image_center_fill else R.layout.asuka_wl_image_center
            else -> if (fill) R.layout.asuka_wl_image_fill_fill else R.layout.asuka_wl_image_fill
        }
        "progress" -> if (fill) R.layout.asuka_wl_progress_fill else R.layout.asuka_wl_progress
        "gauge" -> if (fill) R.layout.asuka_wl_gauge_fill else R.layout.asuka_wl_gauge
        "timer" -> if (fill) R.layout.asuka_wl_timer_fill else R.layout.asuka_wl_timer
        "divider" -> if (fill) R.layout.asuka_wl_divider_fill else R.layout.asuka_wl_divider
        "param" -> if (fill) R.layout.asuka_wl_text_fill else R.layout.asuka_wl_text
        "button" -> if (fill) R.layout.asuka_wl_button_fill else R.layout.asuka_wl_button
        "toggle" -> if (fill) R.layout.asuka_wl_toggle_fill else R.layout.asuka_wl_toggle
        "spacer" -> R.layout.asuka_wl_spacer_fill
        else -> R.layout.asuka_wl_spacer_fill
    }

    /** A color on the wire: a semantic one's resource, or "#rrggbb(aa)". */
    private sealed class Paint {
        class Res(val id: Int) : Paint()
        class Argb(val c: Int) : Paint()
    }

    private fun paint(s: String): Paint? = when (s) {
        "" -> null
        ".label" -> Paint.Res(R.color.asuka_widget_on_surface)
        ".secondaryLabel" -> Paint.Res(R.color.asuka_widget_on_surface_variant)
        ".tertiaryLabel" -> Paint.Res(R.color.asuka_widget_outline)
        ".background" -> Paint.Res(R.color.asuka_widget_surface)
        ".secondaryBackground", ".groupedBackground", ".secondaryGroupedBackground" -> Paint.Res(R.color.asuka_widget_surface_container)
        ".separator" -> Paint.Res(R.color.asuka_widget_outline_variant)
        ".accent" -> Paint.Res(R.color.asuka_widget_accent)
        ".onAccent" -> Paint.Res(R.color.asuka_widget_on_accent)
        ".clear" -> Paint.Argb(Color.TRANSPARENT)
        // The hues, as the host draws them.
        ".red" -> Paint.Argb(0xFFD32F2F.toInt())
        ".orange" -> Paint.Argb(0xFFF57C00.toInt())
        ".yellow" -> Paint.Argb(0xFFFBC02D.toInt())
        ".green" -> Paint.Argb(0xFF388E3C.toInt())
        ".blue" -> Paint.Argb(0xFF1976D2.toInt())
        ".purple" -> Paint.Argb(0xFF7B1FA2.toInt())
        ".pink" -> Paint.Argb(0xFFC2185B.toInt())
        ".gray" -> Paint.Argb(0xFF757575.toInt())
        else -> hex(s)?.let { Paint.Argb(it) }
    }

    /** "#rrggbb" or "#rrggbbaa" as an ARGB int. */
    private fun hex(s: String): Int? {
        val h = s.removePrefix("#")
        val v = h.toLongOrNull(16) ?: return null
        return when (h.length) {
            6 -> (0xFF000000 or v).toInt()
            8 -> ((v and 0xff) shl 24 or (v ushr 8)).toInt()
            else -> null
        }
    }

    private fun RemoteViews.color(ctx: Context, id: Int, method: String, p: Paint) {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            when (p) {
                is Paint.Res -> setColorStateList(id, method, p.id)
                is Paint.Argb -> setColorStateList(id, method, ColorStateList.valueOf(p.c))
            }
            return
        }
        // Before Android 12 there are no color state lists: the color, as
        // it resolves for the configuration now, through the int setters;
        // a bar's tint has none (it's the theme's accent).
        val c = when (p) {
            is Paint.Res -> ctx.getColor(p.id)
            is Paint.Argb -> p.c
        }
        when (method) {
            "setTextColor" -> setTextColor(id, c)
            "setImageTintList" -> setInt(id, "setColorFilter", c)
            "setBackgroundTintList" -> setInt(id, "setBackgroundColor", c)
        }
    }

    private fun dp(ctx: Context, v: Double) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), ctx.resources.displayMetrics).toInt()

    /** A text style's size (px, the user's font scale in it) and its weight, from its Material text appearance. */
    // Its attributes are read by their index in the array asked for, not
    // a declared styleable's.
    @android.annotation.SuppressLint("ResourceType")
    private fun textStyle(ctx: Context, style: String): Pair<Float, Boolean> {
        val s = when (style) {
            "largeTitle" -> AsukaTextStyle.LARGE_TITLE
            "title" -> AsukaTextStyle.TITLE
            "title2" -> AsukaTextStyle.TITLE2
            "title3" -> AsukaTextStyle.TITLE3
            "headline" -> AsukaTextStyle.HEADLINE
            "subheadline" -> AsukaTextStyle.SUBHEADLINE
            "callout" -> AsukaTextStyle.CALLOUT
            "footnote" -> AsukaTextStyle.FOOTNOTE
            "caption" -> AsukaTextStyle.CAPTION
            "caption2" -> AsukaTextStyle.CAPTION2
            else -> AsukaTextStyle.BODY
        }
        val res = textAppearance(s) ?: return Pair(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, 16f, ctx.resources.displayMetrics), false)
        val a = ctx.obtainStyledAttributes(res, intArrayOf(android.R.attr.textSize, android.R.attr.fontFamily))
        val size = a.getDimension(0, 16f)
        val family = a.getString(1) ?: ""
        a.recycle()
        // Material's titles and labels are medium.
        return Pair(size, family.contains("medium") || s == AsukaTextStyle.HEADLINE || s == AsukaTextStyle.SUBHEADLINE ||
            s == AsukaTextStyle.CAPTION || s == AsukaTextStyle.CAPTION2)
    }

    /** text with its weight: bold's span, or the family that draws it. */
    private fun weighted(text: String, weight: String, medium: Boolean): CharSequence {
        val sp = SpannableString(text)
        val span: Any? = when (weight) {
            "semibold", "bold", "heavy", "black" -> StyleSpan(Typeface.BOLD)
            "medium" -> TypefaceSpan("sans-serif-medium")
            "light", "thin", "ultralight" -> TypefaceSpan("sans-serif-light")
            "regular" -> null
            else -> if (medium) TypefaceSpan("sans-serif-medium") else null
        }
        if (span != null) sp.setSpan(span, 0, sp.length, Spanned.SPAN_INCLUSIVE_EXCLUSIVE)
        return sp
    }

    /** Where the widget's images are (the host's "image" keeps them). */
    fun imageFile(ctx: Context, name: String): File =
        File(File(ctx.filesDir, "asuka_widget_images"), name.map { if (it.isLetterOrDigit() || it == '-' || it == '_') it else '_' }.joinToString("") + ".img")

    private fun bitmap(ctx: Context, name: String): Bitmap? = runCatching { BitmapFactory.decodeFile(imageFile(ctx, name).path) }.getOrNull()

    /**
     * The entry's layout as RemoteViews: the root, filling the widget, on
     * the widget's background (its own, when it has one), opening url
     * where no part of it has a link of its own.
     */
    fun render(ctx: Context, root: JSONObject, scope: AsukaWidgetScope, open: (String) -> PendingIntent): RemoteViews {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.S) return legacyRender(ctx, root, scope, open)
        val bg = root.optString("bg")
        val v = node(ctx, root, axis = null, stretch = false, open = open, scope = scope, isRoot = true)
        // A widget's padding (Android's design guidance: 16dp), unless it has its own.
        if (listOf("pt", "pb", "ps", "pe").all { root.optDouble(it, 0.0) <= 0 }) {
            val p = dp(ctx, 16.0)
            v.setViewPadding(ID, p, p, p, p)
        }
        v.setInt(ID, "setBackgroundResource", R.drawable.asuka_widget_background)
        paint(bg)?.let { v.color(ctx, ID, "setBackgroundTintList", it) }
        v.setViewLayoutWidth(ID, ViewGroup.LayoutParams.MATCH_PARENT.toFloat(), TypedValue.COMPLEX_UNIT_PX)
        v.setViewLayoutHeight(ID, ViewGroup.LayoutParams.MATCH_PARENT.toFloat(), TypedValue.COMPLEX_UNIT_PX)
        return v
    }

    /**
     * Before Android 12 a view's size can't be set: the root goes in a
     * cell, which fills the widget (asuka_wl_cell) and has the widget's
     * background and padding, and places the root where its own
     * alignment would place what it holds.
     */
    private fun legacyRender(ctx: Context, root: JSONObject, scope: AsukaWidgetScope, open: (String) -> PendingIntent): RemoteViews {
        val cell = RemoteViews(ctx.packageName, R.layout.asuka_wl_cell)
        if (listOf("pt", "pb", "ps", "pe").all { root.optDouble(it, 0.0) <= 0 }) {
            val p = dp(ctx, 16.0)
            cell.setViewPadding(ID, p, p, p, p)
        }
        cell.setInt(ID, "setBackgroundResource", R.drawable.asuka_widget_background)
        paint(root.optString("bg"))?.let { cell.color(ctx, ID, "setBackgroundTintList", it) }
        val a = root.optString("align")
        cell.setInt(ID, "setGravity", when (root.optString("t")) {
            "column" -> Gravity.TOP or when (a) { "center" -> Gravity.CENTER_HORIZONTAL; "end" -> Gravity.END; else -> Gravity.START }
            "row" -> Gravity.START or when (a) { "start" -> Gravity.TOP; "end" -> Gravity.BOTTOM; else -> Gravity.CENTER_VERTICAL }
            "layers" -> when (a) { "start" -> Gravity.START; "end" -> Gravity.END; else -> Gravity.CENTER_HORIZONTAL } or
                when (root.optString("valign")) { "start" -> Gravity.TOP; "end" -> Gravity.BOTTOM; else -> Gravity.CENTER_VERTICAL }
            else -> Gravity.TOP or Gravity.START
        })
        cell.removeAllViews(ID)
        cell.addView(ID, node(ctx, root, axis = null, stretch = false, open = open, scope = scope, isRoot = true))
        return cell
    }

    /** axis: its parent's ('v' a column, 'h' a row, null layers or the root). */
    private fun node(ctx: Context, n: JSONObject, axis: Char?, stretch: Boolean, open: (String) -> PendingIntent, scope: AsukaWidgetScope, isRoot: Boolean = false): RemoteViews {
        val t = n.optString("t")
        val fill = n.optBoolean("fill") && axis != null
        val v = RemoteViews(ctx.packageName, layoutOf(t, n.optString("mode"), fill || t == "spacer" && axis != null))
        val color = paint(n.optString("color"))
        when (t) {
            "column", "row" -> {
                val col = t == "column"
                val a = n.optString("align")
                v.setInt(ID, "setGravity", if (col) when (a) { "center" -> Gravity.CENTER_HORIZONTAL; "end" -> Gravity.END; else -> Gravity.START }
                    else when (a) { "start" -> Gravity.TOP; "end" -> Gravity.BOTTOM; else -> Gravity.CENTER_VERTICAL })
                val kids = n.optJSONArray("c")
                val spacing = n.optDouble("spacing", 0.0)
                // A launcher reapplies the views as the widget resizes: its
                // children added again, unless they're cleared first.
                v.removeAllViews(ID)
                for (i in 0 until (kids?.length() ?: 0)) {
                    val c = kids!!.getJSONObject(i)
                    val cv = node(ctx, c, if (col) 'v' else 'h', col && a == "stretch", open, scope)
                    // Before Android 12 a margin can't be set: no spacing.
                    if (i > 0 && spacing > 0 && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                        cv.setViewLayoutMargin(ID, if (col) RemoteViews.MARGIN_TOP else RemoteViews.MARGIN_START, spacing.toFloat(), TypedValue.COMPLEX_UNIT_DIP)
                    }
                    v.addView(ID, cv)
                }
            }
            "layers" -> {
                val g = when (n.optString("align")) { "start" -> Gravity.START; "end" -> Gravity.END; else -> Gravity.CENTER_HORIZONTAL } or
                    when (n.optString("valign")) { "start" -> Gravity.TOP; "end" -> Gravity.BOTTOM; else -> Gravity.CENTER_VERTICAL }
                val kids = n.optJSONArray("c")
                v.removeAllViews(ID)
                for (i in 0 until (kids?.length() ?: 0)) {
                    val cell = RemoteViews(ctx.packageName, R.layout.asuka_wl_cell)
                    cell.setInt(ID, "setGravity", g)
                    cell.removeAllViews(ID)
                    cell.addView(ID, node(ctx, kids!!.getJSONObject(i), null, false, open, scope))
                    v.addView(ID, cell)
                }
            }
            "text", "timer", "param" -> {
                val (size, medium) = textStyle(ctx, n.optString("style"))
                if (n.optDouble("size", 0.0) > 0) v.setTextViewTextSize(ID, TypedValue.COMPLEX_UNIT_SP, n.optDouble("size").toFloat())
                else v.setTextViewTextSize(ID, TypedValue.COMPLEX_UNIT_PX, size)
                v.color(ctx, ID, "setTextColor", color ?: Paint.Res(R.color.asuka_widget_on_surface))
                if (t == "text" || t == "param") {
                    val text = if (t == "param") scope.titles[n.optString("name")].orEmpty() else n.optString("text")
                    v.setTextViewText(ID, weighted(text, n.optString("weight"), medium))
                    val lines = n.optInt("lines")
                    if (lines > 0) v.setInt(ID, "setMaxLines", lines)
                    v.setInt(ID, "setGravity", when (n.optString("textAlign")) { "center" -> Gravity.CENTER_HORIZONTAL; "end" -> Gravity.END; else -> Gravity.START })
                } else {
                    // Down to it, then up from it.
                    val left = n.optLong("untilMs") - System.currentTimeMillis()
                    v.setChronometer(ID, SystemClock.elapsedRealtime() + left, null, true)
                    v.setChronometerCountDown(ID, left > 0)
                }
            }
            "symbol" -> {
                val res = symbolDrawables[n.optString("name")] ?: symbolDrawables[n.optString("name").removeSuffix(".fill")]
                if (res != null) v.setImageViewResource(ID, res)
                v.color(ctx, ID, "setImageTintList", color ?: Paint.Res(R.color.asuka_widget_on_surface))
                // Its size: as tall as the body's text, by default (Material's 24dp beside 16sp).
                // Before Android 12, the drawable's own.
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    val px = if (n.optDouble("size", 0.0) > 0) dp(ctx, n.optDouble("size")) else dp(ctx, 20.0)
                    v.setViewLayoutWidth(ID, px.toFloat(), TypedValue.COMPLEX_UNIT_PX)
                    v.setViewLayoutHeight(ID, px.toFloat(), TypedValue.COMPLEX_UNIT_PX)
                }
            }
            "image" -> bitmap(ctx, n.optString("name"))?.let { v.setImageViewBitmap(ID, it) }
            "progress" -> {
                v.setProgressBar(ID, 1000, (n.optDouble("value", 0.0).coerceIn(0.0, 1.0) * 1000).toInt(), false)
                v.color(ctx, ID, "setProgressTintList", color ?: Paint.Res(R.color.asuka_widget_accent))
                if (axis != 'h' && Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    v.setViewLayoutWidth(ID, ViewGroup.LayoutParams.MATCH_PARENT.toFloat(), TypedValue.COMPLEX_UNIT_PX)
                }
            }
            "gauge" -> {
                v.setProgressBar(R.id.asuka_wl_ring, 1000, (n.optDouble("value", 0.0).coerceIn(0.0, 1.0) * 1000).toInt(), false)
                v.color(ctx, R.id.asuka_wl_ring, "setProgressTintList", color ?: Paint.Res(R.color.asuka_widget_accent))
                v.setTextViewText(R.id.asuka_wl_label, n.optString("text"))
            }
            "divider" -> {
                // The drawable first: before Android 12 the color replaces it.
                v.setInt(ID, "setBackgroundResource", R.drawable.asuka_wl_fill)
                v.color(ctx, ID, "setBackgroundTintList", Paint.Res(R.color.asuka_widget_outline_variant))
                val match = ViewGroup.LayoutParams.MATCH_PARENT.toFloat()
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    if (axis == 'h') v.setViewLayoutHeight(ID, match, TypedValue.COMPLEX_UNIT_PX) else v.setViewLayoutWidth(ID, match, TypedValue.COMPLEX_UNIT_PX)
                }
            }
            "button" -> {
                v.setTextViewText(R.id.asuka_wl_label, n.optString("text"))
                val res = symbolDrawables[n.optString("name")] ?: symbolDrawables[n.optString("name").removeSuffix(".fill")]
                if (res != null) {
                    v.setViewVisibility(R.id.asuka_wl_icon, View.VISIBLE)
                    v.setImageViewResource(R.id.asuka_wl_icon, res)
                    v.setContentDescription(R.id.asuka_wl_icon, null)
                }
                color?.let {
                    v.color(ctx, R.id.asuka_wl_label, "setTextColor", it)
                    if (res != null) v.color(ctx, R.id.asuka_wl_icon, "setImageTintList", it)
                }
                v.setOnClickPendingIntent(ID, scope.action(ctx, n.optString("action"), n.optString("arg"), toggle = false))
            }
            "toggle" -> {
                val action = n.optString("action")
                val arg = n.optString("arg")
                val on = AsukaWidgets.toggle(ctx, scope.toggleKey(action, arg)) ?: n.optBoolean("on")
                val pi = scope.action(ctx, action, arg, toggle = true)
                if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
                    // The platform's switch: it flips as it's tapped, and the
                    // system tells the receiver the state the user set.
                    v.setTextViewText(ID, n.optString("text"))
                    v.setCompoundButtonChecked(ID, on)
                    v.setOnCheckedChangeResponse(ID, RemoteViews.RemoteResponse.fromPendingIntent(pi))
                    color?.let { v.color(ctx, ID, "setThumbTintList", it) }
                } else {
                    v.setTextViewText(R.id.asuka_wl_label, n.optString("text"))
                    v.setImageViewResource(R.id.asuka_wl_switch, if (on) R.drawable.asuka_wl_switch_on else R.drawable.asuka_wl_switch_off)
                    v.setOnClickPendingIntent(ID, pi)
                }
            }
            "spacer" -> {
                val min = dp(ctx, n.optDouble("size", 0.0))
                if (min > 0) v.setInt(ID, if (axis == 'h') "setMinimumWidth" else "setMinimumHeight", min)
            }
        }
        modifiers(ctx, v, n, axis, stretch, open, scope, isRoot)
        return v
    }

    private fun modifiers(ctx: Context, v: RemoteViews, n: JSONObject, axis: Char?, stretch: Boolean, open: (String) -> PendingIntent, scope: AsukaWidgetScope, isRoot: Boolean) {
        val rtl = ctx.resources.configuration.layoutDirection == View.LAYOUT_DIRECTION_RTL
        val (pt, pb, ps, pe) = listOf("pt", "pb", "ps", "pe").map { dp(ctx, n.optDouble(it, 0.0)) }
        if (pt > 0 || pb > 0 || ps > 0 || pe > 0) v.setViewPadding(ID, if (rtl) pe else ps, pt, if (rtl) ps else pe, pb)
        if (!isRoot) paint(n.optString("bg"))?.let {
            v.setInt(ID, "setBackgroundResource", R.drawable.asuka_wl_fill)
            v.color(ctx, ID, "setBackgroundTintList", it)
        }
        // Before Android 12 a view's corners and frame can't be set: its
        // layout's own (wrapping what it holds), square.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
            val radius = n.optDouble("radius", 0.0)
            if (radius > 0) v.setViewOutlinePreferredRadius(ID, radius.toFloat(), TypedValue.COMPLEX_UNIT_DIP)
            val match = ViewGroup.LayoutParams.MATCH_PARENT.toFloat()
            // Its frame; a layer or the root that fills, both ways; a column's
            // child that stretches, across it.
            if (n.optDouble("w", 0.0) > 0) v.setViewLayoutWidth(ID, n.optDouble("w").toFloat(), TypedValue.COMPLEX_UNIT_DIP)
            else if (stretch || n.optBoolean("fill") && axis == null) v.setViewLayoutWidth(ID, match, TypedValue.COMPLEX_UNIT_PX)
            if (n.optDouble("h", 0.0) > 0) v.setViewLayoutHeight(ID, n.optDouble("h").toFloat(), TypedValue.COMPLEX_UNIT_DIP)
            else if (n.optBoolean("fill") && axis == null) v.setViewLayoutHeight(ID, match, TypedValue.COMPLEX_UNIT_PX)
        }
        val opacity = n.optDouble("opacity", 1.0)
        if (opacity < 1) v.setFloat(ID, "setAlpha", opacity.toFloat())
        val url = n.optString("url")
        val t = n.optString("t")
        if (n.optString("action").isNotEmpty() && t != "button" && t != "toggle") {
            // An action part: tapping it runs the action, without opening the app.
            v.setOnClickPendingIntent(ID, scope.action(ctx, n.optString("action"), n.optString("arg"), toggle = false))
        } else if (url.isNotEmpty() && t != "button" && t != "toggle") v.setOnClickPendingIntent(ID, open(url))
    }
}

/** Opens the app with url (ui.OnOpenURL), or as it launches ("": none). */
internal fun widgetOpen(ctx: Context, key: String, url: String): PendingIntent {
    val i = Intent(ctx, AsukaActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_SINGLE_TOP)
    if (url.isNotEmpty()) i.setAction(Intent.ACTION_VIEW).setData(Uri.parse(url)) else i.setAction(Intent.ACTION_MAIN)
    return PendingIntent.getActivity(ctx, (key + url).hashCode(), i, PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT)
}
