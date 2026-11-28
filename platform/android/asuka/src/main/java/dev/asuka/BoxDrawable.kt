package dev.asuka

import android.graphics.Canvas
import android.graphics.ColorFilter
import android.graphics.DashPathEffect
import android.graphics.LinearGradient
import android.graphics.Matrix
import android.graphics.Outline
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PathEffect
import android.graphics.PixelFormat
import android.graphics.RadialGradient
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.SweepGradient
import android.graphics.drawable.Drawable
import android.os.Build

/**
 * A view's background when a GradientDrawable can't draw it: radii that
 * differ per corner, borders that differ per side or are dashed or
 * dotted, and gradients (linear at any angle, radial, conic) over the
 * background color. Radii are clamped as CSS's (all scaled down by one
 * factor until adjacent ones fit their side); a border is drawn inside
 * the box, its inner corners the outer ones less the sides' widths.
 * Its paths and shader are made again only when its bounds or values
 * change.
 */
class AsukaBoxDrawable : Drawable() {
    private var fill = 0
    private var gradient: AsukaGradient? = null
    private val radii = FloatArray(4) // px: tl, tr, br, bl
    private val widths = FloatArray(4) // px: top, right, bottom, left
    private var borderColor = 0
    private var borderStyle = AsukaBorderStyle.SOLID

    private var dirty = true
    private val r = FloatArray(4) // the radii, clamped
    private val r8 = FloatArray(8)
    private val in8 = FloatArray(8)
    private val rect = RectF()
    private val path = Path()
    private val border = Path()
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val shaderPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val borderPaint = Paint(Paint.ANTI_ALIAS_FLAG)
    private val sideEffects = arrayOfNulls<PathEffect>(4)

    /** Sets what it draws (px); invalidates only when something changed. */
    fun set(fill: Int, gradient: AsukaGradient?, radii: FloatArray, widths: FloatArray, borderColor: Int, borderStyle: Int) {
        if (fill == this.fill && gradient === this.gradient && radii.contentEquals(this.radii) && widths.contentEquals(this.widths) &&
            borderColor == this.borderColor && borderStyle == this.borderStyle) return
        this.fill = fill
        this.gradient = gradient
        radii.copyInto(this.radii)
        widths.copyInto(this.widths)
        this.borderColor = borderColor
        this.borderStyle = borderStyle
        dirty = true
        invalidateSelf()
    }

    override fun onBoundsChange(bounds: Rect) {
        super.onBoundsChange(bounds)
        dirty = true
    }

    private fun rebuild() {
        dirty = false
        val b = bounds
        val w = b.width().toFloat()
        val h = b.height().toFloat()
        clampRadii(radii, w, h, r)
        for (i in 0..3) { r8[2 * i] = r[i]; r8[2 * i + 1] = r[i] }
        rect.set(b)
        path.reset()
        path.addRoundRect(rect, r8, Path.Direction.CW)

        val g = gradient
        shaderPaint.shader = if (g == null || w <= 0f || h <= 0f) null else {
            val x0 = b.left.toFloat()
            val y0 = b.top.toFloat()
            val cx = x0 + g.cx * w
            val cy = y0 + g.cy * h
            when (g.kind) {
                AsukaGradientKind.RADIAL -> RadialGradient(cx, cy, maxOf(g.radius * minOf(w, h), 0.01f), g.colors, g.offsets, Shader.TileMode.CLAMP)
                AsukaGradientKind.CONIC -> SweepGradient(cx, cy, g.colors, g.offsets).apply {
                    // A sweep starts at 3 o'clock; a conic gradient at its angle from the top.
                    setLocalMatrix(Matrix().apply { setRotate(g.angle - 90f, cx, cy) })
                }
                else -> LinearGradient(x0 + g.sx * w, y0 + g.sy * h, x0 + g.ex * w, y0 + g.ey * h, g.colors, g.offsets, Shader.TileMode.CLAMP)
            }
        }

        border.reset()
        sideEffects.fill(null)
        val (t, rt, bt, l) = widths
        if (t <= 0f && rt <= 0f && bt <= 0f && l <= 0f) return
        val uniform = t == rt && rt == bt && bt == l
        if (borderStyle == AsukaBorderStyle.SOLID) {
            // The ring between the box and its inside: each inner corner's
            // radius the outer one's less the widths of its sides.
            border.fillType = Path.FillType.EVEN_ODD
            border.addRoundRect(rect, r8, Path.Direction.CW)
            val inner = RectF(rect.left + l, rect.top + t, rect.right - rt, rect.bottom - bt)
            if (inner.width() > 0f && inner.height() > 0f) {
                in8[0] = maxOf(r[0] - l, 0f); in8[1] = maxOf(r[0] - t, 0f)
                in8[2] = maxOf(r[1] - rt, 0f); in8[3] = maxOf(r[1] - t, 0f)
                in8[4] = maxOf(r[2] - rt, 0f); in8[5] = maxOf(r[2] - bt, 0f)
                in8[6] = maxOf(r[3] - l, 0f); in8[7] = maxOf(r[3] - bt, 0f)
                border.addRoundRect(inner, in8, Path.Direction.CCW)
            }
            borderPaint.style = Paint.Style.FILL
            borderPaint.pathEffect = null
        } else if (uniform) {
            // The line along the middle of the border, around its corners.
            val half = t / 2
            val mid = RectF(rect.left + half, rect.top + half, rect.right - half, rect.bottom - half)
            for (i in 0..3) { in8[2 * i] = maxOf(r[i] - half, 0f); in8[2 * i + 1] = in8[2 * i] }
            border.addRoundRect(mid, in8, Path.Direction.CW)
            borderPaint.style = Paint.Style.STROKE
            borderPaint.strokeWidth = t
            borderPaint.strokeCap = if (borderStyle == AsukaBorderStyle.DOTTED) Paint.Cap.ROUND else Paint.Cap.BUTT
            borderPaint.pathEffect = dash(t)
        } else {
            // Sides of their own widths: each a line along its middle.
            for (i in 0..3) if (widths[i] > 0f) sideEffects[i] = dash(widths[i])
        }
    }

    /** Dashes three widths long, gaps as long; or round dots a width wide, a width apart. */
    private fun dash(w: Float): PathEffect =
        if (borderStyle == AsukaBorderStyle.DOTTED) DashPathEffect(floatArrayOf(0.001f, 2 * w), 0f)
        else DashPathEffect(floatArrayOf(3 * w, 3 * w), 0f)

    override fun draw(c: Canvas) {
        if (dirty) rebuild()
        if (bounds.isEmpty) return
        if (fill != 0) {
            fillPaint.color = fill
            c.drawPath(path, fillPaint)
        }
        if (shaderPaint.shader != null) c.drawPath(path, shaderPaint)
        borderPaint.color = borderColor
        if (!border.isEmpty) {
            c.drawPath(border, borderPaint)
            return
        }
        if (sideEffects.all { it == null }) return
        borderPaint.style = Paint.Style.STROKE
        borderPaint.strokeCap = if (borderStyle == AsukaBorderStyle.DOTTED) Paint.Cap.ROUND else Paint.Cap.BUTT
        val (t, rt, bt, l) = widths
        val x0 = rect.left; val y0 = rect.top; val x1 = rect.right; val y1 = rect.bottom
        fun side(i: Int, w: Float, ax: Float, ay: Float, bx: Float, by: Float) {
            val e = sideEffects[i] ?: return
            borderPaint.strokeWidth = w
            borderPaint.pathEffect = e
            c.drawLine(ax, ay, bx, by, borderPaint)
        }
        side(0, t, x0, y0 + t / 2, x1, y0 + t / 2)
        side(1, rt, x1 - rt / 2, y0, x1 - rt / 2, y1)
        side(2, bt, x1, y1 - bt / 2, x0, y1 - bt / 2)
        side(3, l, x0 + l / 2, y1, x0 + l / 2, y0)
    }

    /** Its shape, for clipping and shadows: a rounded rectangle, or (radii differing per corner) its path. */
    override fun getOutline(o: Outline) {
        if (dirty) rebuild()
        if (r[0] == r[1] && r[1] == r[2] && r[2] == r[3]) {
            o.setRoundRect(bounds, r[0])
        } else if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            o.setPath(path)
        } else {
            @Suppress("DEPRECATION")
            o.setConvexPath(path)
        }
        o.alpha = 1f
    }

    override fun setAlpha(alpha: Int) {
        fillPaint.alpha = alpha
        shaderPaint.alpha = alpha
        borderPaint.alpha = alpha
        invalidateSelf()
    }

    override fun setColorFilter(colorFilter: ColorFilter?) {
        fillPaint.colorFilter = colorFilter
        shaderPaint.colorFilter = colorFilter
        borderPaint.colorFilter = colorFilter
        invalidateSelf()
    }

    @Deprecated("Deprecated in Java")
    override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
}

/**
 * Corner radii (tl, tr, br, bl) on a w by h box, as CSS clamps them into
 * out: when two on a side are more than it, all are scaled down by the
 * same factor, the least that makes every side's fit.
 */
fun clampRadii(radii: FloatArray, w: Float, h: Float, out: FloatArray) {
    var f = 1f
    fun fit(side: Float, a: Float, c: Float) { if (a + c > side && a + c > 0f) f = minOf(f, maxOf(side, 0f) / (a + c)) }
    val r0 = maxOf(radii[0], 0f); val r1 = maxOf(radii[1], 0f); val r2 = maxOf(radii[2], 0f); val r3 = maxOf(radii[3], 0f)
    fit(w, r0, r1); fit(h, r1, r2); fit(w, r2, r3); fit(h, r3, r0)
    out[0] = r0 * f; out[1] = r1 * f; out[2] = r2 * f; out[3] = r3 * f
}
