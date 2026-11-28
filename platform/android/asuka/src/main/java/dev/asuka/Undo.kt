package dev.asuka

import android.view.KeyEvent
import android.view.PointerIcon
import android.view.View
import android.widget.TextView
import java.nio.ByteBuffer
import java.nio.ByteOrder

// The app's undo stack (the core's: ui's undo) on a hardware keyboard:
// Ctrl+Z undoes, Ctrl+Shift+Z and Ctrl+Y redo, sent to the root as Undo
// when no text field has the focus (a field being edited undoes its own
// typing: TextView's, a rich field's history) and nothing else took the
// key (a view's Keys, the focused view). Android has no system-wide undo
// of its own to give the app's stack to; UndoState is kept for what
// there is to undo and what it's called.
//
// And Cursor as the mouse pointer's icon (a mouse, a Chromebook, DeX):
// View.setPointerIcon with the system's icon, which a view inside that
// sets none shows too (ViewGroup.onResolvePointerIcon asks the child
// under the pointer first; a text field's I-beam is its own).

/** UndoState's: what there is to undo and redo, and what it's called. */
data class AsukaUndoState(
    val canUndo: Boolean = false,
    val canRedo: Boolean = false,
    val undoName: String = "",
    val redoName: String = "",
) {
    companion object {
        /** UndoState's args: u8 flags (1 undo, 2 redo), u32-length undo name, u32-length redo name. */
        fun decode(buf: ByteBuffer): AsukaUndoState {
            val b = buf.duplicate().order(ByteOrder.LITTLE_ENDIAN)
            if (b.remaining() < 1) return AsukaUndoState()
            val flags = b.get().toInt() and 0xff
            return AsukaUndoState(flags and 1 != 0, flags and 2 != 0, string(b), string(b))
        }

        private fun string(b: ByteBuffer): String {
            if (b.remaining() < 4) return ""
            val n = b.int
            if (n < 0 || n > b.remaining()) return ""
            val a = ByteArray(n)
            b.get(a)
            return String(a, Charsets.UTF_8)
        }
    }
}

object AsukaUndoKeys {
    /**
     * What a key going down asks of the app's undo: 0 undo (Ctrl+Z), 1
     * redo (Ctrl+Shift+Z, Ctrl+Y), -1 neither. Alt or Meta held: neither.
     */
    fun of(keyCode: Int, meta: Int): Int {
        val m = AsukaKeys.mods(meta)
        return when {
            keyCode == KeyEvent.KEYCODE_Z && m == 1 -> 0
            keyCode == KeyEvent.KEYCODE_Z && m == 3 -> 1
            keyCode == KeyEvent.KEYCODE_Y && m == 1 -> 1
            else -> -1
        }
    }
}

object AsukaCursors {
    /** The system pointer icon's type for a Cursor, or null for Auto (the parent's, else the view's own). */
    fun pointerType(cursor: Int): Int? = when (cursor) {
        AsukaCursor.ARROW -> PointerIcon.TYPE_ARROW
        AsukaCursor.POINTER -> PointerIcon.TYPE_HAND
        AsukaCursor.TEXT -> PointerIcon.TYPE_TEXT
        AsukaCursor.CROSSHAIR -> PointerIcon.TYPE_CROSSHAIR
        AsukaCursor.MOVE -> PointerIcon.TYPE_ALL_SCROLL
        AsukaCursor.GRAB -> PointerIcon.TYPE_GRAB
        AsukaCursor.GRABBING -> PointerIcon.TYPE_GRABBING
        AsukaCursor.NOT_ALLOWED -> PointerIcon.TYPE_NO_DROP
        AsukaCursor.WAIT, AsukaCursor.PROGRESS -> PointerIcon.TYPE_WAIT // Android's one busy pointer
        AsukaCursor.HELP -> PointerIcon.TYPE_HELP
        AsukaCursor.CONTEXT_MENU -> PointerIcon.TYPE_CONTEXT_MENU
        AsukaCursor.COPY -> PointerIcon.TYPE_COPY
        AsukaCursor.ALIAS -> PointerIcon.TYPE_ALIAS
        AsukaCursor.RESIZE_HORIZONTAL, AsukaCursor.RESIZE_COLUMN -> PointerIcon.TYPE_HORIZONTAL_DOUBLE_ARROW
        AsukaCursor.RESIZE_VERTICAL, AsukaCursor.RESIZE_ROW -> PointerIcon.TYPE_VERTICAL_DOUBLE_ARROW
        AsukaCursor.RESIZE_DIAGONAL -> PointerIcon.TYPE_TOP_LEFT_DIAGONAL_DOUBLE_ARROW
        AsukaCursor.RESIZE_ANTI_DIAGONAL -> PointerIcon.TYPE_TOP_RIGHT_DIAGONAL_DOUBLE_ARROW
        AsukaCursor.ZOOM_IN -> PointerIcon.TYPE_ZOOM_IN
        AsukaCursor.ZOOM_OUT -> PointerIcon.TYPE_ZOOM_OUT
        AsukaCursor.HIDDEN -> PointerIcon.TYPE_NULL
        else -> null
    }
}

/** The UndoState command. */
fun AsukaRuntime.setUndoState(args: ByteBuffer) {
    undoState = AsukaUndoState.decode(args)
}

/**
 * A key nothing took (a view's Keys, the focused view): Ctrl+Z, Ctrl+Shift+Z
 * or Ctrl+Y outside a text field is the app's undo or redo, if the root
 * takes Undo. True if it was.
 */
fun AsukaRuntime.undoKey(event: KeyEvent, focused: View?): Boolean {
    if (event.action != KeyEvent.ACTION_DOWN) return false
    val how = AsukaUndoKeys.of(event.keyCode, event.metaState)
    if (how < 0) return false
    if (focused is TextView && focused.onCheckIsTextEditor() && focused.isEnabled) return false // its own typing's
    val id = rootID
    if (nodes[id]?.listens(AsukaEventKind.UNDO) != true) return false
    send(AsukaEvents.undo(id, how.toLong()))
    return true
}

/** The Cursor prop: the view's pointer icon (Auto: none of its own). */
fun AsukaRuntime.setCursor(n: AsukaNode, cursor: Int) {
    val type = AsukaCursors.pointerType(cursor)
    n.view.pointerIcon = type?.let { PointerIcon.getSystemIcon(n.view.context, it) }
}
