package dev.asuka

import android.annotation.SuppressLint
import android.content.Context
import android.graphics.Color
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.accessibility.AccessibilityNodeInfo
import android.view.inputmethod.EditorInfo
import android.widget.ArrayAdapter
import android.widget.Filter
import androidx.core.view.ViewCompat
import androidx.core.view.accessibility.AccessibilityNodeInfoCompat
import com.google.android.material.color.MaterialColors
import com.google.android.material.shape.MaterialShapeDrawable
import com.google.android.material.shape.ShapeAppearanceModel
import com.google.android.material.textfield.MaterialAutoCompleteTextView
import java.nio.ByteBuffer

/**
 * A suggestion list's adapter: the core's Suggestions, which it filtered
 * by what's typed, shown as they are, in order. Its Filter, which
 * AutoCompleteTextView runs as the user types, lets everything through:
 * what it publishes is the list as it is then (set on the main thread),
 * so a list the core sent while the filter ran isn't undone by it.
 */
class AsukaSuggestAdapter(context: Context, layout: Int) : ArrayAdapter<String>(context, layout) {
    private var items: List<String> = emptyList()

    val suggestions: List<String> get() = items
    /** The field's layout direction: the popup's window has the system's, its rows the app's. */
    var direction = View.LAYOUT_DIRECTION_LTR

    override fun getView(position: Int, convertView: View?, parent: android.view.ViewGroup): View =
        super.getView(position, convertView, parent).also {
            it.layoutDirection = direction
            (it as? android.widget.TextView)?.textAlignment = View.TEXT_ALIGNMENT_VIEW_START
        }

    /** The list, as the core sent it: shown in place if the list shows. */
    fun setItems(list: List<String>) {
        if (list == items) return
        items = list
        notifyDataSetChanged()
    }

    override fun getCount() = items.size
    override fun getItem(position: Int): String? = items.getOrNull(position)
    override fun getPosition(item: String?) = items.indexOf(item)
    override fun getItemId(position: Int) = position.toLong()

    private val all = object : Filter() {
        // On the filter's thread: nothing to work out.
        override fun performFiltering(constraint: CharSequence?) = FilterResults()

        // On the main thread, before AutoCompleteTextView reads the count.
        override fun publishResults(constraint: CharSequence?, results: FilterResults) {
            results.values = items
            results.count = items.size
            notifyDataSetChanged()
        }

        override fun convertResultToString(resultValue: Any?) = resultValue?.toString() ?: ""
    }

    override fun getFilter(): Filter = all
}

/**
 * A TextField with Suggestions: Material's MaterialAutoCompleteTextView,
 * in the look of the host's other fields (the theme's EditText style),
 * its drop-down the core's list (AsukaSuggestAdapter) in Material's menu
 * surface. The list shows once the user has typed since the field took
 * the focus or since a choice (AutoCompleteTextView's threshold, one
 * character), updates as the core's list changes, and hides when it's
 * empty. A choice puts its text in the field, the cursor at its end
 * (reported with Suggestion, not TextChanged). Dropdown: Material's
 * drop-down arrow at the field's end shows all of them, or hides them
 * (Alt+Down too, and TalkBack's "Show dropdown menu" action).
 */
@SuppressLint("ViewConstructor")
class AsukaSuggestField(context: Context, private val rt: AsukaRuntime, private val asukaID: Int) :
    MaterialAutoCompleteTextView(context, null, androidx.appcompat.R.attr.editTextStyle) {
    val selection = AsukaSelectionReport(rt, asukaID, this)
    // AutoCompleteTextView's constructor moves the selection before this
    // class's fields are set: until then there's nothing to report it to.
    private var ready = false
    @SuppressLint("PrivateResource") // Material's own item, as its exposed dropdown menus show
    val suggestAdapter = AsukaSuggestAdapter(context, com.google.android.material.R.layout.m3_auto_complete_simple_item)
    /** A choice's text going in: not reported as typed. */
    var choosing = false
        private set
    /** The user has typed since the focus came or since a choice: the list may show. */
    private var typed = false
    private var arrow: android.graphics.drawable.Drawable? = null
    private var arrowUp = false
    private var showAction = 0
    var dropdown = false
        set(v) {
            if (field == v) return
            field = v
            setArrow()
        }

    init {
        threshold = 1
        setAdapter(suggestAdapter)
        setDropDownBackgroundDrawable(MaterialShapeDrawable(ShapeAppearanceModel.builder()
            .setAllCornerSizes(4 * rt.density).build()).apply { // Material 3's menu: extra small corners, level 2
            initializeElevationOverlay(context)
            fillColor = android.content.res.ColorStateList.valueOf(MaterialColors.getColor(context, com.google.android.material.R.attr.colorSurfaceContainer, Color.WHITE))
            elevation = 3 * rt.density
        })
        setOnItemClickListener { _, _, position, _ ->
            typed = false
            rt.emit(asukaID, AsukaEventKind.SUGGESTION) { AsukaEvents.suggestion(asukaID, position.toLong()) }
        }
        ready = true
    }

    fun setItems(list: List<String>) {
        suggestAdapter.setItems(list)
        // AutoCompleteTextView updates a list that shows (its data set
        // observer); one that doesn't shows once the user types.
        if (list.isEmpty() && isPopupShowing) dismissDropDown()
    }

    /** The user typed (TextWatcher, not the core's text): the list may show. */
    fun userTyped() { typed = true }

    override fun replaceText(text: CharSequence?) {
        choosing = true
        try { super.replaceText(text) } finally { choosing = false }
    }

    // A choice made with TalkBack (Material's modal list) goes in this way.
    override fun setText(text: CharSequence?, filter: Boolean) {
        if (filter) return super.setText(text, true)
        choosing = true
        try { super.setText(text, false) } finally { choosing = false }
    }

    override fun showDropDown() {
        if (!typed || suggestAdapter.count == 0) return
        suggestAdapter.direction = layoutDirection
        super.showDropDown()
        showArrow()
    }

    override fun dismissDropDown() {
        super.dismissDropDown()
        showArrow()
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        showArrow()
    }

    /** Shows all the Suggestions, as the drop-down arrow does; or hides them. */
    fun toggleList() {
        if (isPopupShowing) { dismissDropDown(); return }
        if (!hasFocus()) requestFocus()
        typed = true
        showDropDown()
    }

    override fun onFocusChanged(focused: Boolean, direction: Int, previouslyFocusedRect: android.graphics.Rect?) {
        typed = false
        super.onFocusChanged(focused, direction, previouslyFocusedRect)
    }

    override fun onSelectionChanged(selStart: Int, selEnd: Int) {
        super.onSelectionChanged(selStart, selEnd)
        if (ready) selection.moved(selStart, selEnd)
    }

    override fun onInitializeAccessibilityNodeInfo(info: AccessibilityNodeInfo) {
        super.onInitializeAccessibilityNodeInfo(info)
        fieldHint(rt, asukaID, info)
    }

    override fun onKeyDown(keyCode: Int, event: KeyEvent): Boolean {
        if (isPopupShowing && keyCode == KeyEvent.KEYCODE_ESCAPE) {
            dismissDropDown()
            return true
        }
        if (!isPopupShowing && dropdown && keyCode == KeyEvent.KEYCODE_DPAD_DOWN && event.isAltPressed) {
            toggleList()
            return true
        }
        // Material's drop-down takes Enter to open its menu (a TextInputLayout's
        // end icon): this field's Enter is its return key, as other fields'.
        if (!isPopupShowing && (keyCode == KeyEvent.KEYCODE_ENTER || keyCode == KeyEvent.KEYCODE_NUMPAD_ENTER) && maxLines == 1) {
            onEditorAction(EditorInfo.IME_NULL)
            return true
        }
        return super.onKeyDown(keyCode, event)
    }

    @SuppressLint("ClickableViewAccessibility") // the arrow's TalkBack action is the view's (setArrow)
    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (dropdown && arrow != null && onArrow(event.x)) {
            if (event.actionMasked == MotionEvent.ACTION_UP && isEnabled) toggleList()
            return true
        }
        return super.onTouchEvent(event)
    }

    /** Whether x (the view's) is on the arrow, at the field's end. */
    private fun onArrow(x: Float): Boolean {
        val room = (arrow?.intrinsicWidth ?: 0) + compoundDrawablePadding + Math.round(12 * rt.density)
        return if (layoutDirection == View.LAYOUT_DIRECTION_RTL) x <= paddingLeft + room else x >= width - paddingRight - room
    }

    /** Material's exposed drop-down arrow, which points up while its list shows. */
    @SuppressLint("PrivateResource") // Material's own icons
    private fun showArrow() {
        if (!dropdown) return
        val up = isPopupShowing
        if (arrow != null && up == arrowUp) return
        arrowUp = up
        val d = androidx.appcompat.content.res.AppCompatResources.getDrawable(context,
            if (up) com.google.android.material.R.drawable.mtrl_ic_arrow_drop_up else com.google.android.material.R.drawable.mtrl_ic_arrow_drop_down)?.mutate()
        d?.setTint(MaterialColors.getColor(this, com.google.android.material.R.attr.colorOnSurfaceVariant, Color.GRAY))
        arrow = d
        setCompoundDrawablesRelativeWithIntrinsicBounds(null, null, d, null)
    }

    @SuppressLint("PrivateResource") // Material's own words for its drop-down arrow, in its languages
    private fun setArrow() {
        if (showAction != 0) {
            ViewCompat.removeAccessibilityAction(this, showAction)
            showAction = 0
        }
        if (!dropdown) {
            arrow = null
            setCompoundDrawablesRelative(null, null, null, null)
            return
        }
        showArrow()
        showAction = ViewCompat.addAccessibilityAction(this,
            context.getString(com.google.android.material.R.string.exposed_dropdown_menu_content_description)) { _, _ ->
            toggleList()
            true
        }
    }
}

/**
 * Reports a field's selection (SelectionChanged) as it moves, after the
 * text change that moved it, unless the core is setting it.
 */
class AsukaSelectionReport(private val rt: AsukaRuntime, private val id: Int, private val v: android.widget.EditText) {
    var setting = false
    private var queued = false
    private var last = -1L

    fun moved(selStart: Int, selEnd: Int) {
        if (setting) {
            last = pack(selStart, selEnd)
            return
        }
        if (queued || rt.nodes[id]?.listens(AsukaEventKind.SELECTION_CHANGED) != true) return
        queued = true
        // After the text change that moved it (its watcher reports first).
        v.post {
            queued = false
            val s = v.selectionStart.coerceAtLeast(0)
            val e = v.selectionEnd.coerceAtLeast(0)
            val p = pack(s, e)
            if (p == last) return@post
            last = p
            val w = AsukaWriter()
            w.u32(s)
            w.u32(e)
            val b = w.bytes()
            rt.emit(id, AsukaEventKind.SELECTION_CHANGED) { AsukaEvents.selectionChanged(id, b) }
        }
    }

    private fun pack(s: Int, e: Int) = (s.toLong() shl 32) or (e.toLong() and 0xffffffffL)
}

/** A field's Label is TalkBack's hint, as TextInputLayout's is: a content description would be read in place of what's typed. */
fun fieldHint(rt: AsukaRuntime, id: Int, info: AccessibilityNodeInfo) {
    (rt.nodes[id]?.props?.get(AsukaProp.A11Y_LABEL) as? String)?.takeIf { it.isNotEmpty() }?.let { AccessibilityNodeInfoCompat.wrap(info).hintText = it }
}

/**
 * TextFieldProps.Suggestions: the field's list. The first makes the
 * field a suggestion field (AsukaSuggestField: the view made again with
 * its props, before it's inserted); later ones update its list.
 */
fun AsukaRuntime.setSuggestions(n: AsukaNode, v: ByteBuffer) {
    val list = decodeStrings(v)
    n.suggestions = list
    val f = n.view as? AsukaSuggestField
    if (f != null) { f.setItems(list); return }
    rebuild(n)
}
