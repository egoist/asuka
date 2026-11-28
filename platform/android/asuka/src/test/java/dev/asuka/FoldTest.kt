package dev.asuka

import android.graphics.Rect
import androidx.window.layout.FoldingFeature
import org.junit.Assert.assertEquals
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

// A foldable's fold as the environment reports it: the posture from the
// fold's state and orientation (Jetpack WindowManager's tabletop and book
// postures), and where it crosses the window.
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class FoldTest {
    private class Fold(
        override val bounds: Rect,
        override val state: FoldingFeature.State,
        override val orientation: FoldingFeature.Orientation,
    ) : FoldingFeature {
        override val isSeparating get() = state == FoldingFeature.State.HALF_OPENED
        override val occlusionType get() = FoldingFeature.OcclusionType.NONE
    }

    @Test
    fun postures() {
        assertEquals(AsukaFold(), AsukaFold.of(emptyList()))
        // Open flat, the fold down the middle: no posture, its place.
        val flat = AsukaFold.of(listOf(Fold(Rect(1038, 0, 1038, 2152), FoldingFeature.State.FLAT, FoldingFeature.Orientation.VERTICAL)))
        assertEquals(AsukaFold(0, 1038, 0, 0, 2152), flat)
        // Half open, the fold down it: a book.
        val book = AsukaFold.of(listOf(Fold(Rect(1038, 0, 1038, 2152), FoldingFeature.State.HALF_OPENED, FoldingFeature.Orientation.VERTICAL)))
        assertEquals(2, book.posture)
        // Half open, the fold across it: a laptop's (tabletop).
        val table = AsukaFold.of(listOf(Fold(Rect(0, 1076, 2076, 1076), FoldingFeature.State.HALF_OPENED, FoldingFeature.Orientation.HORIZONTAL)))
        assertEquals(AsukaFold(1, 0, 1076, 2076, 0), table)
    }
}
