package dev.asuka

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ApplicationInfo
import android.view.View
import android.widget.CompoundButton
import android.widget.TextView
import java.io.File

/**
 * Drives a debug build from outside, for development and tests, as the
 * iOS host's AsukaAutomation does:
 *
 *     adb shell am broadcast -a dev.asuka.automation -p <package> --es commands 'tap Add'
 *
 * Commands, a line each: `tap <text>` presses the view whose text or
 * content description is <text> (or the pressable view it's in); `type
 * <text>` replaces the focused text field's text (or the first one's);
 * `submit` presses its return key; `back` is the system's back; `tab
 * <title>` selects the tab with that title; `rotate <degrees> <text>`
 * turns two fingers on the view with that text or content description
 * (a two-pointer touch, as a person's); `dump`
 * writes what the screen shows (each text, or content description, a
 * line each, in creation order) to files/asuka-dump.txt:
 *
 *     adb shell run-as <package> cat files/asuka-dump.txt
 *
 * `fits` writes whether each one-line text fits its view, ends in an
 * ellipsis or is cut off to files/asuka-fits.txt; `a11y` writes what
 * TalkBack is told about each view (its class, hint and content
 * description) to files/asuka-a11y.txt; `states` writes, for each view
 * that has a role, a state or the focus, its text and what TalkBack is
 * told of its states, and whether it has the keyboard's focus, to
 * files/asuka-states.txt (RFC 0010: a line each, `name=value` fields
 * after a tab; hidden=true: a modal hides it from TalkBack).
 *
 * While asuka dev's error overlay shows (DevError.kt), it covers the
 * app: the commands read and press its views (dump: its title, message,
 * source, details and buttons; tap Dismiss; back), not the app's.
 *
 * Only in a debuggable app.
 */
object AsukaAutomation {
    private var receiver: BroadcastReceiver? = null

    fun start(rt: AsukaRuntime) {
        val app = rt.app
        if (app.applicationInfo.flags and ApplicationInfo.FLAG_DEBUGGABLE == 0 || receiver != null) return
        val r = object : BroadcastReceiver() {
            override fun onReceive(c: Context, i: Intent) {
                for (line in i.getStringExtra("commands")?.lines() ?: emptyList()) run(rt, line)
            }
        }
        receiver = r
        val f = IntentFilter("dev.asuka.automation")
        androidx.core.content.ContextCompat.registerReceiver(app, r, f, androidx.core.content.ContextCompat.RECEIVER_EXPORTED)
    }

    private fun run(rt: AsukaRuntime, line: String) {
        val cmd = line.substringBefore(' ')
        val arg = line.substringAfter(' ', "")
        when (cmd) {
            "tap" -> tap(rt, arg)
            "type" -> field(rt)?.apply { requestFocus(); setText(arg); setSelection(arg.length) }
            "submit" -> field(rt)?.onEditorAction(android.view.inputmethod.EditorInfo.IME_ACTION_DONE)
            "dump" -> dump(rt)
            "fits" -> fits(rt)
            "a11y" -> a11y(rt)
            "states" -> states(rt)
            "rotate" -> {
                val deg = arg.substringBefore(' ').toFloatOrNull() ?: return
                shown(rt).firstOrNull { textOf(it) == arg.substringAfter(' ') }?.let { rotate(it, deg) }
            }
            "back" -> if (!rt.devErrors.back()) rt.activity?.onBackPressedDispatcher?.onBackPressed()
            "tab" -> rt.activity?.window?.decorView?.let { tab(it, arg) }
        }
    }

    /** The views shown, in creation order; asuka dev's error overlay's alone while it covers the app's. */
    private fun shown(rt: AsukaRuntime): List<View> =
        rt.devErrors.shownViews() ?: rt.nodes.values.sortedBy { it.id }.map { it.view }.filter { it.isShown && it.alpha > 0f }

    private fun textOf(v: View): String? {
        val t = (v as? TextView)?.text?.toString()
        return if (!t.isNullOrEmpty()) t else v.contentDescription?.toString()
    }

    private fun field(rt: AsukaRuntime): android.widget.EditText? {
        val fields = shown(rt).filterIsInstance<android.widget.EditText>()
        return fields.firstOrNull { it.isFocused } ?: fields.firstOrNull()
    }

    /** Two fingers on v, a quarter of its width from its center, turning by degrees in 10 steps. */
    private fun rotate(v: View, degrees: Float) {
        val cx = v.width / 2f
        val cy = v.height / 2f
        val r = v.width / 4f
        val t0 = android.os.SystemClock.uptimeMillis()
        val props = Array(2) { android.view.MotionEvent.PointerProperties().apply { id = it; toolType = android.view.MotionEvent.TOOL_TYPE_FINGER } }
        fun coords(a: Double) = Array(2) { i ->
            val b = a + i * Math.PI
            android.view.MotionEvent.PointerCoords().apply { x = cx + r * Math.cos(b).toFloat(); y = cy + r * Math.sin(b).toFloat(); pressure = 1f; size = 1f }
        }
        fun send(action: Int, n: Int, a: Double, at: Long) {
            val e = android.view.MotionEvent.obtain(t0, t0 + at, action, n, props, coords(a), 0, 0, 1f, 1f, 0, 0, 0, 0)
            v.dispatchTouchEvent(e)
            e.recycle()
        }
        val second = android.view.MotionEvent.ACTION_POINTER_DOWN or (1 shl android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT)
        val secondUp = android.view.MotionEvent.ACTION_POINTER_UP or (1 shl android.view.MotionEvent.ACTION_POINTER_INDEX_SHIFT)
        send(android.view.MotionEvent.ACTION_DOWN, 1, 0.0, 0)
        send(second, 2, 0.0, 10)
        for (s in 1..10) send(android.view.MotionEvent.ACTION_MOVE, 2, Math.toRadians(degrees * s / 10.0), 10L + s * 16)
        send(secondUp, 2, Math.toRadians(degrees.toDouble()), 200)
        send(android.view.MotionEvent.ACTION_UP, 1, Math.toRadians(degrees.toDouble()), 210)
    }

    private fun tap(rt: AsukaRuntime, text: String) {
        var v: View? = shown(rt).firstOrNull { textOf(it) == text } ?: return
        // A label inside a pressable view: the view.
        while (v != null && !v.isClickable && v !is CompoundButton) v = v.parent as? View
        v?.performClick()
    }

    private fun tab(v: View, title: String): Boolean {
        if (v is com.google.android.material.navigation.NavigationBarView) {
            val m = v.menu
            for (i in 0 until m.size()) if (m.getItem(i).title == title) { v.selectedItemId = m.getItem(i).itemId; return true }
        }
        if (v is android.view.ViewGroup) for (i in 0 until v.childCount) if (tab(v.getChildAt(i), title)) return true
        return false
    }

    private fun dump(rt: AsukaRuntime) = write(rt, "asuka-dump.txt", shown(rt).mapNotNull { v -> textOf(v)?.takeIf { it.isNotEmpty() } })

    /**
     * Whether each one-line text shown in the window (a Picker's field
     * too) fits its view, ends in an ellipsis, or is cut off: text, a tab,
     * then fits, ellipsized or cut.
     */
    private fun fits(rt: AsukaRuntime) {
        val lines = mutableListOf<String>()
        fun walk(v: View) {
            if (!v.isShown) return
            if (v is TextView && v.lineCount == 1 && v.text.isNotEmpty()) {
                val l = v.layout
                val room = v.width - v.totalPaddingLeft - v.totalPaddingRight
                lines += v.text.toString() + "\t" + when {
                    l != null && l.getEllipsisCount(0) > 0 -> "ellipsized"
                    v.paint.measureText(v.text, 0, v.text.length) <= room + 1 -> "fits"
                    else -> "cut"
                }
            }
            if (v is android.view.ViewGroup) for (i in 0 until v.childCount) walk(v.getChildAt(i))
        }
        rt.activity?.window?.decorView?.let(::walk)
        write(rt, "asuka-fits.txt", lines)
    }

    /** What TalkBack is told about each view: its class, hint and content description. */
    private fun a11y(rt: AsukaRuntime) = write(rt, "asuka-a11y.txt", shown(rt).map { v ->
        val i = androidx.core.view.accessibility.AccessibilityNodeInfoCompat.wrap(v.createAccessibilityNodeInfo())
        "${i.className}\t${i.hintText ?: ""}\t${i.contentDescription ?: ""}"
    })

    /**
     * The states TalkBack is told of (RFC 0010's: checkable and checked,
     * selected, expanded, a value, a range, labelled by, a dialog's title)
     * and the focus, for the views that have one: its text or content
     * description, a tab, then name=value fields.
     */
    private fun states(rt: AsukaRuntime) = write(rt, "asuka-states.txt", rt.nodes.values.sortedBy { it.id }.filter { it.view.isShown }.mapNotNull { n ->
        val v = n.view
        val i = androidx.core.view.accessibility.AccessibilityNodeInfoCompat.wrap(v.createAccessibilityNodeInfo())
        val f = ArrayList<String>()
        f += "class=${i.className}"
        if (i.isCheckable) f += "checked=" + (if (android.os.Build.VERSION.SDK_INT >= 36) when (i.unwrap().checked) {
            android.view.accessibility.AccessibilityNodeInfo.CHECKED_STATE_TRUE -> "true"
            android.view.accessibility.AccessibilityNodeInfo.CHECKED_STATE_PARTIAL -> "mixed"
            else -> "false"
        } else i.isChecked.toString())
        if (i.isSelected) f += "selected=true"
        if (i.actionList.any { it.id == android.view.accessibility.AccessibilityNodeInfo.ACTION_EXPAND }) f += "expanded=false"
        if (i.actionList.any { it.id == android.view.accessibility.AccessibilityNodeInfo.ACTION_COLLAPSE }) f += "expanded=true"
        i.stateDescription?.let { f += "value=$it" }
        i.rangeInfo?.let { f += "range=${it.min},${it.max},${it.current}" }
        // The relation itself is read only by a screen reader (a sealed node): the view it names.
        (n.props[AsukaProp.A11Y_LABELLED_BY] as? Long)?.let { rt.nodes[it.toInt()]?.view }?.let { f += "labelledBy=" + (a11yTextOf(it) ?: "") }
        i.hintText?.let { f += "hint=$it" }
        i.paneTitle?.let { f += "pane=$it" }
        if (i.isHeading) f += "heading=true"
        if (v.isFocused) f += "focused=" + (if (v.isInTouchMode) "touch" else "keyboard")
        if (hiddenFromScreenReaders(v)) f += "hidden=true"
        // Before Android 8 there's no default focus highlight: only the container's ring.
        val highlight = android.os.Build.VERSION.SDK_INT < android.os.Build.VERSION_CODES.O || v.defaultFocusHighlightEnabled
        if (!highlight || (v as? AsukaContainer)?.noFocusRing == true) f += "focusRing=none"
        if (f.size == 1) null // its class alone: nothing of RFC 0010's
        else (textOf(v) ?: a11yTextOf(v)?.toString() ?: "") + "\t" + f.joinToString(" ")
    })

    /** Whether a view around v hides it from TalkBack (importantForAccessibility noHideDescendants: a modal's). */
    private fun hiddenFromScreenReaders(v: View): Boolean {
        var x: Any? = v
        while (x is View) {
            if (x.importantForAccessibility == View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS) return true
            x = x.parent
        }
        return false
    }

    /** Lines to a file in the app's files, written whole, then moved: a reader never sees half of it. */
    private fun write(rt: AsukaRuntime, name: String, lines: List<String>) {
        val tmp = File(rt.app.filesDir, "$name.part")
        tmp.writeText(lines.joinToString("\n") + "\n")
        tmp.renameTo(File(rt.app.filesDir, name))
    }
}
