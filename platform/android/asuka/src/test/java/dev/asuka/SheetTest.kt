package dev.asuka

import com.google.android.material.bottomsheet.BottomSheetBehavior
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

// A Sheet's detents (Sheet.kt): what the core's Detents say, the heights
// they resolve to, and the three Material's sheet rests at.
class SheetTest {
    private fun detents(vararg d: Pair<Int, Float>): ByteBuffer {
        val b = ByteBuffer.allocate(4 + 5 * d.size).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(d.size)
        for ((k, v) in d) { b.put(k.toByte()); b.putFloat(v) }
        b.flip()
        return b
    }

    @Test
    fun decodesDetents() {
        val (k, v) = decodeDetents(detents(AsukaDetentKind.HEIGHT to 120f, AsukaDetentKind.MEDIUM to 0f, AsukaDetentKind.FRACTION to 0.25f))
        assertArrayEquals(intArrayOf(AsukaDetentKind.HEIGHT, AsukaDetentKind.MEDIUM, AsukaDetentKind.FRACTION), k)
        assertArrayEquals(floatArrayOf(120f, 0f, 0.25f), v, 0f)
        // Cut short: the detents before the cut; none at all: large alone.
        val cut = detents(AsukaDetentKind.LARGE to 0f, AsukaDetentKind.FIT to 0f)
        cut.limit(cut.limit() - 2)
        assertEquals(1, decodeDetents(cut).first.size)
        assertEquals(0, decodeDetents(ByteBuffer.allocate(0)).first.size)
    }

    @Test
    fun resolvesHeights() {
        val kinds = intArrayOf(AsukaDetentKind.HEIGHT, AsukaDetentKind.MEDIUM, AsukaDetentKind.LARGE, AsukaDetentKind.FRACTION, AsukaDetentKind.FIT, AsukaDetentKind.HEIGHT)
        val values = floatArrayOf(120f, 0f, 0f, 0.25f, 0f, 9000f)
        // 2x: 120pt is 240px; a fraction of large; fit is its content's and
        // the grabber; nothing taller than large.
        assertArrayEquals(intArrayOf(240, 900, 2000, 500, 380, 2000), resolveDetents(kinds, values, 2f, 2000, 900, 300, 80))
        // Its content not measured yet: fit is large.
        assertEquals(2000, resolveDetents(intArrayOf(AsukaDetentKind.FIT), floatArrayOf(0f), 2f, 2000, 900, 0, 80)[0])
        assertArrayEquals(intArrayOf(2000), resolveDetents(IntArray(0), FloatArray(0), 2f, 2000, 900, 0, 0))
    }

    @Test
    fun restsAtThreeAtMost() {
        // Shortest first; one of each height.
        assertArrayEquals(intArrayOf(1, 0), sheetSlots(intArrayOf(2000, 900), -1))
        assertArrayEquals(intArrayOf(0), sheetSlots(intArrayOf(2000, 2000), -1))
        assertArrayEquals(intArrayOf(2, 0, 1), sheetSlots(intArrayOf(500, 2000, 240), -1))
        // More than three: the shortest, the tallest, and the one it's at
        // between them, else the next shortest.
        val four = intArrayOf(240, 500, 900, 2000)
        assertArrayEquals(intArrayOf(0, 1, 3), sheetSlots(four, -1))
        assertArrayEquals(intArrayOf(0, 2, 3), sheetSlots(four, 2))
        assertArrayEquals(intArrayOf(0, 1, 3), sheetSlots(four, 3))
    }

    @Test
    fun mapsSlotsToStates() {
        assertEquals(BottomSheetBehavior.STATE_EXPANDED, sheetSlotState(0, 1))
        assertEquals(BottomSheetBehavior.STATE_COLLAPSED, sheetSlotState(0, 2))
        assertEquals(BottomSheetBehavior.STATE_EXPANDED, sheetSlotState(1, 2))
        assertEquals(BottomSheetBehavior.STATE_HALF_EXPANDED, sheetSlotState(1, 3))
        for (n in 1..3) for (pos in 0 until n) assertEquals(pos, sheetStateSlot(sheetSlotState(pos, n), n))
        // With fewer than three, half-expanded is the tallest's height.
        assertEquals(1, sheetStateSlot(BottomSheetBehavior.STATE_HALF_EXPANDED, 2))
        assertEquals(-1, sheetStateSlot(BottomSheetBehavior.STATE_DRAGGING, 3))
    }

    @Test
    fun dimsAboveTheUndimmedDetent() {
        assertEquals(0f, sheetScrim(300, 900, 2000), 0f)
        assertEquals(0f, sheetScrim(900, 900, 2000), 0f)
        assertEquals(0.16f, sheetScrim(1450, 900, 2000), 0.0001f)
        assertEquals(0.32f, sheetScrim(2000, 900, 2000), 0f)
        assertEquals(0f, sheetScrim(2000, 2000, 2000), 0f) // undimmed at its tallest: never
    }

    // A Stepper's buttons sit at the start of the frame the core gives it,
    // which is physical: the left, or right to left its right end, never
    // pinned to its left (x = 0) when the frame is wider than the pair.
    @Test
    fun stepperSitsAtItsFramesStart() {
        val side = 126
        val gap = 10
        assertEquals(0, stepperPairX(2 * side + gap, side, gap, rtl = false))
        assertEquals(0, stepperPairX(2 * side + gap, side, gap, rtl = true))
        assertEquals(0, stepperPairX(1000, side, gap, rtl = false))
        assertEquals(1000 - (2 * side + gap), stepperPairX(1000, side, gap, rtl = true))
        assertEquals(0, stepperPairX(100, side, gap, rtl = true)) // narrower than the pair: from its left
    }
}
