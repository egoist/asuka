package dev.asuka

import androidx.core.util.Consumer
import androidx.window.java.layout.WindowInfoTrackerCallbackAdapter
import androidx.window.layout.DisplayFeature
import androidx.window.layout.FoldingFeature
import androidx.window.layout.WindowInfoTracker
import androidx.window.layout.WindowLayoutInfo

// A foldable's fold (Jetpack WindowManager's FoldingFeature): how the
// device is held (the environment's posture) and where its fold or hinge
// crosses the window (its fold), followed while the activity shows. A
// device without one, or a window the fold doesn't cross, is flat, with
// no fold.

/** The fold as the environment has it: its posture (ops.Posture), and its frame in the window, in px. */
class AsukaFold(val posture: Int = 0, val x: Int = 0, val y: Int = 0, val width: Int = 0, val height: Int = 0) {
    override fun equals(other: Any?) = other is AsukaFold && other.posture == posture && other.x == x && other.y == y &&
        other.width == width && other.height == height
    override fun hashCode() = ((posture * 31 + x) * 31 + y) * 31 + width * 7 + height

    companion object {
        /** The posture and fold the window's layout's features say. */
        fun of(features: List<DisplayFeature>): AsukaFold {
            val f = features.filterIsInstance<FoldingFeature>().firstOrNull() ?: return AsukaFold()
            val posture = when {
                f.state != FoldingFeature.State.HALF_OPENED -> 0 // flat
                f.orientation == FoldingFeature.Orientation.HORIZONTAL -> 1 // tabletop
                else -> 2 // book
            }
            val b = f.bounds
            return AsukaFold(posture, b.left, b.top, b.width(), b.height())
        }
    }
}

/** Follows the window's fold while the activity shows; the environment reports it. */
class AsukaFoldTracker(private val act: AsukaActivity) {
    private val adapter = WindowInfoTrackerCallbackAdapter(WindowInfoTracker.getOrCreate(act))
    private val listener = Consumer<WindowLayoutInfo> { info ->
        val f = AsukaFold.of(info.displayFeatures)
        if (f == act.fold) return@Consumer
        act.fold = f
        act.foldChanged()
    }

    fun start() = try {
        adapter.addWindowLayoutInfoListener(act, androidx.core.content.ContextCompat.getMainExecutor(act), listener)
    } catch (e: Throwable) {
        // A device whose WindowManager extensions misbehave: no fold.
    }

    fun stop() = try {
        adapter.removeWindowLayoutInfoListener(listener)
    } catch (e: Throwable) {
    }
}
