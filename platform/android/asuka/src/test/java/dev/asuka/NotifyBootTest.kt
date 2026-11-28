package dev.asuka

import android.app.AlarmManager
import android.content.Context
import android.content.Intent
import androidx.test.core.app.ApplicationProvider
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.Shadows.shadowOf
import org.robolectric.annotation.Config
import org.robolectric.shadows.ShadowAlarmManager
import java.util.concurrent.TimeUnit

// Scheduled notifications on Android's own classes (Robolectric): kept
// with their alarms, exact when the app may set them and inexact when
// not; a reboot clears the alarms and the boot receiver sets them again
// from what's kept (a repeating one for its next time, a one-time one
// that went by meanwhile posted); a repeat's alarm goes off and sets the
// next; cancel takes the alarm and what's kept.
@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35])
class NotifyBootTest {
    private lateinit var ctx: Context
    private lateinit var alarms: AlarmManager
    private val now = System.currentTimeMillis()

    @Before
    fun setUp() {
        ctx = ApplicationProvider.getApplicationContext()
        alarms = ctx.getSystemService(Context.ALARM_SERVICE) as AlarmManager
        ShadowAlarmManager.setCanScheduleExactAlarms(true)
        AsukaNotifySchedule.prefs(ctx).edit().clear().commit()
    }

    private fun spec(id: String, trigger: JSONObject) = JSONObject().put("id", id).put("title", id).put("seconds", 60.0).put("trigger", trigger)

    private fun scheduled() = shadowOf(alarms).scheduledAlarms

    /** The alarm for id: its time, and whether it's exact (allowed while idle either way). */
    private fun alarmFor(id: String) = scheduled().firstOrNull { shadowOf(it.operation).savedIntent.getStringExtra("id") == id }

    /** What a reboot does to the alarms: Android forgets them all. */
    private fun reboot() {
        for (a in scheduled().toList()) a.operation?.let { alarms.cancel(it) }
    }

    private fun drain() = AsukaNotifications.worker.submit {}.get(5, TimeUnit.SECONDS)

    @Test
    fun keptAndArmed() {
        val daily = JSONObject().put("kind", "daily").put("hour", 9).put("minute", 0)
        AsukaNotifySchedule.add(ctx, spec("water", daily), now)
        AsukaNotifySchedule.add(ctx, spec("tea", JSONObject().put("kind", "after").put("at", now + 240_000)), now)
        val water = alarmFor("water")!!
        assertEquals(AsukaTriggers.next(daily, now), water.triggerAtMs)
        assertEquals(AlarmManager.RTC_WAKEUP, water.type)
        assertEquals(now + 240_000, alarmFor("tea")!!.triggerAtMs)
        val pending = AsukaNotifySchedule.pending(ctx)
        assertEquals(2, pending.length())
        assertEquals("tea", pending.getJSONObject(0).getString("id"))
        assertEquals("daily", pending.getJSONObject(1).getJSONObject("trigger").getString("kind"))
        assertEquals(water.triggerAtMs, pending.getJSONObject(1).getLong("next"))
    }

    @Test
    fun inexactWithoutThePermission() {
        ShadowAlarmManager.setCanScheduleExactAlarms(false)
        AsukaNotifySchedule.add(ctx, spec("tea", JSONObject().put("kind", "after").put("at", now + 60_000)), now)
        val a = alarmFor("tea")!!
        assertTrue(a.isAllowWhileIdle)
        assertEquals(false, AsukaNotifySchedule.exact(ctx))
    }

    @Test
    fun rebootSetsThemAgain() {
        val weekly = JSONObject().put("kind", "weekly").put("weekday", 2).put("hour", 8).put("minute", 30)
        val every = JSONObject().put("kind", "every").put("seconds", 3600.0)
        AsukaNotifySchedule.add(ctx, spec("standup", weekly), now)
        AsukaNotifySchedule.add(ctx, spec("hourly", every), now)
        AsukaNotifySchedule.add(ctx, spec("soon", JSONObject().put("kind", "at").put("at", now + 1000)), now)
        AsukaNotifySchedule.add(ctx, spec("later", JSONObject().put("kind", "at").put("at", now + 86_400_000)), now)
        // The reboot: Android forgets every alarm.
        reboot()
        assertEquals(0, scheduled().size)

        // Back up two hours later: what's kept is set again.
        val up = now + 2 * 3_600_000L + 30_000
        val missed = AsukaNotifySchedule.restore(ctx, up)
        assertEquals(listOf("soon"), missed.map { it.getString("id") })
        assertEquals(AsukaTriggers.next(weekly, up), alarmFor("standup")!!.triggerAtMs)
        assertEquals(now + 3 * 3_600_000L, alarmFor("hourly")!!.triggerAtMs)
        assertEquals(now + 86_400_000, alarmFor("later")!!.triggerAtMs)
        assertNull(alarmFor("soon"))
        assertEquals(setOf("standup", "hourly", "later"), (0 until AsukaNotifySchedule.pending(ctx).length()).map { AsukaNotifySchedule.pending(ctx).getJSONObject(it).getString("id") }.toSet())
    }

    @Test
    fun bootReceiver() {
        val daily = JSONObject().put("kind", "daily").put("hour", 7).put("minute", 15)
        AsukaNotifySchedule.add(ctx, spec("plants", daily), now)
        reboot()
        for (action in listOf(Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED, Intent.ACTION_TIMEZONE_CHANGED)) {
            reboot()
            AsukaNotifyBootReceiver().onReceive(ctx, Intent(action))
            drain()
            assertNotNull(action, alarmFor("plants"))
        }
        // Another broadcast: nothing.
        reboot()
        AsukaNotifyBootReceiver().onReceive(ctx, Intent(Intent.ACTION_SCREEN_ON))
        drain()
        assertNull(alarmFor("plants"))
    }

    @Test
    fun aRepeatSetsItsNext() {
        val every = JSONObject().put("kind", "every").put("seconds", 600.0)
        AsukaNotifySchedule.add(ctx, spec("tick", every), now)
        val first = alarmFor("tick")!!.triggerAtMs
        val shown = AsukaNotifySchedule.fired(ctx, "tick", null, first + 50)!!
        assertEquals("tick", shown.getString("id"))
        assertEquals(first + 600_000, alarmFor("tick")!!.triggerAtMs)
        // A one-time one is forgotten once it posts.
        AsukaNotifySchedule.add(ctx, spec("once", JSONObject().put("kind", "after").put("at", now + 5000)), now)
        assertNotNull(AsukaNotifySchedule.fired(ctx, "once", null, now + 5000))
        assertNull(AsukaNotifySchedule.prefs(ctx).getString("once", null))
        // Cancelled: its alarm and what's kept go, and a late alarm posts nothing.
        AsukaNotifySchedule.cancel(ctx, "tick")
        assertNull(alarmFor("tick"))
        assertNull(AsukaNotifySchedule.fired(ctx, "tick", spec("tick", every), first + 600_000))
        // One scheduled by an earlier version (carried, not kept): posted.
        assertNotNull(AsukaNotifySchedule.fired(ctx, "old", JSONObject().put("id", "old").put("title", "Old"), now))
    }
}
