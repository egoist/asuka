package dev.asuka

import android.annotation.SuppressLint
import android.content.res.ColorStateList
import android.graphics.Color
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.TextView
import androidx.core.view.AccessibilityDelegateCompat
import androidx.core.view.ViewCompat
import androidx.core.view.accessibility.AccessibilityNodeInfoCompat
import com.google.android.material.color.MaterialColors

/**
 * A Disclosure's header: a Material list item's row (48dp at least,
 * its ripple), the title (bodyLarge, onSurface) at the start and the
 * expand icon at the end (onSurfaceVariant), which turns 180° as it
 * expands and back as it collapses. The content under it is the core's
 * (its sibling views): pressing the row (a D-pad's center, Enter or
 * Space when focused) flips Expanded at once and reports Toggled.
 * TalkBack reads a button with its expand or collapse action, as an
 * expandable list item.
 */
@SuppressLint("ViewConstructor")
class AsukaDisclosure(private val rt: AsukaRuntime, private val asukaID: Int) : FrameLayout(rt.themed) {
    val title = TextView(rt.themed)
    private val icon = ImageView(rt.themed)
    var expanded = false
        private set

    init {
        val d = rt.density
        minimumHeight = Math.round(48 * d)
        isClickable = true
        isFocusable = true
        val tv = TypedValue()
        rt.themed.theme.resolveAttribute(android.R.attr.selectableItemBackground, tv, true)
        background = androidx.core.content.ContextCompat.getDrawable(rt.themed, tv.resourceId)
        val iconSize = Math.round(24 * d)
        title.textAlignment = View.TEXT_ALIGNMENT_VIEW_START
        addView(title, LayoutParams(LayoutParams.WRAP_CONTENT, LayoutParams.WRAP_CONTENT, Gravity.START or Gravity.CENTER_VERTICAL).apply {
            marginEnd = iconSize + Math.round(16 * d)
            topMargin = Math.round(8 * d)
            bottomMargin = Math.round(8 * d)
        })
        icon.setImageDrawable(rt.symbol("chevron.down")) // Material's expand_more
        icon.importantForAccessibility = IMPORTANT_FOR_ACCESSIBILITY_NO
        addView(icon, LayoutParams(iconSize, iconSize, Gravity.END or Gravity.CENTER_VERTICAL))
        restyle()
        setOnClickListener { toggle() }
        ViewCompat.setAccessibilityDelegate(this, object : AccessibilityDelegateCompat() {
            override fun onInitializeAccessibilityNodeInfo(host: View, info: AccessibilityNodeInfoCompat) {
                super.onInitializeAccessibilityNodeInfo(host, info)
                info.className = "android.widget.Button"
                if (host.contentDescription.isNullOrEmpty()) info.text = title.text // what it's named: its title
                info.addAction(if (expanded) AccessibilityNodeInfoCompat.AccessibilityActionCompat.ACTION_COLLAPSE
                    else AccessibilityNodeInfoCompat.AccessibilityActionCompat.ACTION_EXPAND)
            }

            override fun performAccessibilityAction(host: View, action: Int, args: android.os.Bundle?): Boolean {
                if (action == AccessibilityNodeInfoCompat.ACTION_EXPAND && !expanded ||
                    action == AccessibilityNodeInfoCompat.ACTION_COLLAPSE && expanded) {
                    toggle()
                    return true
                }
                return super.performAccessibilityAction(host, action, args)
            }
        })
    }

    /** The theme's colors and the title's type (made again after a dark mode or font scale change). */
    fun restyle() {
        rt.applyTextStyle(title, AsukaTextStyle.BODY)
        title.setTextColor(MaterialColors.getColor(rt.themed, com.google.android.material.R.attr.colorOnSurface, Color.BLACK))
        icon.imageTintList = ColorStateList.valueOf(MaterialColors.getColor(rt.themed, com.google.android.material.R.attr.colorOnSurfaceVariant, Color.GRAY))
    }

    /** Color: the expand icon's tint. */
    fun setTint(c: Long) {
        if (c != 0L) icon.imageTintList = ColorStateList.valueOf(rt.color(c))
    }

    private fun toggle() {
        if (!isEnabled) return
        setExpanded(!expanded, true)
        val on = expanded
        rt.emit(asukaID, AsukaEventKind.TOGGLED) { AsukaEvents.toggled(asukaID, on) }
    }

    fun setExpanded(on: Boolean, animated: Boolean = isAttachedToWindow) {
        if (on == expanded && icon.rotation == (if (on) 180f else 0f)) return
        expanded = on
        val to = if (on) 180f else 0f
        icon.animate().cancel()
        if (animated && isAttachedToWindow) {
            icon.animate().rotation(to).setDuration(resources.getInteger(android.R.integer.config_shortAnimTime).toLong())
                .setInterpolator(android.view.animation.PathInterpolator(0.2f, 0f, 0f, 1f)).start() // Material's standard easing
        } else icon.rotation = to
        stateChanged()
    }

    private fun stateChanged() {
        if (!isAttachedToWindow) return
        val am = context.getSystemService(android.content.Context.ACCESSIBILITY_SERVICE) as android.view.accessibility.AccessibilityManager
        if (am.isEnabled) sendAccessibilityEvent(android.view.accessibility.AccessibilityEvent.TYPE_WINDOW_CONTENT_CHANGED)
    }
}

/** Sets a Disclosure's prop; false if prop isn't one of its own. */
fun setDisclosureProp(n: AsukaNode, prop: Int, v: Any): Boolean {
    val d = n.view as? AsukaDisclosure ?: return false
    when (prop) {
        AsukaProp.Disclosure.TITLE -> { d.title.text = v as String; d.requestLayout() }
        AsukaProp.Disclosure.EXPANDED -> d.setExpanded(v as Boolean)
        else -> return false
    }
    return true
}
