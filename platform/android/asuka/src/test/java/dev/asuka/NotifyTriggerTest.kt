package dev.asuka

import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Calendar
import java.util.TimeZone

// When a scheduled notification's trigger (sdk/notify's WireTrigger)
// shows next, on a zone's wall clock: each kind, the months without a
// day, February 29, daylight saving time, and a repeat's next after it
// showed (an interval keeping its pace, missed times skipped).
class NotifyTriggerTest {
    private val utc = TimeZone.getTimeZone("UTC")
    private val ny = TimeZone.getTimeZone("America/New_York")

    private fun at(zone: TimeZone, y: Int, mo: Int, d: Int, h: Int = 0, m: Int = 0) =
        Calendar.getInstance(zone).apply { clear(); set(y, mo - 1, d, h, m, 0) }.timeInMillis

    // A Saturday, 2026-03-07 15:04 UTC, as the package's tests.
    private val sat = at(utc, 2026, 3, 7, 15, 4)

    private fun t(kind: String, vararg kv: Pair<String, Any>) = JSONObject().put("kind", kind).also { o -> kv.forEach { o.put(it.first, it.second) } }

    @Test
    fun daily() {
        assertEquals(at(utc, 2026, 3, 7, 16, 30), AsukaTriggers.next(t("daily", "hour" to 16, "minute" to 30), sat, utc))
        assertEquals(at(utc, 2026, 3, 8, 9), AsukaTriggers.next(t("daily", "hour" to 9), sat, utc))
        // Its own minute: the next day's.
        assertEquals(at(utc, 2026, 3, 8, 15, 4), AsukaTriggers.next(t("daily", "hour" to 15, "minute" to 4), sat, utc))
    }

    @Test
    fun weeklyMonthlyYearly() {
        assertEquals(at(utc, 2026, 3, 9, 9, 30), AsukaTriggers.next(t("weekly", "weekday" to 2, "hour" to 9, "minute" to 30), sat, utc))
        assertEquals(at(utc, 2026, 3, 7, 18), AsukaTriggers.next(t("weekly", "weekday" to 7, "hour" to 18), sat, utc))
        assertEquals(at(utc, 2026, 3, 14, 8), AsukaTriggers.next(t("weekly", "weekday" to 7, "hour" to 8), sat, utc))
        assertEquals(at(utc, 2026, 4, 1, 8), AsukaTriggers.next(t("monthly", "day" to 1, "hour" to 8), sat, utc))
        // April has no 31st.
        assertEquals(at(utc, 2026, 5, 31, 8), AsukaTriggers.next(t("monthly", "day" to 31, "hour" to 8), at(utc, 2026, 3, 31, 9), utc))
        assertEquals(at(utc, 2027, 1, 5), AsukaTriggers.next(t("monthly", "day" to 5), at(utc, 2026, 12, 20), utc))
        assertEquals(at(utc, 2026, 12, 25, 7), AsukaTriggers.next(t("yearly", "month" to 12, "day" to 25, "hour" to 7), sat, utc))
        assertEquals(at(utc, 2028, 2, 29), AsukaTriggers.next(t("yearly", "month" to 2, "day" to 29), sat, utc))
    }

    @Test
    fun onceAndEvery() {
        val later = sat + 7_200_000
        assertEquals(later, AsukaTriggers.next(t("at", "at" to later), sat, utc))
        assertNull(AsukaTriggers.next(t("at", "at" to sat), later, utc))
        assertEquals(sat + 900_000, AsukaTriggers.next(t("every", "seconds" to 900.0), sat, utc))
        // At least a minute.
        assertEquals(sat + 60_000, AsukaTriggers.next(t("every", "seconds" to 5.0), sat, utc))
        assertFalse(AsukaTriggers.repeats(t("after", "at" to later)))
        assertTrue(AsukaTriggers.repeats(t("every", "seconds" to 60.0)))
    }

    @Test
    fun daylightSaving() {
        // New York's clocks go forward on 2026-03-08: 9:00 is 9:00 on the
        // clock both days, 23 hours apart.
        val before = at(ny, 2026, 3, 7, 9, 0)
        val next = AsukaTriggers.next(t("daily", "hour" to 9), before, ny)!!
        assertEquals(at(ny, 2026, 3, 8, 9, 0), next)
        assertEquals(23L * 3_600_000, next - before)
    }

    @Test
    fun afterShowing() {
        // An interval shown late (the device asleep): its next keeps the
        // pace, the missed ones skipped.
        val every = t("every", "seconds" to 3600.0)
        assertEquals(sat + 3 * 3_600_000L, AsukaTriggers.after(every, sat, sat + 2 * 3_600_000L + 5, utc))
        assertEquals(sat + 3_600_000L, AsukaTriggers.after(every, sat + 3_600_000L, sat, utc))
        // A daily one: the next on the clock after now.
        assertEquals(at(utc, 2026, 3, 10, 9), AsukaTriggers.after(t("daily", "hour" to 9), at(utc, 2026, 3, 8, 9), at(utc, 2026, 3, 9, 12), utc))
        // Once: none.
        assertNull(AsukaTriggers.after(t("at", "at" to sat), sat, sat + 1, utc))
    }
}
