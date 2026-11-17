package dev.asuka

import android.view.KeyCharacterMap
import android.view.KeyEvent
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.nio.ByteBuffer
import java.nio.ByteOrder

// RFC 0010's host contract, the parts that are the host's own logic:
// decoding A11yRange, the roles' Android classes, AnchorFrame's payload
// and when it's sent again, FocusChange's value, KeyText's text, and the
// events' encodings (ops.Event's).
class HeadlessTest {
    private fun le(n: Int) = ByteBuffer.allocate(n).order(ByteOrder.LITTLE_ENDIAN)

    @Test
    fun decodesRange() {
        val b = le(24).putDouble(0.0).putDouble(100.0).putDouble(40.0)
        b.flip()
        assertArrayEquals(doubleArrayOf(0.0, 100.0, 40.0), decodeA11yRange(b), 0.0)
        assertEquals(24, b.remaining()) // read from a copy: the prop's bytes stay
        assertNull(decodeA11yRange(ByteBuffer.allocate(0))) // empty: none
        assertNull(decodeA11yRange(ByteBuffer.wrap(ByteArray(23)))) // cut short
    }

    @Test
    fun rolesAsAndroidClasses() {
        assertEquals("android.widget.CheckBox", a11yClassName(AsukaRole.CHECKBOX))
        assertEquals("android.widget.CheckBox", a11yClassName(AsukaRole.MENU_ITEM_CHECKBOX))
        assertEquals("android.widget.RadioButton", a11yClassName(AsukaRole.RADIO))
        assertEquals("android.widget.RadioButton", a11yClassName(AsukaRole.MENU_ITEM_RADIO))
        assertEquals("android.widget.Switch", a11yClassName(AsukaRole.SWITCH))
        assertEquals("android.widget.SeekBar", a11yClassName(AsukaRole.SLIDER))
        assertEquals("android.widget.ProgressBar", a11yClassName(AsukaRole.PROGRESS_BAR))
        assertEquals("android.widget.TabWidget", a11yClassName(AsukaRole.TAB_LIST))
        assertEquals("android.app.ActionBar\$Tab", a11yClassName(AsukaRole.TAB))
        assertEquals("android.widget.Spinner", a11yClassName(AsukaRole.COMBO_BOX))
        assertEquals("android.widget.Button", a11yClassName(AsukaRole.BUTTON))
        assertNull(a11yClassName(AsukaRole.NONE))
        assertNull(a11yClassName(AsukaRole.GROUP))
        assertNull(a11yClassName(AsukaRole.HEADER)) // a heading, not a class
        // Every role the wire has maps or is left to the view, never fails.
        for (r in AsukaRole.NONE..AsukaRole.STATUS) a11yClassName(r)
    }

    @Test
    fun checkableRoles() {
        for (r in listOf(AsukaRole.CHECKBOX, AsukaRole.RADIO, AsukaRole.SWITCH, AsukaRole.MENU_ITEM_CHECKBOX, AsukaRole.MENU_ITEM_RADIO)) assertTrue(a11yCheckable(r))
        for (r in listOf(AsukaRole.NONE, AsukaRole.BUTTON, AsukaRole.OPTION, AsukaRole.TAB, AsukaRole.SLIDER)) assertFalse(a11yCheckable(r))
    }

    @Test
    fun anchorFramePayload() {
        val want = le(17).putFloat(10f).putFloat(20.5f).putFloat(100f).putFloat(44f).put(1.toByte()).array()
        assertArrayEquals(want, encodeAnchorFrame(10f, 20.5f, 100f, 44f, true))
        assertEquals(0, encodeAnchorFrame(0f, 0f, 0f, 0f, false)[16].toInt())
    }

    @Test
    fun anchorFrameOnlyWhenChanged() {
        val r = AsukaAnchorReport()
        assertArrayEquals(encodeAnchorFrame(1f, 2f, 3f, 4f, true), r.changed(1f, 2f, 3f, 4f, true)) // the first
        assertNull(r.changed(1f, 2f, 3f, 4f, true)) // never an unchanged one
        assertArrayEquals(encodeAnchorFrame(1f, 7f, 3f, 4f, true), r.changed(1f, 7f, 3f, 4f, true)) // scrolled
        assertArrayEquals(encodeAnchorFrame(1f, 7f, 3f, 4f, false), r.changed(1f, 7f, 3f, 4f, false)) // scrolled out of view
        assertNull(r.changed(1f, 7f, 3f, 4f, false))
        r.forget() // a new anchor: reported whatever it is
        assertArrayEquals(encodeAnchorFrame(1f, 7f, 3f, 4f, false), r.changed(1f, 7f, 3f, 4f, false))
        assertArrayEquals(floatArrayOf(1f, 7f, 3f, 4f), r.lastFrame(), 0f)
    }

    @Test
    fun focusChangeValues() {
        assertEquals(1L, focusHow(touchMode = true)) // a pointer's press, or programmatic
        assertEquals(2L, focusHow(touchMode = false)) // the keyboard: a key was the last input
    }

    @Test
    fun keyText() {
        assertEquals("a", keyTextOf('a'.code, 0))
        assertEquals("A", keyTextOf('A'.code, KeyEvent.META_SHIFT_ON))
        assertEquals(" ", keyTextOf(' '.code, 0)) // a space not pressing anything is typed
        assertEquals("é", keyTextOf('é'.code, 0))
        assertEquals("😀", keyTextOf(0x1F600, 0))
        assertNull(keyTextOf('\n'.code, 0)) // Enter
        assertNull(keyTextOf('\t'.code, 0)) // Tab
        assertNull(keyTextOf(0, 0)) // no character (an arrow)
        assertNull(keyTextOf(0x7f, 0))
        assertNull(keyTextOf('a'.code, KeyEvent.META_CTRL_ON)) // a shortcut
        assertNull(keyTextOf('a'.code, KeyEvent.META_ALT_ON))
        assertNull(keyTextOf('a'.code, KeyEvent.META_META_ON))
        assertNull(keyTextOf('`'.code or KeyCharacterMap.COMBINING_ACCENT, 0)) // a dead key's accent
    }

    @Test
    fun eventEncodings() {
        fun head(id: Int, kind: Int, n: Int) = le(6 + n).putInt(id).putShort(kind.toShort())
        assertArrayEquals(head(7, AsukaEventKind.FOCUS_CHANGE, 8).putLong(2).array(), AsukaEvents.focusChange(7, 2))
        assertArrayEquals(head(7, AsukaEventKind.KEY_TEXT, 5).putInt(1).put('q'.code.toByte()).array(), AsukaEvents.keyText(7, "q"))
        assertArrayEquals(head(7, AsukaEventKind.OUTSIDE_PRESS, 16).putDouble(1.5).putDouble(2.0).array(), AsukaEvents.outsidePress(7, 1.5, 2.0))
        assertArrayEquals(head(7, AsukaEventKind.PRESS_POINT, 16).putDouble(3.0).putDouble(4.5).array(), AsukaEvents.pressPoint(7, 3.0, 4.5))
        val frame = encodeAnchorFrame(1f, 2f, 3f, 4f, true)
        assertArrayEquals(head(7, AsukaEventKind.ANCHOR_FRAME, 4 + 17).putInt(17).put(frame).array(), AsukaEvents.anchorFrame(7, frame))
    }
}
