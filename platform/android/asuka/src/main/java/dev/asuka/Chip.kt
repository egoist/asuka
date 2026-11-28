package dev.asuka

import android.annotation.SuppressLint
import android.content.res.ColorStateList
import android.view.ContextThemeWrapper
import android.view.KeyEvent
import android.view.View
import android.view.ViewGroup
import com.google.android.material.chip.Chip

/** Which of Material 3's chips a Chip's props make: its style, fixed when the view is made. */
object AsukaChipStyle {
    const val ASSIST = 0 // an action, with an icon
    const val SUGGESTION = 1 // an action, without
    const val FILTER = 2 // a choice: checkable, a checkmark while selected
    const val INPUT = 3 // an entered item: a close icon at its end

    fun of(filter: Boolean, removable: Boolean, icon: Boolean) = when {
        filter -> FILTER
        removable -> INPUT
        icon -> ASSIST
        else -> SUGGESTION
    }

    fun overlay(style: Int) = when (style) {
        FILTER -> R.style.ThemeOverlay_Asuka_Chip_Filter
        INPUT -> R.style.ThemeOverlay_Asuka_Chip_Input
        SUGGESTION -> R.style.ThemeOverlay_Asuka_Chip_Suggestion
        else -> R.style.ThemeOverlay_Asuka_Chip_Assist
    }
}

/**
 * A Chip: Material 3's, in the style its props make (AsukaChipStyle): an
 * assist chip (an Icon) or a suggestion chip (none) pressed reports
 * Press; a filter chip toggles, shows its checkmark in place of its icon
 * while selected and reports Toggled; an input chip's close icon (named
 * by Material's own, localized "Remove …"), Delete or Backspace while
 * it's focused, and TalkBack's action on the close icon report Remove.
 * Focusable for a D-pad (Android TV) and a keyboard.
 */
@SuppressLint("ViewConstructor")
class AsukaChip(private val rt: AsukaRuntime, private val asukaID: Int, val style: Int) :
    Chip(ContextThemeWrapper(rt.themed, AsukaChipStyle.overlay(style))) {
    private var setting = false
    var iconName = ""
        private set
    private var tint = 0L
    // The icon's color, Material 3's: an assist chip's the primary color,
    // the others' onSurfaceVariant. Set on the chip, not left to the style:
    // a chip whose icon comes later draws it in the drawable's own black.
    private val iconColor = ColorStateList.valueOf(com.google.android.material.color.MaterialColors.getColor(context,
        if (style == AsukaChipStyle.ASSIST) androidx.appcompat.R.attr.colorPrimary else com.google.android.material.R.attr.colorOnSurfaceVariant,
        android.graphics.Color.GRAY))

    init {
        isCheckable = style == AsukaChipStyle.FILTER
        isCloseIconVisible = style == AsukaChipStyle.INPUT
        isFocusable = true
        setEnsureMinTouchTargetSize(true)
        setOnCheckedChangeListener { _, on ->
            showIcon()
            if (!setting && !rt.settingProp) rt.emit(asukaID, AsukaEventKind.TOGGLED) { AsukaEvents.toggled(asukaID, on) }
        }
        setOnClickListener { if (!isCheckable) rt.clicked(asukaID) }
        setOnCloseIconClickListener { remove() }
    }

    private fun remove() = rt.emit(asukaID, AsukaEventKind.REMOVE) { AsukaEvents.remove(asukaID) }

    fun select(on: Boolean) {
        setting = true
        isChecked = on
        setting = false
        showIcon()
    }

    fun setIcon(name: String) {
        iconName = name
        chipIcon = if (name.isEmpty()) null else rt.symbol(name)
        applyTint()
        showIcon()
    }

    /** The icon, unless a selected filter chip's checkmark takes its place (Material's own draws both, over each other). */
    private fun showIcon() {
        isChipIconVisible = chipIcon != null && !(isCheckable && isChecked)
    }

    /** Color: the icons' tint, and a selected filter chip's container (Material's secondary container otherwise). */
    fun setTint(c: Long) {
        tint = c
        applyTint()
    }

    private fun applyTint() {
        if (tint == 0L) {
            chipIconTint = iconColor
            return
        }
        val c = rt.color(tint)
        chipIconTint = ColorStateList.valueOf(c)
        checkedIconTint = ColorStateList.valueOf(c)
        if (isCheckable) {
            // Unselected, Material's filter chip is clear inside its outline.
            chipBackgroundColor = ColorStateList(arrayOf(intArrayOf(android.R.attr.state_checked), intArrayOf()),
                intArrayOf(androidx.core.graphics.ColorUtils.setAlphaComponent(c, 0x3d), android.graphics.Color.TRANSPARENT))
        }
    }

    /** Delete and Backspace remove an input chip, as a keyboard's do on iOS and the desktops. */
    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (isCloseIconVisible && isEnabled && (keyCode == KeyEvent.KEYCODE_DEL || keyCode == KeyEvent.KEYCODE_FORWARD_DEL) && event.hasNoModifiers()) {
            remove()
            return true
        }
        return super.onKeyDown(keyCode, event)
    }
}

/** Sets a Chip's prop; false if prop isn't one of its own. */
fun AsukaRuntime.setChipProp(n: AsukaNode, prop: Int, v: Any): Boolean {
    val c = n.view as? AsukaChip ?: return false
    when (prop) {
        AsukaProp.Chip.TITLE -> c.text = v as String
        AsukaProp.Chip.ICON -> { if (!restyleChip(n)) c.setIcon(v as String) }
        AsukaProp.Chip.FILTER, AsukaProp.Chip.REMOVABLE -> restyleChip(n)
        AsukaProp.Chip.SELECTED -> c.select(v as Boolean)
        else -> return false
    }
    return true
}

/** A Chip's view in the style its props make now: made again (keeping what it shows) if that changed. True if it was. */
fun AsukaRuntime.restyleChip(n: AsukaNode, force: Boolean = false): Boolean {
    val old = n.view as? AsukaChip ?: return false
    val icon = n.props[AsukaProp.Chip.ICON] as? String ?: ""
    val style = AsukaChipStyle.of(n.props[AsukaProp.Chip.FILTER] == true, n.props[AsukaProp.Chip.REMOVABLE] == true, icon.isNotEmpty())
    if (style == old.style && !force) return false
    val c = AsukaChip(this, n.id, style)
    c.layoutDirection = directionOf(n)
    carryViewProps(n, old, c)
    c.text = old.text
    c.setIcon(icon)
    c.select(n.props[AsukaProp.Chip.SELECTED] == true)
    c.setTint(n.color)
    val parent = old.parent as? ViewGroup
    if (parent != null) {
        val i = parent.indexOfChild(old)
        val lp = old.layoutParams
        val focused = old.hasFocus()
        parent.removeViewAt(i)
        parent.addView(c, i, lp)
        c.measure(View.MeasureSpec.makeMeasureSpec(old.width, View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(old.height, View.MeasureSpec.EXACTLY))
        c.layout(old.left, old.top, old.right, old.bottom)
        if (focused) c.requestFocus()
    } else old.layoutParams?.let { c.layoutParams = it }
    n.view = c
    restyle(n)
    return true
}
