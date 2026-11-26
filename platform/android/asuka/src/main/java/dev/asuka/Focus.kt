package dev.asuka

import android.os.Build
import android.view.KeyCharacterMap
import android.view.KeyEvent
import android.view.View
import android.widget.EditText
import java.util.IdentityHashMap

/*
 * The keyboard's focus (RFC 0010), as Android has it: a view takes it from
 * a hardware keyboard (Tab, the arrows) and programmatically, never from
 * a tap in touch mode (only a field is focusable in touch mode). The
 * window's focus changes are reported as FocusChange for the node whose
 * view has it (a field keeps Focus and Blur): 2 when the keyboard moved
 * it (the window isn't in touch mode: a key was the last input), else 1.
 * Focusable makes a container a tab stop; a pressable one (it listens for
 * Press: clickable, so FOCUSABLE_AUTO) and an adjustable one already are.
 */

/** FocusChange's value for a view taking the focus: 2 from the keyboard (not in touch mode), else 1. */
fun focusHow(touchMode: Boolean): Long = if (touchMode) 1L else 2L

/**
 * The text a key going down types, as KeyText sends it: a printable
 * character (a space too) typed without Ctrl, Alt or Meta; null for a
 * control character (Enter, Tab), a dead key's accent or a shortcut.
 */
fun keyTextOf(char: Int, meta: Int): String? {
    if (meta and (KeyEvent.META_CTRL_ON or KeyEvent.META_ALT_ON or KeyEvent.META_META_ON) != 0) return null
    if (char and KeyCharacterMap.COMBINING_ACCENT != 0) return null
    if (char < 0x20 || char == 0x7f || !Character.isValidCodePoint(char)) return null
    return String(Character.toChars(char))
}

/** The kinds whose views hold other nodes' views: a view in one that's no node's is the host's own (a toolbar's search field). */
private val holders = setOf(
    0, AsukaKind.VIEW, AsukaKind.SCROLL, AsukaKind.LIST, AsukaKind.STACK, AsukaKind.TABS, AsukaKind.SCREEN,
    AsukaKind.SHEET, AsukaKind.POPOVER, AsukaKind.OVERLAY, AsukaKind.SPLIT, AsukaKind.ACCESSORY, AsukaKind.SELECTABLE,
)

/**
 * The node a focused view is: the node whose view it is, or the control
 * it's part of (a segmented control's button, a picker's field); null
 * for the host's own views and a list's cells.
 */
fun AsukaRuntime.nodeOfFocus(v: View?): AsukaNode? {
    if (v == null) return null
    val byView = IdentityHashMap<View, AsukaNode>(nodes.size)
    for (n in nodes.values) {
        byView[n.view] = n
        n.wrapper?.let { byView[it] = n }
    }
    var x: Any? = v
    while (x is View) {
        val n = byView[x]
        if (n != null) return if (n.kind != 0 && (x === v || n.kind !in holders)) n else null
        x = x.parent
    }
    return null
}

/** A window's focus moved from old to new (OnGlobalFocusChangeListener): FocusChange to the nodes that lost and took it. */
fun AsukaRuntime.focusMoved(old: View?, new: View?) {
    val was = nodeOfFocus(old)
    val now = nodeOfFocus(new)
    if (was === now) return
    if (was != null) {
        // Focused by the Focus command in touch mode: a tap takes no focus again.
        if (was.touchFocused) {
            was.touchFocused = false
            if (was.kind != AsukaKind.TEXT_FIELD) was.view.isFocusableInTouchMode = false
        }
        if (was.kind != AsukaKind.TEXT_FIELD) send(AsukaEvents.focusChange(was.id, 0))
    }
    if (now != null && now.kind != AsukaKind.TEXT_FIELD) send(AsukaEvents.focusChange(now.id, focusHow(new!!.isInTouchMode)))
}

/**
 * The Focus command: a field takes the focus and shows the keyboard;
 * another view takes it in touch mode too (focusable in touch mode until
 * it loses it, so a tap doesn't focus it afterwards).
 */
fun AsukaRuntime.focusNode(n: AsukaNode) {
    val v = n.view
    if (v is EditText) {
        v.requestFocus()
        app.getSystemService(android.view.inputmethod.InputMethodManager::class.java)?.showSoftInput(v, 0)
        return
    }
    if (v.hasFocus()) return
    if (v.isInTouchMode && v.isFocusable && !v.isFocusableInTouchMode) {
        v.isFocusableInTouchMode = true
        n.touchFocused = true
    }
    if (!v.requestFocus() && n.touchFocused) {
        n.touchFocused = false
        v.isFocusableInTouchMode = false
    }
}

/** The Blur command: the view (or what's focused in it) gives the focus up. */
fun AsukaRuntime.blurNode(n: AsukaNode) {
    val v = n.view
    if (v.hasFocus()) v.clearFocus()
    if (v is EditText) app.getSystemService(android.view.inputmethod.InputMethodManager::class.java)?.hideSoftInputFromWindow(v.windowToken, 0)
}

/**
 * A field being edited that goes away takes the keyboard with it, as on
 * iOS: the focus it leaves may go to a view that keeps the keyboard up (a
 * WebView). As the Blur command does, before its view leaves the window.
 */
fun AsukaRuntime.blurLeaving(n: AsukaNode) {
    val f = activity?.currentFocus as? EditText ?: return
    var x: View? = f
    while (x != null && x !== n.outer && x !== n.view) x = x.parent as? View
    if (x == null) return
    f.clearFocus()
    app.getSystemService(android.view.inputmethod.InputMethodManager::class.java)?.hideSoftInputFromWindow(f.windowToken, 0)
}

/** The focus props: Focusable, NoFocusOnPress, NoFocusRing, FocusTrap. False: not one of them. */
fun AsukaRuntime.setFocusProp(n: AsukaNode, prop: Int, on: Boolean): Boolean {
    val v = n.view
    when (prop) {
        AsukaProp.FOCUSABLE -> if (n.kind != AsukaKind.TEXT_FIELD) {
            // A tab stop; a tap in touch mode doesn't focus it, as a button's doesn't.
            // Before Android 8 there's no FOCUSABLE_AUTO: a pressable view stays focusable.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) v.focusable = if (on) View.FOCUSABLE else View.FOCUSABLE_AUTO
            else v.isFocusable = on || v.isClickable
            if (on && !n.touchFocused) v.isFocusableInTouchMode = false
        }
        AsukaProp.NO_FOCUS_ON_PRESS -> if (on && n.kind != AsukaKind.TEXT_FIELD && !n.touchFocused) v.isFocusableInTouchMode = false
        AsukaProp.NO_FOCUS_RING -> {
            // Before Android 8 there's no default focus highlight: only the container's ring.
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) v.defaultFocusHighlightEnabled = !on
            (v as? AsukaContainer)?.let { it.noFocusRing = on; it.refreshDrawableState() }
        }
        AsukaProp.FOCUS_TRAP -> (v as? AsukaContainer)?.focusTrap = on
        else -> return false
    }
    return true
}

/**
 * KeyText: a key going down that typed text while a view listening for
 * it has the focus, which nothing took (not its Keys, nor the view: a
 * pressable one's Enter and Space press it). Reports whether it was sent.
 */
fun AsukaRuntime.keyText(e: KeyEvent, focused: View?): Boolean {
    if (e.action != KeyEvent.ACTION_DOWN) return false
    val n = nodeOfFocus(focused) ?: return false
    if (n.kind == AsukaKind.TEXT_FIELD || !n.listens(AsukaEventKind.KEY_TEXT)) return false
    val t = keyTextOf(e.unicodeChar, e.metaState) ?: return false
    val id = n.id
    send(AsukaEvents.keyText(id, t))
    return true
}

/** Wires PressPoint: where a press begins on the view, in points, before its Pressed or Press. */
fun AsukaRuntime.wirePressPoint(n: AsukaNode) {
    val id = n.id
    val on = n.listens(AsukaEventKind.PRESS_POINT)
    val f: ((Float, Float) -> Unit)? = if (on) { x, y ->
        val d = density
        emit(id, AsukaEventKind.PRESS_POINT) { AsukaEvents.pressPoint(id, (x / d).toDouble(), (y / d).toDouble()) }
    } else null
    when (val v = n.view) {
        is AsukaContainer -> v.onPressPoint = f
        // A button's, a text's: before its own handling, which it doesn't take.
        is android.widget.TextView, is AsukaSymbol, is AsukaShape -> v.setOnTouchListener(if (f == null) null else View.OnTouchListener { _, e ->
            if (e.actionMasked == android.view.MotionEvent.ACTION_DOWN) f(e.x, e.y)
            false
        })
    }
}
