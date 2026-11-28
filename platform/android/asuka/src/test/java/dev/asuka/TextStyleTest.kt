package dev.asuka

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

// The pure parts of the text and box styles: MinimumScale's fit and
// CornerRadii's clamping.
class TextStyleTest {
    @Test fun fitsAtItsOwnSize() {
        assertEquals(1f, fitTextScale(0.5f) { true }, 0f)
    }

    @Test fun neverUnderTheMinimum() {
        assertEquals(0.5f, fitTextScale(0.5f) { false }, 0f)
    }

    @Test fun theLargestScaleThatFits() {
        // A text 160 wide in 100: it fits from 0.625 down.
        val s = fitTextScale(0.25f) { 160 * it <= 100 }
        assertEquals(0.625f, s, 0.01f)
        assertTrue(160 * s <= 100)
    }

    @Test fun radiiThatFitStay() {
        val out = FloatArray(4)
        clampRadii(floatArrayOf(10f, 20f, 0f, 5f), 100f, 50f, out)
        assertArrayEquals(floatArrayOf(10f, 20f, 0f, 5f), out, 0f)
    }

    @Test fun radiiScaleDownTogether() {
        val out = FloatArray(4)
        // A capsule: 999 on each corner of a 100 by 40 box is 20.
        clampRadii(floatArrayOf(999f, 999f, 999f, 999f), 100f, 40f, out)
        assertArrayEquals(floatArrayOf(20f, 20f, 20f, 20f), out, 1e-3f)
        // The top's 60 and 60 on a 100 wide box: all by 100 / 120.
        clampRadii(floatArrayOf(60f, 60f, 0f, 12f), 100f, 100f, out)
        assertArrayEquals(floatArrayOf(50f, 50f, 0f, 10f), out, 1e-3f)
    }
}
