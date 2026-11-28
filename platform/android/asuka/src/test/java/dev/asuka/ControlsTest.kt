package dev.asuka

import android.content.Context
import androidx.test.core.app.ApplicationProvider
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Calendar
import java.util.TimeZone

// The new controls' logic that isn't a view: Material's date range picker's
// UTC midnights and the local days they stand for; a suggestion list that
// shows the core's list as it is (the core filtered it); the image
// module's and an Image's headers; which Material chip a Chip's props make.
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class ControlsTest {
    private val utc = TimeZone.getTimeZone("UTC")

    private fun at(zone: TimeZone, y: Int, m: Int, d: Int, h: Int = 0, min: Int = 0): Long =
        Calendar.getInstance(zone).apply { clear(); set(y, m - 1, d, h, min) }.timeInMillis

    @Test
    fun utcDayIsTheLocalDate() {
        // Late in Los Angeles is the next day in UTC: Material's selection is the local date's.
        val la = TimeZone.getTimeZone("America/Los_Angeles")
        assertEquals(at(utc, 2026, 3, 14), AsukaDays.utcDay(at(la, 2026, 3, 14, 23, 30), la))
        // Early in Tokyo is the day before in UTC.
        val tokyo = TimeZone.getTimeZone("Asia/Tokyo")
        assertEquals(at(utc, 2026, 3, 14), AsukaDays.utcDay(at(tokyo, 2026, 3, 14, 0, 15), tokyo))
        // UTC+14: a whole day ahead of some of the world.
        val kiritimati = TimeZone.getTimeZone("Pacific/Kiritimati")
        assertEquals(at(utc, 2026, 1, 1), AsukaDays.utcDay(at(kiritimati, 2026, 1, 1, 1), kiritimati))
    }

    @Test
    fun localDayIsItsMidnight() {
        val la = TimeZone.getTimeZone("America/Los_Angeles")
        assertEquals(at(la, 2026, 3, 14), AsukaDays.localDay(at(utc, 2026, 3, 14), la))
        // The day clocks go forward (8 March 2026 in the US): its midnight still starts it.
        assertEquals(at(la, 2026, 3, 8), AsukaDays.localDay(at(utc, 2026, 3, 8), la))
        val tokyo = TimeZone.getTimeZone("Asia/Tokyo")
        assertEquals(at(tokyo, 2026, 12, 31), AsukaDays.localDay(at(utc, 2026, 12, 31), tokyo))
        // There and back: any instant of a day is that day's midnight.
        for (zone in listOf(la, tokyo, utc, TimeZone.getTimeZone("Asia/Kolkata"))) {
            val noon = at(zone, 2026, 7, 4, 12)
            assertEquals(at(zone, 2026, 7, 4), AsukaDays.startOfDay(noon, zone))
            assertEquals(at(zone, 2026, 7, 4), AsukaDays.localDay(AsukaDays.utcDay(noon, zone), zone))
        }
    }

    @Test
    fun boundsAreTheDaysFromMinToMax() {
        val zone = TimeZone.getTimeZone("Europe/Berlin")
        val v = AsukaDays.validators(at(zone, 2026, 5, 10, 18), at(zone, 2026, 5, 20, 9), zone)
        assertEquals(2, v.size)
        fun ok(day: Int) = v.all { it.isValid(at(utc, 2026, 5, day)) }
        assertTrue(!ok(9) && ok(10) && ok(15) && ok(20) && !ok(21))
        assertTrue(AsukaDays.validators(0, 0, zone).isEmpty())
    }

    @Test
    fun suggestionsAreShownUnfiltered() {
        val ctx: Context = ApplicationProvider.getApplicationContext()
        val a = AsukaSuggestAdapter(ctx, android.R.layout.simple_list_item_1)
        a.setItems(listOf("Paris", "Prague", "Amsterdam"))
        var count = -1
        // What's typed doesn't filter them: the core did.
        a.filter.filter("zz") { count = it }
        shadowOf(android.os.Looper.getMainLooper()).idle()
        waitFor { count >= 0 }
        assertEquals(3, count)
        assertEquals(listOf("Paris", "Prague", "Amsterdam"), (0 until a.count).map { a.getItem(it) })
        // The core's next list, whatever's typed, is the one shown.
        a.setItems(listOf("Paris"))
        count = -1
        a.filter.filter("x") { count = it }
        waitFor { count >= 0 }
        assertEquals(1, count)
        assertEquals("Paris", a.getItem(0))
        assertEquals("Paris", a.filter.convertResultToString("Paris"))
    }

    private fun waitFor(cond: () -> Boolean) {
        val until = System.nanoTime() + 5_000_000_000L
        while (!cond() && System.nanoTime() < until) {
            shadowOf(android.os.Looper.getMainLooper()).idle()
            Thread.sleep(5)
        }
    }

    private fun strings(vararg s: String): ByteArray {
        val parts = s.map { it.toByteArray() }
        val b = ByteBuffer.allocate(4 + parts.sumOf { 4 + it.size }).order(ByteOrder.LITTLE_ENDIAN)
        b.putInt(s.size)
        for (p in parts) { b.putInt(p.size); b.put(p) }
        return b.array()
    }

    @Test
    fun decodesHeadersAndPrefetch() {
        assertEquals(mapOf("Authorization" to "Bearer t", "X-A" to "1"),
            decodeHeaders(ByteBuffer.wrap(strings("Authorization", "Bearer t", "X-A", "1"))))
        assertEquals(emptyMap<String, String>(), decodeHeaders(ByteBuffer.allocate(0)))
        val args = strings("https://a/1.jpg", "pin.png") + strings("Authorization", "Bearer t")
        val (urls, headers) = decodePrefetch(ByteBuffer.wrap(args))
        assertEquals(listOf("https://a/1.jpg", "pin.png"), urls)
        assertEquals(mapOf("Authorization" to "Bearer t"), headers)
        // No headers after the URLs, or cut short: what's there.
        assertEquals(listOf("x") to emptyMap<String, String>(), decodePrefetch(ByteBuffer.wrap(strings("x"))))
        assertEquals(emptyList<String>(), decodePrefetch(ByteBuffer.wrap(strings("abc").copyOf(9))).first)
    }

    @Test
    fun chipStyles() {
        assertEquals(AsukaChipStyle.FILTER, AsukaChipStyle.of(filter = true, removable = true, icon = true))
        assertEquals(AsukaChipStyle.INPUT, AsukaChipStyle.of(filter = false, removable = true, icon = true))
        assertEquals(AsukaChipStyle.ASSIST, AsukaChipStyle.of(filter = false, removable = false, icon = true))
        assertEquals(AsukaChipStyle.SUGGESTION, AsukaChipStyle.of(filter = false, removable = false, icon = false))
    }
}
