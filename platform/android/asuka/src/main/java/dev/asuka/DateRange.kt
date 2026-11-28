package dev.asuka

import android.annotation.SuppressLint
import android.view.ContextThemeWrapper
import android.widget.FrameLayout
import com.google.android.material.button.MaterialButton
import com.google.android.material.datepicker.CalendarConstraints
import com.google.android.material.datepicker.CompositeDateValidator
import com.google.android.material.datepicker.DateValidatorPointBackward
import com.google.android.material.datepicker.DateValidatorPointForward
import com.google.android.material.datepicker.MaterialDatePicker
import java.util.Calendar
import java.util.TimeZone

/**
 * Days as Material's date pickers keep them, UTC midnights, and as the
 * app does, local ones: a day is its date in the device's time zone.
 */
object AsukaDays {
    private val utc: TimeZone = TimeZone.getTimeZone("UTC")

    /** The UTC midnight of the date ms is on in zone (Material's selection for that day). */
    fun utcDay(ms: Long, zone: TimeZone = TimeZone.getDefault()): Long {
        val c = Calendar.getInstance(zone).apply { timeInMillis = ms }
        return Calendar.getInstance(utc).apply {
            clear()
            set(c.get(Calendar.YEAR), c.get(Calendar.MONTH), c.get(Calendar.DAY_OF_MONTH))
        }.timeInMillis
    }

    /** The midnight in zone that starts the date of utcMidnight (a day Material chose). */
    fun localDay(utcMidnight: Long, zone: TimeZone = TimeZone.getDefault()): Long {
        val u = Calendar.getInstance(utc).apply { timeInMillis = utcMidnight }
        return Calendar.getInstance(zone).apply {
            clear()
            set(u.get(Calendar.YEAR), u.get(Calendar.MONTH), u.get(Calendar.DAY_OF_MONTH))
        }.timeInMillis
    }

    /** The local midnight of the day ms is in (zone's). */
    fun startOfDay(ms: Long, zone: TimeZone = TimeZone.getDefault()): Long = localDay(utcDay(ms, zone), zone)

    /**
     * Material's calendar bounds for Min and Max (0: none): the validators
     * that let the user choose only the days from min's to max's, both in.
     */
    fun validators(min: Long, max: Long, zone: TimeZone = TimeZone.getDefault()): List<CalendarConstraints.DateValidator> {
        val out = ArrayList<CalendarConstraints.DateValidator>()
        if (min != 0L) out.add(DateValidatorPointForward.from(utcDay(min, zone)))
        if (max != 0L) out.add(DateValidatorPointBackward.before(utcDay(max, zone) + 1))
        return out
    }
}

/**
 * A DateRangePicker: Material's pattern, a button showing the range (in
 * the device's format, DateUtils.formatDateRange; Material's "Start date
 * – End date" while none is chosen) that opens Material's date range
 * picker, the days before Min and after Max not chosen. Its Save reports
 * RangeChanged, the two days' local midnights. Inline is the same button:
 * Material has no calendar in place for a range. Made again with the
 * theme (restyle), keeping what it shows.
 */
@SuppressLint("ViewConstructor")
class AsukaDateRangePicker(private val rt: AsukaRuntime, private val asukaID: Int) : FrameLayout(rt.themed) {
    var start = 0L
        set(v) { field = v; refresh() }
    var end = 0L
        set(v) { field = v; refresh() }
    var min = 0L
    var max = 0L
    private var tint = 0L
    private lateinit var button: MaterialButton

    init { make() }

    private fun make() {
        val enabled = if (::button.isInitialized) button.isEnabled else true
        removeAllViews()
        button = MaterialButton(ContextThemeWrapper(rt.themed, R.style.ThemeOverlay_Asuka_TonalButton)).apply {
            isAllCaps = false
            maxLines = 1
            ellipsize = android.text.TextUtils.TruncateAt.END
            isEnabled = enabled
            setOnClickListener { pick() }
        }
        addView(button, LayoutParams(LayoutParams.MATCH_PARENT, LayoutParams.WRAP_CONTENT))
        refresh()
        applyTint()
    }

    /** Made again with the current theme (dark mode, the accent). */
    fun restyle() = make()

    override fun setEnabled(enabled: Boolean) {
        super.setEnabled(enabled)
        button.isEnabled = enabled
    }

    /** Color: the button's text, as a Button's. */
    fun setTint(c: Long) {
        tint = c
        applyTint()
    }

    private fun applyTint() {
        if (tint != 0L) button.setTextColor(rt.color(tint))
    }

    /** Shows the range, in the language. */
    @SuppressLint("PrivateResource") // Material's own words for none chosen, in its languages
    fun refresh() {
        if (!::button.isInitialized) return
        button.text = if (start == 0L) context.getString(com.google.android.material.R.string.mtrl_picker_range_header_unselected)
        else {
            // An instant inside the last day: formatDateRange takes a
            // midnight end as the day before's.
            val last = maxOf(AsukaDays.startOfDay(if (end != 0L) end else start), AsukaDays.startOfDay(start)) + 12 * 3_600_000L
            android.text.format.DateUtils.formatDateRange(context, start, last,
                android.text.format.DateUtils.FORMAT_SHOW_DATE or android.text.format.DateUtils.FORMAT_ABBREV_MONTH)
        }
        requestLayout()
    }

    private fun pick() {
        val act = rt.activity ?: return
        val constraints = CalendarConstraints.Builder().setFirstDayOfWeek(rt.region.firstWeekday)
        val validators = AsukaDays.validators(min, max)
        if (validators.isNotEmpty()) constraints.setValidator(CompositeDateValidator.allOf(validators))
        val b = MaterialDatePicker.Builder.dateRangePicker().setCalendarConstraints(constraints.build())
        if (start != 0L) {
            val s = AsukaDays.utcDay(start)
            b.setSelection(androidx.core.util.Pair(s, maxOf(s, AsukaDays.utcDay(if (end != 0L) end else start))))
        }
        val p = b.build()
        p.addOnPositiveButtonClickListener { sel ->
            val s = sel.first ?: return@addOnPositiveButtonClickListener
            val from = AsukaDays.localDay(s)
            val to = maxOf(from, AsukaDays.localDay(sel.second ?: s))
            // Shown at once; the core sends it back, or what the app made of it.
            start = from
            end = to
            rt.emit(asukaID, AsukaEventKind.RANGE_CHANGED) { AsukaEvents.rangeChanged(asukaID, from.toDouble(), to.toDouble()) }
        }
        p.show(act.supportFragmentManager, "asuka-range-$asukaID")
        act.supportFragmentManager.executePendingTransactions()
        p.dialog?.let { rt.directDialog(it) }
    }
}

/** Sets a DateRangePicker's prop; false if prop isn't one of its own. */
fun setDateRangeProp(n: AsukaNode, prop: Int, v: Any): Boolean {
    val p = n.view as? AsukaDateRangePicker ?: return false
    when (prop) {
        AsukaProp.DateRangePicker.START -> p.start = v as Long
        AsukaProp.DateRangePicker.END -> p.end = v as Long
        AsukaProp.DateRangePicker.MIN -> p.min = v as Long
        AsukaProp.DateRangePicker.MAX -> p.max = v as Long
        AsukaProp.DateRangePicker.INLINE -> {} // the same button: Material has no range calendar in place
        else -> return false
    }
    return true
}
