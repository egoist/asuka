package dev.asuka

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Paint
import android.text.Layout
import android.text.SpannableString
import android.text.Spanned
import android.text.StaticLayout
import android.text.TextPaint
import android.text.TextUtils
import android.text.style.LineHeightSpan
import android.util.TypedValue
import android.view.View
import android.widget.TextView
import java.nio.ByteBuffer

/**
 * LineHeight: each line this many px from baseline to baseline, the text
 * in the middle of its line's height (CSS's half-leading, React Native's
 * lineHeight), the first's and last's too: no font padding.
 */
class AsukaLineHeightSpan(var px: Int = 0) : LineHeightSpan {
    override fun chooseHeight(text: CharSequence, start: Int, end: Int, spanstartv: Int, lineHeight: Int, fm: Paint.FontMetricsInt) {
        if (px <= 0) return
        val extra = px - (fm.descent - fm.ascent)
        val below = Math.floorDiv(extra, 2)
        fm.ascent -= extra - below
        fm.descent += below
        fm.top = fm.ascent
        fm.bottom = fm.descent
    }
}

/**
 * A Text's view: a TextView that also shrinks its text to fit its width
 * (MinimumScale), finding the scale as it's measured, the same way
 * whether the core measures it or its frame is applied, and keeps its
 * line height (LineHeight) through a span.
 */
// The platform's TextView, as a Text's was: AppCompat's posts a message to
// load the theme's font for each one made, and a Text needs nothing else
// of it.
@SuppressLint("AppCompatCustomView")
class AsukaText(ctx: Context) : TextView(ctx) {
    /** The text size the style sets, px, before it's shrunk. */
    var baseSize = 0f
    /** The line height, px at its own size (0: the font's). */
    var lineHeightPx = 0f
    /** MinimumScale: 0 (or 1 and over) doesn't shrink. */
    var minScale = 0f
    /** LineLimit (0: none). */
    var lineLimit = 0
    /** The scale the text is drawn at. */
    var scale = 1f
        private set
    private var raw: CharSequence = ""
    private val lineSpan = AsukaLineHeightSpan()
    private val work = TextPaint()

    val shrinks: Boolean get() = minScale > 0f && minScale < 1f

    /** The Text's text. */
    fun setContent(s: String) {
        raw = s
        refreshText()
    }

    private fun refreshText() {
        lineSpan.px = Math.round(lineHeightPx * scale)
        if (lineHeightPx > 0f && raw.isNotEmpty()) {
            val sp = SpannableString(raw)
            sp.setSpan(lineSpan, 0, raw.length, Spanned.SPAN_INCLUSIVE_INCLUSIVE)
            text = sp
        } else {
            text = raw
        }
    }

    /** Its style changed (applyTextStyle set baseSize's size): drawn at its own size until it's measured. */
    fun restyled() {
        scale = 1f
        refreshText()
        requestLayout()
    }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val want = if (shrinks) fitScale(widthMeasureSpec) else 1f
        if (want != scale) {
            scale = want
            lineSpan.px = Math.round(lineHeightPx * want)
            super.setTextSize(TypedValue.COMPLEX_UNIT_PX, baseSize * want) // lays the text out again
        }
        super.onMeasure(widthMeasureSpec, heightMeasureSpec)
    }

    /**
     * The largest scale, from 1 down to minScale, at which the text fits its
     * lines (its limit, else one) in the width the spec gives its content.
     */
    private fun fitScale(spec: Int): Float {
        if (View.MeasureSpec.getMode(spec) == View.MeasureSpec.UNSPECIFIED || raw.isEmpty() || baseSize <= 0f) return 1f
        val width = View.MeasureSpec.getSize(spec) - compoundPaddingLeft - compoundPaddingRight
        if (width <= 0) return minScale
        val lines = if (lineLimit > 0) lineLimit else 1
        work.set(paint)
        return fitTextScale(minScale) { s ->
            work.textSize = baseSize * s
            if (lines == 1) Layout.getDesiredWidth(raw, work) <= width + 0.5f
            else StaticLayout.Builder.obtain(raw, 0, raw.length, work, width).setIncludePad(false).build().lineCount <= lines
        }
    }
}

/**
 * The largest scale in [min, 1] at which fits holds (a binary search, to
 * a 128th of the range); min when it never does.
 */
fun fitTextScale(min: Float, fits: (Float) -> Boolean): Float {
    if (fits(1f)) return 1f
    if (!fits(min)) return min
    var lo = min
    var hi = 1f
    repeat(7) {
        val mid = (lo + hi) / 2
        if (fits(mid)) lo = mid else hi = mid
    }
    return lo
}

/** LetterSpacing as Android takes it: ems of the text size (px). */
fun AsukaRuntime.letterSpacingEm(points: Float, sizePx: Float): Float =
    if (sizePx <= 0f) 0f else TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, points, themed.resources.displayMetrics) / sizePx

/**
 * A text's letter spacing and line height, over its style's (after
 * applyTextStyle, which sets the appearance's), scaled with the font.
 */
fun AsukaRuntime.applyTextSpacing(n: AsukaNode, tv: TextView) {
    val t = tv as? AsukaText
    t?.baseSize = tv.textSize
    if (n.letterSpacing != 0f) tv.letterSpacing = letterSpacingEm(n.letterSpacing, t?.baseSize ?: tv.textSize)
    if (t == null) return
    t.lineHeightPx = if (n.lineHeight > 0f) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_SP, n.lineHeight, themed.resources.displayMetrics) else 0f
    if (t.lineHeightPx > 0f) t.setLineSpacing(0f, 1f) // the span's alone
    t.restyled()
}

/**
 * LineLimit and Truncation (and MinimumScale's one line, when there's no
 * limit): where the text is cut. A TextView ellipsizes at the start or
 * the middle on one line only: on more, at the end. Clip on one line
 * cuts it off at its edge, not at a word.
 */
fun applyLineLimit(n: AsukaNode, tv: TextView) {
    val limit = (n.props[AsukaProp.LINE_LIMIT] as? Long)?.toInt() ?: 0
    val t = tv as? AsukaText
    if (t == null) {
        tv.maxLines = if (limit > 0) limit else Int.MAX_VALUE
        tv.ellipsize = if (limit > 0) TextUtils.TruncateAt.END else null
        return
    }
    t.lineLimit = limit
    val lines = if (limit > 0) limit else if (t.shrinks) 1 else 0
    val trunc = n.props[AsukaProp.TRUNCATION] as? Int ?: AsukaTruncation.TAIL
    t.maxLines = if (lines > 0) lines else Int.MAX_VALUE
    t.setHorizontallyScrolling(lines == 1 && trunc == AsukaTruncation.CLIP)
    t.ellipsize = if (lines <= 0) null else when (trunc) {
        AsukaTruncation.CLIP -> null
        AsukaTruncation.HEAD -> if (lines == 1) TextUtils.TruncateAt.START else TextUtils.TruncateAt.END
        AsukaTruncation.MIDDLE -> if (lines == 1) TextUtils.TruncateAt.MIDDLE else TextUtils.TruncateAt.END
        else -> TextUtils.TruncateAt.END
    }
    t.requestLayout()
}

/**
 * TextShadow: a shadow under the glyphs (u64 color, 0: none; f32 blur
 * radius, x, y in points). Its blur is a Gaussian of σ half the radius,
 * as CSS's; Android's shadow layer takes a radius Skia turns into σ =
 * 0.57735 r + 0.5.
 */
fun AsukaRuntime.setTextShadow(n: AsukaNode, b: ByteBuffer) {
    val tv = n.view as? TextView ?: return
    if (b.remaining() < 20) return
    val c = b.long
    val radius = b.float
    val x = b.float
    val y = b.float
    if (c == 0L) {
        tv.setShadowLayer(0f, 0f, 0f, 0)
        return
    }
    val d = density
    val sigma = radius / 2 * d
    tv.setShadowLayer(maxOf((sigma - 0.5f) / 0.57735f, 0.01f), x * d, y * d, color(c))
}
