// platform/car's maneuvers on Android Auto: each Turn's Car App Library
// maneuver type, and its arrow, drawn here (the Material symbols the host
// has don't draw turns): white on clear, as the car tints and places it,
// kept once drawn. Left and right follow the side the country drives on
// for U-turns and roundabouts.
package platform.car

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import androidx.car.app.navigation.model.Maneuver as CarManeuver
import kotlin.math.cos
import kotlin.math.sin

internal object Maneuvers {
    /** Countries that drive on the left: their U-turns go right, their roundabouts clockwise. */
    private val leftHand = setOf(
        "GB", "IE", "JP", "AU", "NZ", "IN", "ZA", "HK", "MO", "SG", "MY", "TH", "ID", "PK", "BD", "LK", "NP", "KE", "UG",
        "TZ", "ZM", "ZW", "MZ", "BW", "NA", "MW", "CY", "MT", "JM", "TT", "BS", "BB", "GY", "SR", "FJ", "PG", "BN", "MU",
    )

    fun leftHand(context: Context) = context.resources.configuration.locales[0].country in leftHand

    /** The Car App Library's type for t, with a roundabout's exit (0: not known). */
    fun type(t: Turn, exit: Long, left: Boolean): Int = when (t) {
        Turn.DEPART -> CarManeuver.TYPE_DEPART
        Turn.SLIGHT_LEFT -> CarManeuver.TYPE_TURN_SLIGHT_LEFT
        Turn.LEFT -> CarManeuver.TYPE_TURN_NORMAL_LEFT
        Turn.SHARP_LEFT -> CarManeuver.TYPE_TURN_SHARP_LEFT
        Turn.SLIGHT_RIGHT -> CarManeuver.TYPE_TURN_SLIGHT_RIGHT
        Turn.RIGHT -> CarManeuver.TYPE_TURN_NORMAL_RIGHT
        Turn.SHARP_RIGHT -> CarManeuver.TYPE_TURN_SHARP_RIGHT
        Turn.U_TURN -> if (left) CarManeuver.TYPE_U_TURN_RIGHT else CarManeuver.TYPE_U_TURN_LEFT
        Turn.KEEP_LEFT -> CarManeuver.TYPE_KEEP_LEFT
        Turn.KEEP_RIGHT -> CarManeuver.TYPE_KEEP_RIGHT
        Turn.MERGE -> CarManeuver.TYPE_MERGE_SIDE_UNSPECIFIED
        Turn.EXIT_LEFT -> CarManeuver.TYPE_OFF_RAMP_NORMAL_LEFT
        Turn.EXIT_RIGHT -> CarManeuver.TYPE_OFF_RAMP_NORMAL_RIGHT
        Turn.ROUNDABOUT -> when {
            exit > 0 && left -> CarManeuver.TYPE_ROUNDABOUT_ENTER_AND_EXIT_CW
            exit > 0 -> CarManeuver.TYPE_ROUNDABOUT_ENTER_AND_EXIT_CCW
            left -> CarManeuver.TYPE_ROUNDABOUT_ENTER_CW
            else -> CarManeuver.TYPE_ROUNDABOUT_ENTER_CCW
        }
        Turn.FERRY -> CarManeuver.TYPE_FERRY_BOAT
        Turn.ARRIVE -> CarManeuver.TYPE_DESTINATION
        Turn.ARRIVE_LEFT -> CarManeuver.TYPE_DESTINATION_LEFT
        Turn.ARRIVE_RIGHT -> CarManeuver.TYPE_DESTINATION_RIGHT
        else -> CarManeuver.TYPE_STRAIGHT
    }

    private val drawn = HashMap<String, Bitmap>()

    /** t's arrow, size pixels square. */
    fun icon(t: Turn, left: Boolean, size: Int = 128): Bitmap {
        val key = "${t.value}.$left.$size"
        drawn[key]?.let { return it }
        val b = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        draw(Canvas(b), t, left, size.toFloat())
        drawn[key] = b
        return b
    }

    /** The start button's arrow (a preview's, when the app gives no title). */
    fun go(size: Int = 96): Bitmap = icon(Turn.STRAIGHT, false, size)

    private fun draw(c: Canvas, t: Turn, left: Boolean, s: Float) {
        val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply {
            color = Color.WHITE
            style = Paint.Style.STROKE
            strokeWidth = s * 0.11f
            strokeCap = Paint.Cap.ROUND
            strokeJoin = Paint.Join.ROUND
        }
        val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.WHITE; style = Paint.Style.FILL }
        /** A stem up from the bottom to (0.5, bend), then a line at angle degrees (0 up, + right), with its head. */
        fun turn(angle: Double, bend: Float = 0.55f, length: Float = 0.3f) {
            val a = Math.toRadians(angle)
            val ex = 0.5f + (sin(a) * length).toFloat()
            val ey = bend - (cos(a) * length).toFloat()
            c.drawPath(Path().apply { moveTo(0.5f * s, 0.92f * s); lineTo(0.5f * s, bend * s); lineTo(ex * s, ey * s) }, stroke)
            head(c, fill, ex * s, ey * s, a, s)
        }
        when (t) {
            Turn.STRAIGHT, Turn.DEPART -> {
                turn(0.0, 0.6f, 0.42f)
                if (t == Turn.DEPART) c.drawCircle(0.5f * s, 0.88f * s, s * 0.08f, fill)
            }
            Turn.SLIGHT_LEFT, Turn.KEEP_LEFT, Turn.EXIT_LEFT -> turn(-40.0)
            Turn.LEFT -> turn(-90.0, 0.5f)
            Turn.SHARP_LEFT -> turn(-140.0, 0.4f, 0.32f)
            Turn.SLIGHT_RIGHT, Turn.KEEP_RIGHT, Turn.EXIT_RIGHT -> turn(40.0)
            Turn.RIGHT -> turn(90.0, 0.5f)
            Turn.SHARP_RIGHT -> turn(140.0, 0.4f, 0.32f)
            Turn.U_TURN -> {
                // Up, around, and back down: to the left where traffic keeps right.
                val (from, to) = if (left) 0.35f to 0.65f else 0.65f to 0.35f
                val r = RectF(0.35f * s, 0.22f * s, 0.65f * s, 0.52f * s)
                c.drawPath(Path().apply {
                    moveTo(from * s, 0.92f * s)
                    lineTo(from * s, 0.37f * s)
                    arcTo(r, 0f, if (left) 180f else -180f)
                    lineTo(to * s, 0.62f * s)
                }, stroke)
                head(c, fill, to * s, 0.66f * s, Math.PI, s)
            }
            Turn.MERGE -> {
                c.drawPath(Path().apply { moveTo(0.28f * s, 0.92f * s); quadTo(0.28f * s, 0.6f * s, 0.5f * s, 0.48f * s) }, stroke)
                turn(0.0, 0.92f, 0.62f)
            }
            Turn.ROUNDABOUT -> {
                c.drawCircle(0.5f * s, 0.45f * s, 0.16f * s, stroke)
                c.drawLine(0.5f * s, 0.92f * s, 0.5f * s, 0.61f * s, stroke)
                val a = Math.toRadians(if (left) -45.0 else 45.0)
                val sx = 0.5f + (sin(a) * 0.16).toFloat()
                val sy = 0.45f - (cos(a) * 0.16).toFloat()
                val ex = sx + (sin(a) * 0.2).toFloat()
                val ey = sy - (cos(a) * 0.2).toFloat()
                c.drawLine(sx * s, sy * s, ex * s, ey * s, stroke)
                head(c, fill, ex * s, ey * s, a, s)
            }
            Turn.FERRY -> {
                c.drawPath(Path().apply {
                    moveTo(0.18f * s, 0.5f * s); lineTo(0.82f * s, 0.5f * s); lineTo(0.7f * s, 0.68f * s); lineTo(0.3f * s, 0.68f * s); close()
                }, fill)
                c.drawRect(0.38f * s, 0.32f * s, 0.62f * s, 0.5f * s, fill)
                c.drawPath(Path().apply {
                    moveTo(0.12f * s, 0.82f * s)
                    quadTo(0.24f * s, 0.74f * s, 0.36f * s, 0.82f * s)
                    quadTo(0.48f * s, 0.9f * s, 0.6f * s, 0.82f * s)
                    quadTo(0.72f * s, 0.74f * s, 0.88f * s, 0.82f * s)
                }, stroke)
            }
            Turn.ARRIVE, Turn.ARRIVE_LEFT, Turn.ARRIVE_RIGHT -> {
                // A flag, on the side the destination is.
                val x = when (t) { Turn.ARRIVE_LEFT -> 0.3f; Turn.ARRIVE_RIGHT -> 0.62f; else -> 0.42f }
                c.drawLine(x * s, 0.88f * s, x * s, 0.14f * s, stroke)
                c.drawPath(Path().apply {
                    moveTo(x * s, 0.14f * s); lineTo((x + 0.36f) * s, 0.27f * s); lineTo(x * s, 0.42f * s); close()
                }, fill)
            }
            else -> turn(0.0, 0.6f, 0.42f)
        }
    }

    /** An arrowhead at (x, y), pointing at angle a (radians, 0 up, + right). */
    private fun head(c: Canvas, p: Paint, x: Float, y: Float, a: Double, s: Float) {
        val len = s * 0.2f
        val half = s * 0.14f
        val dx = sin(a).toFloat()
        val dy = -cos(a).toFloat()
        // The tip beyond the line's end, the base across it.
        val tx = x + dx * len * 0.55f
        val ty = y + dy * len * 0.55f
        val bx = x - dx * len * 0.45f
        val by = y - dy * len * 0.45f
        c.drawPath(Path().apply {
            moveTo(tx, ty)
            lineTo(bx - dy * half, by + dx * half)
            lineTo(bx + dy * half, by - dx * half)
            close()
        }, p)
    }
}
