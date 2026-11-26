package dev.asuka

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Rect
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.view.PixelCopy
import android.view.View
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.UUID

/**
 * A view drawn into an image file (imageedit.capture): the "view"
 * module's "capture". args: u32 the native view, f32 x, y, width, height
 * of what's drawn in it (points; a width below 0: all of it), u8 format
 * (0 JPEG, 1 PNG), f32 quality, f32 scale (pixels per point; 0: the
 * screen's); answers the file's path, in the caches. The view drawn into
 * a bitmap, what's around it transparent; one with a surface in it (a
 * video's, a map's), which drawing can't reach, is the activity's
 * window's pixels where it is (PixelCopy).
 */
fun AsukaRuntime.captureMethods(): Map<String, AsukaModuleMethod> = mapOf(
    "capture" to { args, reply ->
        val b = ByteBuffer.wrap(args).order(ByteOrder.LITTLE_ENDIAN)
        if (args.size < 29) {
            reply(Result.failure(Exception("bad arguments")))
        } else {
            val id = b.int
            val x = b.float; val y = b.float; val w = b.float; val h = b.float
            val png = b.get().toInt() == 1
            val quality = b.float
            val scale = b.float
            val v = nodes[id]?.view
            if (v == null || !v.isAttachedToWindow) {
                reply(Result.failure(Exception("capture: the view isn't showing")))
            } else {
                // The area, in the view's pixels.
                val area = if (w < 0 || h < 0) Rect(0, 0, v.width, v.height)
                else Rect((x * density).toInt(), (y * density).toInt(), ((x + w) * density).toInt(), ((y + h) * density).toInt())
                if (area.width() < 1 || area.height() < 1) {
                    reply(Result.failure(Exception("capture: the view has no size")))
                } else {
                    val k = if (scale > 0) scale / density else 1f
                    val done = { full: Bitmap ->
                        val out = if (k == 1f) full else Bitmap.createScaledBitmap(full, maxOf(1, (full.width * k).toInt()), maxOf(1, (full.height * k).toInt()), true)
                        val dir = File(app.cacheDir, "asuka-capture").apply { mkdirs() }
                        val file = File(dir, UUID.randomUUID().toString() + if (png) ".png" else ".jpg")
                        val ok = runCatching {
                            file.outputStream().use {
                                out.compress(if (png) Bitmap.CompressFormat.PNG else Bitmap.CompressFormat.JPEG, (quality * 100).toInt().coerceIn(0, 100), it)
                            }
                        }.isSuccess
                        if (ok) reply(Result.success(file.absolutePath.toByteArray())) else reply(Result.failure(Exception("capture: the image couldn't be written")))
                    }
                    val window = activity?.window
                    if (Build.VERSION.SDK_INT >= 26 && window != null && v.rootView === window.decorView && hasSurface(v)) {
                        val at = IntArray(2).also { v.getLocationInWindow(it) }
                        val src = Rect(area).apply { offset(at[0], at[1]) }
                        val bmp = Bitmap.createBitmap(src.width(), src.height(), Bitmap.Config.ARGB_8888)
                        PixelCopy.request(window, src, bmp, { result ->
                            if (result == PixelCopy.SUCCESS) done(bmp) else done(drawn(v, area, png))
                        }, Handler(Looper.getMainLooper()))
                    } else {
                        done(drawn(v, area, png))
                    }
                }
            }
        }
    },
)

/** Whether a SurfaceView or TextureView is in v, whose pixels drawing misses. */
private fun hasSurface(v: View): Boolean = when (v) {
    is android.view.SurfaceView, is android.view.TextureView -> true
    is android.view.ViewGroup -> (0 until v.childCount).any { hasSurface(v.getChildAt(it)) }
    else -> false
}

/** The view drawn into a bitmap of area (its pixels); JPEG's on white. */
private fun drawn(v: View, area: Rect, png: Boolean): Bitmap {
    val bmp = Bitmap.createBitmap(area.width(), area.height(), Bitmap.Config.ARGB_8888)
    val c = Canvas(bmp)
    if (!png) c.drawColor(Color.WHITE)
    c.translate(-area.left.toFloat(), -area.top.toFloat())
    v.draw(c)
    return bmp
}
