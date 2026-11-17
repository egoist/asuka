package dev.asuka

import android.os.Build
import android.os.Bundle
import android.view.View
import android.view.ViewGroup
import android.view.accessibility.AccessibilityEvent
import android.view.accessibility.AccessibilityNodeInfo
import android.view.accessibility.AccessibilityNodeProvider
import android.widget.TextView
import androidx.core.view.accessibility.AccessibilityNodeInfoCompat
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.IdentityHashMap

/*
 * What TalkBack is told about a view (RFC 0010): its role, as the class
 * name of the Android widget that has it, and its states (checked,
 * expanded, selected, a value, a range) and relations (labelled by,
 * described by), read from the node's props each time TalkBack asks, so a
 * relation to a view made later, or a state that changed, is current.
 * One delegate per view (AsukaA11yDelegate), over the view's own (a
 * Slider's virtual views, a compat delegate's actions), which it asks
 * first.
 */

/** The Android widget class TalkBack reads a role as, or null: the view's own. */
fun a11yClassName(role: Int): String? = when (role) {
    AsukaRole.BUTTON, AsukaRole.LINK -> "android.widget.Button"
    AsukaRole.IMAGE -> "android.widget.ImageView"
    AsukaRole.TEXT -> "android.widget.TextView"
    AsukaRole.SEARCH -> "android.widget.EditText"
    AsukaRole.CHECKBOX, AsukaRole.MENU_ITEM_CHECKBOX -> "android.widget.CheckBox"
    AsukaRole.RADIO, AsukaRole.MENU_ITEM_RADIO -> "android.widget.RadioButton"
    AsukaRole.RADIO_GROUP -> "android.widget.RadioGroup"
    AsukaRole.SWITCH -> "android.widget.Switch"
    // Material's TabLayout tells TalkBack its tabs are ActionBar.Tab's.
    AsukaRole.TAB -> "android.app.ActionBar\$Tab"
    AsukaRole.TAB_LIST -> "android.widget.TabWidget"
    AsukaRole.SLIDER, AsukaRole.SPIN_BUTTON -> "android.widget.SeekBar"
    AsukaRole.PROGRESS_BAR, AsukaRole.METER -> "android.widget.ProgressBar"
    AsukaRole.MENU, AsukaRole.LISTBOX -> "android.widget.ListView"
    AsukaRole.COMBO_BOX -> "android.widget.Spinner"
    AsukaRole.TOOLBAR, AsukaRole.MENU_BAR -> "android.widget.Toolbar"
    else -> null
}

/** Whether a role is checkable: None as its A11yChecked reads as unchecked. */
fun a11yCheckable(role: Int) = when (role) {
    AsukaRole.CHECKBOX, AsukaRole.RADIO, AsukaRole.SWITCH, AsukaRole.MENU_ITEM_CHECKBOX, AsukaRole.MENU_ITEM_RADIO -> true
    else -> false
}

/** A11yRange (ops.DecodeRange): f64 min, max, now, little endian; null when empty or short. */
fun decodeA11yRange(b: ByteBuffer): DoubleArray? {
    val r = b.duplicate().order(ByteOrder.LITTLE_ENDIAN)
    if (r.remaining() < 24) return null
    return doubleArrayOf(r.double, r.double, r.double)
}

/** The props the delegate reads: setting one installs it. */
private val a11yProps = intArrayOf(
    AsukaProp.A11Y_ROLE, AsukaProp.A11Y_ADJUSTABLE, AsukaProp.A11Y_CHECKED, AsukaProp.A11Y_EXPANDED,
    AsukaProp.A11Y_SELECTED, AsukaProp.A11Y_VALUE, AsukaProp.A11Y_RANGE, AsukaProp.A11Y_CONTROLS,
    AsukaProp.A11Y_LABELLED_BY, AsukaProp.A11Y_DESCRIBED_BY,
)

/** Whether n has a prop its delegate reads. */
fun hasA11yProps(n: AsukaNode) = a11yProps.any { n.props.containsKey(it) }

/** What a view says: its text, or content description, or its descendants' texts. */
fun a11yTextOf(v: View): CharSequence? {
    v.contentDescription?.takeIf { it.isNotEmpty() }?.let { return it }
    (v as? TextView)?.text?.takeIf { it.isNotEmpty() }?.let { return it }
    if (v is ViewGroup) {
        val parts = ArrayList<CharSequence>()
        for (i in 0 until v.childCount) {
            val c = v.getChildAt(i)
            if (c.visibility != View.VISIBLE) continue
            a11yTextOf(c)?.let { parts.add(it) }
        }
        if (parts.isNotEmpty()) return parts.joinToString(" ")
    }
    return null
}

/**
 * A view's accessibility delegate: what the view's own (orig) says, then
 * its node's role, states and relations over it.
 */
class AsukaA11yDelegate(private val rt: AsukaRuntime, val id: Int, val orig: View.AccessibilityDelegate?) : View.AccessibilityDelegate() {
    override fun onInitializeAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfo) {
        if (orig != null) orig.onInitializeAccessibilityNodeInfo(host, info) else super.onInitializeAccessibilityNodeInfo(host, info)
        val n = rt.nodes[id] ?: return
        rt.describe(n, host, AccessibilityNodeInfoCompat.wrap(info))
    }

    override fun performAccessibilityAction(host: View, action: Int, args: Bundle?): Boolean {
        val n = rt.nodes[id]
        if (n != null) {
            if (n.props[AsukaProp.A11Y_ADJUSTABLE] == true) {
                val by = when (action) {
                    AccessibilityNodeInfo.ACTION_SCROLL_FORWARD -> 1L
                    AccessibilityNodeInfo.ACTION_SCROLL_BACKWARD -> -1L
                    else -> 0L
                }
                if (by != 0L) {
                    rt.emit(id, AsukaEventKind.ADJUST) { AsukaEvents.adjust(id, by) }
                    return true
                }
            }
            // Expanding or collapsing it is pressing it: what it controls shows or goes.
            if ((n.props[AsukaProp.A11Y_EXPANDED] as? Int ?: 0) != AsukaA11yExpanded.NONE &&
                (action == AccessibilityNodeInfo.ACTION_EXPAND || action == AccessibilityNodeInfo.ACTION_COLLAPSE)) {
                return host.performClick()
            }
        }
        return orig?.performAccessibilityAction(host, action, args) ?: super.performAccessibilityAction(host, action, args)
    }

    override fun getAccessibilityNodeProvider(host: View): AccessibilityNodeProvider? =
        orig?.getAccessibilityNodeProvider(host) ?: super.getAccessibilityNodeProvider(host)

    override fun sendAccessibilityEvent(host: View, eventType: Int) =
        orig?.sendAccessibilityEvent(host, eventType) ?: super.sendAccessibilityEvent(host, eventType)

    override fun sendAccessibilityEventUnchecked(host: View, event: AccessibilityEvent) =
        orig?.sendAccessibilityEventUnchecked(host, event) ?: super.sendAccessibilityEventUnchecked(host, event)

    override fun dispatchPopulateAccessibilityEvent(host: View, event: AccessibilityEvent): Boolean =
        orig?.dispatchPopulateAccessibilityEvent(host, event) ?: super.dispatchPopulateAccessibilityEvent(host, event)

    override fun onPopulateAccessibilityEvent(host: View, event: AccessibilityEvent) =
        orig?.onPopulateAccessibilityEvent(host, event) ?: super.onPopulateAccessibilityEvent(host, event)

    override fun onInitializeAccessibilityEvent(host: View, event: AccessibilityEvent) =
        orig?.onInitializeAccessibilityEvent(host, event) ?: super.onInitializeAccessibilityEvent(host, event)

    override fun onRequestSendAccessibilityEvent(host: ViewGroup, child: View, event: AccessibilityEvent): Boolean =
        orig?.onRequestSendAccessibilityEvent(host, child, event) ?: super.onRequestSendAccessibilityEvent(host, child, event)

    override fun addExtraDataToAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfo, extraDataKey: String, arguments: Bundle?) =
        orig?.addExtraDataToAccessibilityNodeInfo(host, info, extraDataKey, arguments)
            ?: super.addExtraDataToAccessibilityNodeInfo(host, info, extraDataKey, arguments)
}

/** Puts n's view under its delegate (once: it reads the props when asked). */
fun AsukaRuntime.installA11y(n: AsukaNode) {
    val v = n.view
    if (v is AsukaRichText) return // its own (a heading's, its links')
    val cur = v.accessibilityDelegate
    if (cur is AsukaA11yDelegate && cur.id == n.id) return
    v.accessibilityDelegate = AsukaA11yDelegate(this, n.id, (cur as? AsukaA11yDelegate)?.orig ?: cur)
}

/** Tells TalkBack, if it's on, that n's states changed. */
fun AsukaRuntime.a11yChanged(n: AsukaNode) {
    val v = n.view
    if (!v.isAttachedToWindow) return
    val am = app.getSystemService(android.content.Context.ACCESSIBILITY_SERVICE) as android.view.accessibility.AccessibilityManager
    if (!am.isEnabled) return
    val e = AccessibilityEvent(AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED)
    e.contentChangeTypes = AccessibilityEvent.CONTENT_CHANGE_TYPE_STATE_DESCRIPTION
    v.sendAccessibilityEventUnchecked(e)
}

/** n's role, states and relations, over what its view says. */
fun AsukaRuntime.describe(n: AsukaNode, host: View, info: AccessibilityNodeInfoCompat) {
    val p = n.props
    val role = p[AsukaProp.A11Y_ROLE] as? Int ?: AsukaRole.NONE
    val adjustable = p[AsukaProp.A11Y_ADJUSTABLE] == true
    a11yClassName(role)?.let { info.className = it }
    if (role == AsukaRole.HEADER) info.isHeading = true
    if (adjustable) {
        // A seek bar to TalkBack, whose swipe up and down (and the volume keys) step it.
        if (role == AsukaRole.NONE) info.className = "android.widget.SeekBar"
        info.addAction(AccessibilityNodeInfoCompat.AccessibilityActionCompat.ACTION_SCROLL_FORWARD)
        info.addAction(AccessibilityNodeInfoCompat.AccessibilityActionCompat.ACTION_SCROLL_BACKWARD)
    }
    val checked = p[AsukaProp.A11Y_CHECKED] as? Int ?: AsukaA11yChecked.NONE
    if (checked != AsukaA11yChecked.NONE || a11yCheckable(role)) {
        info.isCheckable = true
        if (Build.VERSION.SDK_INT >= 36) info.unwrap().setChecked(when (checked) {
            AsukaA11yChecked.CHECKED -> AccessibilityNodeInfo.CHECKED_STATE_TRUE
            AsukaA11yChecked.MIXED -> AccessibilityNodeInfo.CHECKED_STATE_PARTIAL
            else -> AccessibilityNodeInfo.CHECKED_STATE_FALSE
        }) else info.isChecked = checked == AsukaA11yChecked.CHECKED
    }
    when (p[AsukaProp.A11Y_EXPANDED] as? Int ?: AsukaA11yExpanded.NONE) {
        AsukaA11yExpanded.COLLAPSED -> {
            if (Build.VERSION.SDK_INT >= 36) info.unwrap().expandedState = AccessibilityNodeInfo.EXPANDED_STATE_COLLAPSED
            info.addAction(AccessibilityNodeInfoCompat.AccessibilityActionCompat.ACTION_EXPAND)
        }
        AsukaA11yExpanded.EXPANDED -> {
            if (Build.VERSION.SDK_INT >= 36) info.unwrap().expandedState = AccessibilityNodeInfo.EXPANDED_STATE_FULL
            info.addAction(AccessibilityNodeInfoCompat.AccessibilityActionCompat.ACTION_COLLAPSE)
        }
    }
    if (p[AsukaProp.A11Y_SELECTED] == true) info.isSelected = true
    (p[AsukaProp.A11Y_RANGE] as? DoubleArray)?.let { r ->
        info.rangeInfo = AccessibilityNodeInfoCompat.RangeInfoCompat.obtain(
            AccessibilityNodeInfoCompat.RangeInfoCompat.RANGE_TYPE_FLOAT, r[0].toFloat(), r[1].toFloat(), r[2].toFloat())
    }
    val value = p[AsukaProp.A11Y_VALUE] as? String ?: ""
    val hint = p[AsukaProp.A11Y_HINT] as? String ?: ""
    if (value.isNotEmpty()) {
        info.stateDescription = value
        if (hint.isNotEmpty()) info.hintText = hint // the state description had it
    }
    // Labelled by: the platform's relation, and its text as the label
    // where the view has none of its own.
    val label = p[AsukaProp.A11Y_LABEL] as? String ?: ""
    (p[AsukaProp.A11Y_LABELLED_BY] as? Long)?.takeIf { it != 0L }?.let { nodes[it.toInt()]?.view }?.let { by ->
        info.setLabeledBy(by)
        if (label.isEmpty() && info.contentDescription.isNullOrEmpty() && info.text.isNullOrEmpty()) a11yTextOf(by)?.let { info.contentDescription = it }
    }
    // Described by: Android has no such relation: its text as the hint.
    (p[AsukaProp.A11Y_DESCRIBED_BY] as? Long)?.takeIf { it != 0L }?.let { nodes[it.toInt()]?.view }?.let { by ->
        a11yTextOf(by)?.let { info.hintText = it }
    }
    // A dialog announces itself by its title as it shows.
    if (role == AsukaRole.DIALOG || role == AsukaRole.ALERT_DIALOG) {
        val title = label.ifEmpty { (p[AsukaProp.A11Y_LABELLED_BY] as? Long)?.let { nodes[it.toInt()]?.view }?.let { a11yTextOf(it)?.toString() } ?: "" }
        if (title.isNotEmpty()) info.paneTitle = title
    }
}

/** A11yRole: what's on the view itself (a status's live region, a heading's text), and its delegate. */
fun AsukaRuntime.setA11yRole(n: AsukaNode, role: Int) {
    val v = n.view
    if (v is AsukaRichText) { v.heading = role == AsukaRole.HEADER; return }
    v.accessibilityLiveRegion = when (role) {
        AsukaRole.STATUS -> View.ACCESSIBILITY_LIVE_REGION_POLITE
        AsukaRole.ALERT_DIALOG -> View.ACCESSIBILITY_LIVE_REGION_ASSERTIVE
        else -> View.ACCESSIBILITY_LIVE_REGION_NONE
    }
    installA11y(n)
    a11yChanged(n)
}

// ---- Modal: the rest of the window hidden from screen readers ----

/** The views a modal hid, and what they were (importantForAccessibility). */
private val modalHidden = IdentityHashMap<View, Int>()

/**
 * Hides from TalkBack what's around each modal view (A11yModal, a Modal
 * Overlay): every view beside it and beside each view around it, up to
 * its window's root. Done again from the start whenever one comes, goes
 * or changes: the overlays from the bottom up, then the views, each
 * showing again what a modal under it hid of it.
 */
fun AsukaRuntime.refreshModals() {
    for ((v, was) in modalHidden) v.importantForAccessibility = was
    modalHidden.clear()
    val modals = ArrayList<AsukaNode>()
    for (n in overlays) if (n.props[AsukaProp.Overlay.MODAL] == true && (n.view as? ViewGroup)?.childCount != 0) modals.add(n)
    for (n in nodes.values) if (n.props[AsukaProp.A11Y_MODAL] == true) modals.add(n)
    for (n in modals) {
        var v: View = n.outer
        if (!v.isAttachedToWindow || v.visibility != View.VISIBLE) continue
        while (true) {
            modalHidden.remove(v)?.let { v.importantForAccessibility = it }
            val parent = v.parent as? ViewGroup ?: break
            for (i in 0 until parent.childCount) {
                val s = parent.getChildAt(i)
                if (s === v || modalHidden.containsKey(s)) continue
                if (s.importantForAccessibility == View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS) continue
                modalHidden[s] = s.importantForAccessibility
                s.importantForAccessibility = View.IMPORTANT_FOR_ACCESSIBILITY_NO_HIDE_DESCENDANTS
            }
            v = parent
        }
    }
}

/** What a view about to be hidden from TalkBack by A11yHidden was before a modal hid it (A11yHidden off restores it). */
fun modalRestore(v: View, value: Int) {
    if (modalHidden.containsKey(v)) modalHidden[v] = value else v.importantForAccessibility = value
}
