package dev.asuka

import android.app.AlarmManager
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.os.Build
import org.json.JSONArray
import org.json.JSONObject
import java.util.Calendar
import java.util.TimeZone

/**
 * When a trigger (sdk/notify's WireTrigger) shows next: pure, for the
 * tests. The calendar's kinds are on zone's wall clock (the device's):
 * "daily" (hour, minute), "weekly" (weekday 1 Sunday to 7, as
 * Calendar's), "monthly" (day: a month without it is skipped),
 * "yearly" (month 1 to 12, day); "every" (seconds, at least 60) counts
 * from the time it was scheduled or last showed; "at" and "after" are
 * once, at their time in milliseconds since 1970.
 */
object AsukaTriggers {
    fun repeats(t: JSONObject) = t.optString("kind") !in setOf("", "at", "after")

    /** The first time t shows after after (milliseconds since 1970); null: once, and past. */
    fun next(t: JSONObject, after: Long, zone: TimeZone = TimeZone.getDefault()): Long? {
        val h = t.optInt("hour")
        val m = t.optInt("minute")
        val now = Calendar.getInstance(zone).apply { timeInMillis = after }
        val y = now.get(Calendar.YEAR)
        val mo = now.get(Calendar.MONTH)
        val d = now.get(Calendar.DAY_OF_MONTH)
        // A wall time on zone's clock; fields past their range roll into
        // the next (day 32: the next month's first).
        fun wall(year: Int, month0: Int, day: Int) = Calendar.getInstance(zone).apply {
            clear()
            set(year, month0, day, h, m, 0)
        }.timeInMillis
        fun daysIn(year: Int, month0: Int) = Calendar.getInstance(zone).apply {
            clear()
            set(year, month0, 1)
        }.getActualMaximum(Calendar.DAY_OF_MONTH)
        return when (t.optString("kind")) {
            "at", "after", "" -> t.optLong("at").takeIf { it > after }
            "every" -> after + maxOf(60.0, t.optDouble("seconds", 60.0)).times(1000).toLong()
            "daily" -> wall(y, mo, d).takeIf { it > after } ?: wall(y, mo, d + 1)
            "weekly" -> {
                val ahead = (t.optInt("weekday", 1) - now.get(Calendar.DAY_OF_WEEK) + 7) % 7
                wall(y, mo, d + ahead).takeIf { it > after } ?: wall(y, mo, d + ahead + 7)
            }
            "monthly" -> {
                val day = t.optInt("day", 1)
                (0..12).asSequence().map { i -> (mo + i) to (y + (mo + i) / 12) }
                    .filter { (mm, yy) -> day <= daysIn(yy, mm % 12) }
                    .map { (mm, yy) -> wall(yy, mm % 12, day) }
                    .firstOrNull { it > after }
            }
            "yearly" -> {
                val month0 = t.optInt("month", 1) - 1
                val day = t.optInt("day", 1)
                (0..8).asSequence().map { y + it }.filter { day <= daysIn(it, month0) }.map { wall(it, month0, day) }.firstOrNull { it > after }
            }
            else -> null
        }
    }

    /**
     * When a notification shown (or due) at from shows next, now being
     * now: a repeating one's first time after now (an interval's counted
     * on from from, so it keeps its pace: missed times aren't shown);
     * null for a one-time one.
     */
    fun after(t: JSONObject, from: Long, now: Long, zone: TimeZone = TimeZone.getDefault()): Long? {
        if (!repeats(t)) return null
        if (t.optString("kind") == "every") {
            val iv = maxOf(60.0, t.optDouble("seconds", 60.0)).times(1000).toLong()
            if (from > now) return from
            return from + ((now - from) / iv + 1) * iv
        }
        return next(t, maxOf(from, now), zone)
    }
}

/**
 * Scheduled notifications: kept (SharedPreferences, by ID: the spec, its
 * trigger, and when it's due, "asuka.due") so that what an alarm posts is
 * there however long the app has been closed, and so that a reboot,
 * which clears the alarms, an update, or a change of the clock or the
 * time zone, sets them again (AsukaNotifyBootReceiver). An alarm is
 * exact when the app may set them (SCHEDULE_EXACT_ALARM, Android 12;
 * off for an app just installed on 14), else inexact, allowed while
 * idle: Android posts it within a few minutes of its time. A repeating
 * one's next is set as one goes off.
 */
object AsukaNotifySchedule {
    const val DUE = "asuka.due"

    fun prefs(ctx: Context) = ctx.getSharedPreferences("asuka.notify.scheduled", Context.MODE_PRIVATE)

    private fun alarms(ctx: Context) = ctx.getSystemService(Context.ALARM_SERVICE) as AlarmManager

    /** Whether the app may set exact alarms (before Android 12, always). */
    fun exact(ctx: Context): Boolean =
        Build.VERSION.SDK_INT < Build.VERSION_CODES.S || alarms(ctx).canScheduleExactAlarms()

    /** Schedules spec (with its trigger, else its seconds from now), replacing one of its ID. */
    fun add(ctx: Context, spec: JSONObject, now: Long = System.currentTimeMillis()) {
        val id = spec.getString("id")
        val t = spec.optJSONObject("trigger") ?: JSONObject().put("kind", "after")
            .put("at", now + (maxOf(1.0, spec.optDouble("seconds", 1.0)) * 1000).toLong())
        spec.put("trigger", t)
        val due = AsukaTriggers.next(t, now) ?: (now + 1000) // a time that's past: in a second, as iOS's
        spec.put(DUE, due)
        prefs(ctx).edit().putString(id, spec.toString()).commit()
        arm(ctx, id, due, spec)
    }

    /** Sets id's alarm for at: exact if it may, else inexact. */
    fun arm(ctx: Context, id: String, at: Long, spec: JSONObject) {
        val i = Intent(ctx, AsukaNotifyReceiver::class.java).putExtra(AsukaNotifyReceiver.SPEC, spec.toString()).putExtra("id", id)
        val pi = AsukaNotifyReceiver.alarm(ctx, id, i)
        val am = alarms(ctx)
        try {
            if (exact(ctx)) {
                am.setExactAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi)
                return
            }
        } catch (_: SecurityException) {
            // Taken away meanwhile: inexact.
        }
        am.setAndAllowWhileIdle(AlarmManager.RTC_WAKEUP, at, pi)
    }

    fun cancel(ctx: Context, id: String) {
        alarms(ctx).cancel(AsukaNotifyReceiver.alarm(ctx, id, Intent(ctx, AsukaNotifyReceiver::class.java)))
        prefs(ctx).edit().remove(id).commit()
    }

    fun cancelAll(ctx: Context) {
        for (id in prefs(ctx).all.keys) alarms(ctx).cancel(AsukaNotifyReceiver.alarm(ctx, id, Intent(ctx, AsukaNotifyReceiver::class.java)))
        prefs(ctx).edit().clear().commit()
    }

    fun kept(ctx: Context): List<JSONObject> = prefs(ctx).all.values.mapNotNull { v ->
        (v as? String)?.let { try { JSONObject(it) } catch (_: Exception) { null } }
    }

    /** What's waiting (sdk/notify's WireScheduled), the soonest first. */
    fun pending(ctx: Context): JSONArray {
        val out = JSONArray()
        for (s in kept(ctx).sortedBy { it.optLong(DUE) }) {
            out.put(JSONObject().put("id", s.optString("id")).put("title", s.optString("title")).put("subtitle", s.optString("subtitle"))
                .put("body", s.optString("body")).put("url", s.optString("url")).put("data", s.optJSONObject("data") ?: JSONObject())
                .put("category", s.optString("category")).put("thread", s.optString("thread"))
                .put("trigger", s.optJSONObject("trigger") ?: JSONObject().put("kind", "after")).put("next", s.optLong(DUE)))
        }
        return out
    }

    /**
     * id's alarm went off (spec: what it carried, for one scheduled
     * before they were kept): the spec to post, and a repeating one's
     * next alarm set (a one-time one forgotten). Null: nothing kept or
     * carried (cancelled meanwhile).
     */
    fun fired(ctx: Context, id: String?, carried: JSONObject?, now: Long = System.currentTimeMillis()): JSONObject? {
        val kept = id?.let { prefs(ctx).getString(it, null) }?.let { try { JSONObject(it) } catch (_: Exception) { null } }
        if (kept == null) {
            // Carried only: scheduled before they were kept, or kept and
            // cancelled since (then nothing: the alarm was cancelled too).
            return if (carried != null && carried.optJSONObject("trigger") == null) carried else null
        }
        val sid = kept.getString("id")
        val t = kept.optJSONObject("trigger") ?: JSONObject()
        val next = AsukaTriggers.after(t, kept.optLong(DUE, now), now)
        if (next == null) {
            prefs(ctx).edit().remove(sid).commit()
        } else {
            kept.put(DUE, next)
            prefs(ctx).edit().putString(sid, kept.toString()).commit()
            arm(ctx, sid, next, kept)
        }
        return JSONObject(kept.toString()).apply { remove(DUE) }
    }

    /**
     * After a reboot (which clears the alarms), an update, the clock or
     * the time zone changed, or exact alarms allowed: each kept one's
     * alarm set again, a repeating one's for its next time on the clock
     * as it is now (the times it missed aren't shown). A one-time one
     * whose time went by meanwhile is answered, to post now.
     */
    fun restore(ctx: Context, now: Long = System.currentTimeMillis()): List<JSONObject> {
        val missed = ArrayList<JSONObject>()
        for (s in kept(ctx)) {
            val id = s.optString("id")
            if (id.isEmpty()) continue
            val t = s.optJSONObject("trigger") ?: JSONObject()
            val due = if (AsukaTriggers.repeats(t)) {
                if (t.optString("kind") == "every") AsukaTriggers.after(t, s.optLong(DUE, now), now - 1) else AsukaTriggers.next(t, now)
            } else {
                s.optLong(DUE).takeIf { it > now }
            }
            if (due == null) {
                prefs(ctx).edit().remove(id).commit()
                missed.add(JSONObject(s.toString()).apply { remove(DUE) })
                continue
            }
            s.put(DUE, due)
            prefs(ctx).edit().putString(id, s.toString()).commit()
            arm(ctx, id, due, s)
        }
        return missed
    }
}

/**
 * Sets the scheduled notifications' alarms again when Android cleared or
 * moved them: a reboot, the app updated, the clock or the time zone
 * changed, exact alarms allowed (Android 12's permission). Declared only
 * in apps that schedule notifications (sdk/notify's asuka.toml).
 */
class AsukaNotifyBootReceiver : BroadcastReceiver() {
    override fun onReceive(ctx: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_BOOT_COMPLETED, Intent.ACTION_MY_PACKAGE_REPLACED, Intent.ACTION_TIME_CHANGED,
            Intent.ACTION_TIMEZONE_CHANGED, AlarmManager.ACTION_SCHEDULE_EXACT_ALARM_PERMISSION_STATE_CHANGED -> {}
            else -> return
        }
        val pending = goAsync()
        val app = ctx.applicationContext
        AsukaNotifications.worker.execute {
            try {
                for (s in AsukaNotifySchedule.restore(app)) AsukaNotifications.show(app, s, remote = false)
            } finally {
                pending?.finish()
            }
        }
    }
}
