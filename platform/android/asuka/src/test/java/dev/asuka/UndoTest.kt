package dev.asuka

import android.view.KeyEvent
import android.view.PointerIcon
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test
import java.nio.ByteBuffer

// UndoState's args, the keys that undo and redo the app's changes, and
// Cursor as the system's pointer icons (Undo.kt).
class UndoTest {
    private fun state(flags: Int, undo: String, redo: String): ByteBuffer {
        val w = AsukaWriter()
        w.u8(flags)
        w.string(undo)
        w.string(redo)
        return ByteBuffer.wrap(w.bytes())
    }

    @Test
    fun undoState() {
        assertEquals(AsukaUndoState(true, false, "Delete", ""), AsukaUndoState.decode(state(1, "Delete", "")))
        assertEquals(AsukaUndoState(true, true, "Rename", "Star"), AsukaUndoState.decode(state(3, "Rename", "Star")))
        assertEquals(AsukaUndoState(), AsukaUndoState.decode(ByteBuffer.wrap(ByteArray(0))))
        // Cut short: the names it has.
        assertEquals(AsukaUndoState(false, true, "", ""), AsukaUndoState.decode(ByteBuffer.wrap(byteArrayOf(2, 9, 0))))
    }

    @Test
    fun undoKeys() {
        val ctrl = KeyEvent.META_CTRL_ON or KeyEvent.META_CTRL_LEFT_ON
        val shift = KeyEvent.META_SHIFT_ON or KeyEvent.META_SHIFT_LEFT_ON
        assertEquals(0, AsukaUndoKeys.of(KeyEvent.KEYCODE_Z, ctrl))
        assertEquals(1, AsukaUndoKeys.of(KeyEvent.KEYCODE_Z, ctrl or shift))
        assertEquals(1, AsukaUndoKeys.of(KeyEvent.KEYCODE_Y, ctrl))
        assertEquals(-1, AsukaUndoKeys.of(KeyEvent.KEYCODE_Z, 0))
        assertEquals(-1, AsukaUndoKeys.of(KeyEvent.KEYCODE_Y, ctrl or shift))
        assertEquals(-1, AsukaUndoKeys.of(KeyEvent.KEYCODE_Z, ctrl or KeyEvent.META_ALT_ON))
        assertEquals(-1, AsukaUndoKeys.of(KeyEvent.KEYCODE_Z, KeyEvent.META_META_ON))
        assertEquals(-1, AsukaUndoKeys.of(KeyEvent.KEYCODE_X, ctrl))
    }

    @Test
    fun cursors() {
        assertNull(AsukaCursors.pointerType(AsukaCursor.AUTO))
        assertEquals(PointerIcon.TYPE_HAND, AsukaCursors.pointerType(AsukaCursor.POINTER))
        assertEquals(PointerIcon.TYPE_TEXT, AsukaCursors.pointerType(AsukaCursor.TEXT))
        assertEquals(PointerIcon.TYPE_NULL, AsukaCursors.pointerType(AsukaCursor.HIDDEN))
        assertEquals(PointerIcon.TYPE_TOP_LEFT_DIAGONAL_DOUBLE_ARROW, AsukaCursors.pointerType(AsukaCursor.RESIZE_DIAGONAL))
        assertEquals(PointerIcon.TYPE_TOP_RIGHT_DIAGONAL_DOUBLE_ARROW, AsukaCursors.pointerType(AsukaCursor.RESIZE_ANTI_DIAGONAL))
        assertEquals(PointerIcon.TYPE_HORIZONTAL_DOUBLE_ARROW, AsukaCursors.pointerType(AsukaCursor.RESIZE_COLUMN))
        assertEquals(PointerIcon.TYPE_WAIT, AsukaCursors.pointerType(AsukaCursor.PROGRESS))
        // Every value but Auto has an icon.
        for (c in AsukaCursor.ARROW..AsukaCursor.HIDDEN) assert(AsukaCursors.pointerType(c) != null) { "cursor $c" }
        assertNull(AsukaCursors.pointerType(99))
    }
}
