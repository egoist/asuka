package dev.asuka

import android.content.Context
import android.content.res.Configuration
import android.content.res.Resources
import android.os.Build
import android.os.LocaleList
import java.util.Locale

/**
 * What the environment says of the user's languages and regional formats.
 * It takes a resource context to work out, so it's kept, and read again as
 * they change.
 */
data class AsukaRegion(val languages: String, val locale: String, val firstWeekday: Int, val hourCycle: Int)

/**
 * The system's locales, ignoring the app's own language: the regional formats
 * are the user's whatever language the app shows.
 */
private fun AsukaRuntime.systemLocales(): LocaleList {
    // Before Android 13 there's no app's language in the configuration:
    // the system's resources have the system's locales.
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) return Resources.getSystem().configuration.locales
    val list = app.getSystemService(android.app.LocaleManager::class.java)?.systemLocales
    return if (list == null || list.isEmpty) app.resources.configuration.locales else list
}

/** Where the app's language is kept before Android 13, which has no per-app languages. */
private fun AsukaRuntime.localePrefs() = app.getSharedPreferences("dev.asuka.locale", Context.MODE_PRIVATE)

/** The app's language SaveLocale kept, before Android 13, or null: the system's. */
fun AsukaRuntime.savedAppLocale(): String? = localePrefs().getString("locale", null)?.ifEmpty { null }

/** Keeps the app's language for its next launches, before Android 13, or "" to follow the system's. */
fun AsukaRuntime.saveAppLocale(tag: String) = localePrefs().edit().putString("locale", tag).apply()

/** l without its extensions (-u-, -t-). */
private fun stripped(l: Locale): Locale =
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) l.stripExtensions()
    else runCatching { Locale.Builder().setLocale(l).clearExtensions().build() }.getOrDefault(l)

fun AsukaRuntime.readRegion(): AsukaRegion {
    val tags = LinkedHashSet<String>()
    // The configuration has the app's language first, if it has one.
    // Before Android 13 it has only the system's: the app's saved one comes first.
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.TIRAMISU) savedAppLocale()?.let { tags.add(stripped(Locale.forLanguageTag(it)).toLanguageTag()) }
    val config = app.resources.configuration.locales
    for (i in 0 until config.size()) tags.add(stripped(config[i]).toLanguageTag())
    val formats = systemLocales()[0]
    return AsukaRegion(tags.joinToString(","), formats.toLanguageTag(), firstWeekday(formats), hourCycle(formats))
}

/** 1 Sunday to 7 Saturday: what the user set (Android 14's regional preferences), else the locale's. */
private fun firstWeekday(l: Locale): Int =
    when (l.getUnicodeLocaleType("fw")) {
        "sun" -> 1
        "mon" -> 2
        "tue" -> 3
        "wed" -> 4
        "thu" -> 5
        "fri" -> 6
        "sat" -> 7
        else -> android.icu.util.Calendar.getInstance(l).firstDayOfWeek
    }

/** 12 or 24: the system's setting, or where it has none, what's usual in the formats' locale (not the app's language). */
private fun AsukaRuntime.hourCycle(l: Locale): Int {
    // As DateFormat.is24HourFormat decides, without a context in l (a
    // configuration context is a Resources of its own, made as the app
    // starts).
    when (android.provider.Settings.System.getString(app.contentResolver, android.provider.Settings.System.TIME_12_24)) {
        "24" -> return 24
        "12" -> return 12
    }
    val p = android.text.format.DateFormat.getBestDateTimePattern(l, "jm")
    return if (p.contains('H') || p.contains('k')) 24 else 12
}
