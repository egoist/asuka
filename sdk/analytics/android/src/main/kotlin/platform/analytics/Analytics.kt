// platform/analytics on Android: Google Analytics for Firebase. Firebase
// starts with the process (its FirebaseInitProvider), from the resources
// the CLI writes of the app's google-services.json (asuka.toml's
// android.googleServices); without them there's no FirebaseApp, and no
// Collector: the package sends nothing.
//
// What Asuka checked crosses as it is: the event's parameters (their
// kinds) become a Bundle's Strings, Longs and Doubles, its items the
// items parameter's Bundles.
package platform.analytics

import android.os.Bundle
import com.google.firebase.FirebaseApp
import com.google.firebase.analytics.FirebaseAnalytics
import dev.asuka.AsukaModules
import dev.asuka.AsukaUnsupportedException
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlin.coroutines.resume

private class FirebaseCollector(private val fa: FirebaseAnalytics) : Collector {
    override fun log(name: String, params: List<WireValue>, items: List<WireItem>) {
        val b = bundle(params)
        if (items.isNotEmpty()) {
            b.putParcelableArray(FirebaseAnalytics.Param.ITEMS, items.map { bundle(it.params) }.toTypedArray())
        }
        fa.logEvent(name, if (b.isEmpty) null else b)
    }

    override fun setUserId(id: String?) = fa.setUserId(id)

    override fun setUserProperty(name: String, value: String?) = fa.setUserProperty(name, value)

    override fun setDefaults(params: List<WireValue>) {
        // Firebase adds to its defaults: these replace them.
        fa.setDefaultEventParameters(null)
        if (params.isNotEmpty()) fa.setDefaultEventParameters(bundle(params))
    }

    override fun setEnabled(on: Boolean) = fa.setAnalyticsCollectionEnabled(on)

    override fun setConsent(consent: Consent) {
        val s = mutableMapOf<FirebaseAnalytics.ConsentType, FirebaseAnalytics.ConsentStatus>()
        val pairs = listOf(
            FirebaseAnalytics.ConsentType.ANALYTICS_STORAGE to consent.analyticsStorage,
            FirebaseAnalytics.ConsentType.AD_STORAGE to consent.adStorage,
            FirebaseAnalytics.ConsentType.AD_USER_DATA to consent.adUserData,
            FirebaseAnalytics.ConsentType.AD_PERSONALIZATION to consent.adPersonalization,
        )
        for ((type, granted) in pairs) {
            if (granted != null) s[type] = if (granted) FirebaseAnalytics.ConsentStatus.GRANTED else FirebaseAnalytics.ConsentStatus.DENIED
        }
        if (s.isNotEmpty()) fa.setConsent(s)
    }

    override suspend fun appInstanceId(): String? = suspendCancellableCoroutine { c ->
        fa.appInstanceId.addOnCompleteListener { t -> c.resume(if (t.isSuccessful) t.result else null) }
    }

    override fun reset() = fa.resetAnalyticsData()
}

/** The parameters as a Bundle: text a String, an integer a Long, a number a Double. */
internal fun bundle(params: List<WireValue>): Bundle {
    val b = Bundle()
    for (v in params) {
        when (v.kind) {
            Kind.INTEGER -> b.putLong(v.name, v.integer)
            Kind.NUMBER -> b.putDouble(v.name, v.number)
            else -> b.putString(v.name, v.text)
        }
    }
    return b
}

fun makeCollector(): Collector {
    val context = AsukaModules.context
    if (FirebaseApp.getApps(context).isEmpty()) {
        throw AsukaUnsupportedException("analytics: Firebase isn't configured: set asuka.toml's android.googleServices to the project's google-services.json")
    }
    return FirebaseCollector(FirebaseAnalytics.getInstance(context))
}
