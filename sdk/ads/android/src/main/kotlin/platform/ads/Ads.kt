// platform/ads on Android: Google Mobile Ads (AdMob) and its User
// Messaging Platform. The SDK reads the app's ID from the manifest's
// com.google.android.gms.ads.APPLICATION_ID (asuka.toml's ads.android).
package platform.ads

import android.app.Activity
import android.view.View
import android.widget.FrameLayout
import com.google.android.gms.ads.AdError
import com.google.android.gms.ads.AdListener
import com.google.android.gms.ads.AdRequest
import com.google.android.gms.ads.AdSize
import com.google.android.gms.ads.AdView
import com.google.android.gms.ads.FullScreenContentCallback
import com.google.android.gms.ads.LoadAdError
import com.google.android.gms.ads.MobileAds
import com.google.android.gms.ads.RequestConfiguration
import com.google.android.gms.ads.appopen.AppOpenAd
import com.google.android.gms.ads.interstitial.InterstitialAd
import com.google.android.gms.ads.interstitial.InterstitialAdLoadCallback
import com.google.android.gms.ads.rewarded.RewardedAd
import com.google.android.gms.ads.rewarded.RewardedAdLoadCallback
import com.google.android.gms.ads.rewarded.ServerSideVerificationOptions
import com.google.android.gms.ads.rewardedinterstitial.RewardedInterstitialAd
import com.google.android.gms.ads.rewardedinterstitial.RewardedInterstitialAdLoadCallback
import com.google.android.ump.ConsentDebugSettings
import com.google.android.ump.ConsentInformation
import com.google.android.ump.ConsentRequestParameters
import com.google.android.ump.FormError
import com.google.android.ump.UserMessagingPlatform
import dev.asuka.AsukaModules
import dev.asuka.AsukaUnsupportedException
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext

private class GoogleMobile : Mobile {
    override suspend fun start() {
        val (d, first) = synchronized(GoogleMobile) {
            val s = started
            if (s != null) s to false else CompletableDeferred<kotlin.Unit>().also { started = it } to true
        }
        // Google's advice: off the main thread.
        if (first) withContext(Dispatchers.IO) { MobileAds.initialize(AsukaModules.context) { d.complete(kotlin.Unit) } }
        d.await()
    }

    override fun configure(settings: Settings) {
        val b = MobileAds.getRequestConfiguration().toBuilder()
            .setTestDeviceIds(settings.testDevices)
            // Child: COPPA's treatment; teen: under the age of consent.
            .setTagForChildDirectedTreatment(if (settings.age == Age.CHILD) RequestConfiguration.TAG_FOR_CHILD_DIRECTED_TREATMENT_TRUE
                else RequestConfiguration.TAG_FOR_CHILD_DIRECTED_TREATMENT_UNSPECIFIED)
            .setTagForUnderAgeOfConsent(if (settings.age == Age.TEEN) RequestConfiguration.TAG_FOR_UNDER_AGE_OF_CONSENT_TRUE
                else RequestConfiguration.TAG_FOR_UNDER_AGE_OF_CONSENT_UNSPECIFIED)
            .setMaxAdContentRating(when (settings.maxRating) {
                Rating.GENERAL -> RequestConfiguration.MAX_AD_CONTENT_RATING_G
                Rating.PARENTAL_GUIDANCE -> RequestConfiguration.MAX_AD_CONTENT_RATING_PG
                Rating.TEEN -> RequestConfiguration.MAX_AD_CONTENT_RATING_T
                Rating.MATURE_AUDIENCE -> RequestConfiguration.MAX_AD_CONTENT_RATING_MA
                else -> RequestConfiguration.MAX_AD_CONTENT_RATING_UNSPECIFIED
            })
            .setPublisherPrivacyPersonalizationState(when (settings.personalized) {
                true -> RequestConfiguration.PublisherPrivacyPersonalizationState.ENABLED
                false -> RequestConfiguration.PublisherPrivacyPersonalizationState.DISABLED
                null -> RequestConfiguration.PublisherPrivacyPersonalizationState.DEFAULT
            })
        MobileAds.setRequestConfiguration(b.build())
        MobileAds.setAppMuted(settings.muted)
        MobileAds.setAppVolume(settings.volume.coerceIn(0.0, 1.0).toFloat())
    }

    override suspend fun consent(options: ConsentOptions): Consent = withContext(Dispatchers.Main) {
        val act = activity()
        val p = ConsentRequestParameters.Builder().setTagForUnderAgeOfConsent(options.underAgeOfConsent)
        if (options.debugGeography != DebugGeography.DISABLED || options.testDevices.isNotEmpty()) {
            val d = ConsentDebugSettings.Builder(act).setDebugGeography(when (options.debugGeography) {
                DebugGeography.EEA -> ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_EEA
                DebugGeography.REGULATED_US_STATE -> ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_REGULATED_US_STATE
                DebugGeography.OTHER -> ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_OTHER
                else -> ConsentDebugSettings.DebugGeography.DEBUG_GEOGRAPHY_DISABLED
            })
            for (id in options.testDevices) d.addTestDeviceHashedId(id)
            p.setConsentDebugSettings(d.build())
        }
        suspendCancellableCoroutine<kotlin.Unit> { c ->
            info().requestConsentInfoUpdate(act, p.build(), { c.resume(kotlin.Unit) }, { e -> c.resumeWithException(formException(e)) })
        }
        suspendCancellableCoroutine<kotlin.Unit> { c ->
            UserMessagingPlatform.loadAndShowConsentFormIfRequired(act) { e ->
                if (e != null) c.resumeWithException(formException(e)) else c.resume(kotlin.Unit)
            }
        }
        now()
    }

    override fun currentConsent(): Consent = now()

    override suspend fun privacyOptions(): Consent = withContext(Dispatchers.Main) {
        val act = activity()
        suspendCancellableCoroutine<kotlin.Unit> { c ->
            UserMessagingPlatform.showPrivacyOptionsForm(act) { e ->
                if (e != null) c.resumeWithException(formException(e)) else c.resume(kotlin.Unit)
            }
        }
        now()
    }

    override fun resetConsent() = info().reset()

    override suspend fun load(format: Format, unit: String, request: Request): Loaded = withContext(Dispatchers.Main) {
        val ctx = AsukaModules.activity ?: AsukaModules.context
        val r = adRequest(request)
        val ssv = if (request.userId.isEmpty() && request.customData.isEmpty()) null else
            ServerSideVerificationOptions.Builder().setUserId(request.userId).setCustomData(request.customData).build()
        suspendCancellableCoroutine<Loaded> { c ->
            fun failed(e: LoadAdError) = c.resumeWithException(adsException(e.code, e.domain, e.message))
            when (format) {
                Format.INTERSTITIAL -> InterstitialAd.load(ctx, unit, r, object : InterstitialAdLoadCallback() {
                    override fun onAdLoaded(ad: InterstitialAd) = c.resume(FullScreen(Ad.Interstitial(ad)))
                    override fun onAdFailedToLoad(e: LoadAdError) = failed(e)
                })
                Format.REWARDED -> RewardedAd.load(ctx, unit, r, object : RewardedAdLoadCallback() {
                    override fun onAdLoaded(ad: RewardedAd) {
                        ssv?.let { ad.setServerSideVerificationOptions(it) }
                        c.resume(FullScreen(Ad.Rewarded(ad)))
                    }
                    override fun onAdFailedToLoad(e: LoadAdError) = failed(e)
                })
                Format.REWARDED_INTERSTITIAL -> RewardedInterstitialAd.load(ctx, unit, r, object : RewardedInterstitialAdLoadCallback() {
                    override fun onAdLoaded(ad: RewardedInterstitialAd) {
                        ssv?.let { ad.setServerSideVerificationOptions(it) }
                        c.resume(FullScreen(Ad.RewardedInterstitial(ad)))
                    }
                    override fun onAdFailedToLoad(e: LoadAdError) = failed(e)
                })
                Format.APP_OPEN -> AppOpenAd.load(ctx, unit, r, object : AppOpenAd.AppOpenAdLoadCallback() {
                    override fun onAdLoaded(ad: AppOpenAd) = c.resume(FullScreen(Ad.AppOpen(ad)))
                    override fun onAdFailedToLoad(e: LoadAdError) = failed(e)
                })
                else -> c.resumeWithException(AdsException(AdsError.INVALID_REQUEST))
            }
        }
    }

    companion object {
        /** Done once the SDK has started (start's first call). */
        @Volatile var started: CompletableDeferred<kotlin.Unit>? = null

        fun info(): ConsentInformation = UserMessagingPlatform.getConsentInformation(AsukaModules.context)

        fun now(): Consent {
            val i = info()
            return Consent(
                status = when (i.consentStatus) {
                    ConsentInformation.ConsentStatus.REQUIRED -> ConsentStatus.REQUIRED
                    ConsentInformation.ConsentStatus.NOT_REQUIRED -> ConsentStatus.NOT_REQUIRED
                    ConsentInformation.ConsentStatus.OBTAINED -> ConsentStatus.OBTAINED
                    else -> ConsentStatus.UNKNOWN
                },
                canRequestAds = i.canRequestAds(),
                privacyOptionsRequired = i.privacyOptionsRequirementStatus == ConsentInformation.PrivacyOptionsRequirementStatus.REQUIRED,
            )
        }

        fun activity(): Activity = AsukaModules.activity ?: throw AsukaUnsupportedException("ads: no activity shows the app")

        fun formException(e: FormError) = Exception("ads: consent: ${e.message} (${e.errorCode})")
    }
}

/** The SDK's errors as the package's, where one says it. */
private fun adsException(code: Int, domain: String, message: String): Exception = when (code) {
    AdRequest.ERROR_CODE_NO_FILL, AdRequest.ERROR_CODE_MEDIATION_NO_FILL -> AdsException(AdsError.NO_FILL)
    AdRequest.ERROR_CODE_NETWORK_ERROR -> AdsException(AdsError.NETWORK)
    AdRequest.ERROR_CODE_INVALID_REQUEST, AdRequest.ERROR_CODE_APP_ID_MISSING -> AdsException(AdsError.INVALID_REQUEST)
    else -> Exception("ads: $message ($domain $code)")
}

private fun adRequest(r: Request): AdRequest {
    val b = AdRequest.Builder()
    for (k in r.keywords) b.addKeyword(k)
    if (r.contentURL.isNotEmpty()) b.setContentUrl(r.contentURL)
    return b.build()
}

private sealed class Ad {
    class Interstitial(val ad: InterstitialAd) : Ad()
    class Rewarded(val ad: RewardedAd) : Ad()
    class RewardedInterstitial(val ad: RewardedInterstitialAd) : Ad()
    class AppOpen(val ad: AppOpenAd) : Ad()
}

/** A full-screen ad, loaded: shown once, over the app's activity. */
private class FullScreen(private var ad: Ad?) : Loaded {
    private var shown = false
    private var showing = false

    override suspend fun show(): Reward? = withContext(Dispatchers.Main) {
        val a = ad
        if (a == null || shown) throw AdsException(AdsError.ALREADY_SHOWN)
        val act = AsukaModules.activity ?: throw AsukaUnsupportedException("ads: no activity shows the app")
        shown = true
        showing = true
        var earned: Reward? = null
        try {
            suspendCancellableCoroutine<Reward?> { c ->
                val cb = object : FullScreenContentCallback() {
                    override fun onAdDismissedFullScreenContent() { if (c.isActive) c.resume(earned) }
                    override fun onAdFailedToShowFullScreenContent(e: AdError) {
                        if (c.isActive) c.resumeWithException(if (e.code == 1) AdsException(AdsError.ALREADY_SHOWN) else adsException(e.code, e.domain, e.message))
                    }
                }
                when (a) {
                    is Ad.Interstitial -> { a.ad.fullScreenContentCallback = cb; a.ad.show(act) }
                    is Ad.AppOpen -> { a.ad.fullScreenContentCallback = cb; a.ad.show(act) }
                    is Ad.Rewarded -> { a.ad.fullScreenContentCallback = cb; a.ad.show(act) { earned = Reward(it.type, it.amount.toLong()) } }
                    is Ad.RewardedInterstitial -> { a.ad.fullScreenContentCallback = cb; a.ad.show(act) { earned = Reward(it.type, it.amount.toLong()) } }
                }
            }
        } finally {
            showing = false
            ad = null
        }
    }

    override fun close() {
        // Kept while it shows: let go once it's closed.
        if (!showing) ad = null
    }
}

/**
 * A banner: Google's AdView in a box Asuka lays out, made once the box is
 * attached and has its width (again when the width, the unit, the size or
 * the request changes): an AdView's size is set once.
 */
private class GoogleBanner(private val events: BannerEvents) : Banner {
    private val context = events.context
    private var unit = ""
    private var size = BannerSize.ADAPTIVE
    private var request = Request()
    private var ad: AdView? = null
    private var stale = true
    private var loadedFor = -1

    private val box = object : FrameLayout(context) {
        override fun onMeasure(widthSpec: Int, heightSpec: Int) {
            // At most the screen's width (a measure without a width proposes no limit).
            val screen = resources.displayMetrics.widthPixels
            val w = if (MeasureSpec.getMode(widthSpec) == MeasureSpec.UNSPECIFIED) screen else minOf(MeasureSpec.getSize(widthSpec), screen)
            val s = adSize(w)
            val width = if (size == BannerSize.ADAPTIVE) w else s.getWidthInPixels(context)
            setMeasuredDimension(width, s.getHeightInPixels(context))
            for (i in 0 until childCount) {
                val c = getChildAt(i)
                c.measure(MeasureSpec.makeMeasureSpec(s.getWidthInPixels(context), MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(s.getHeightInPixels(context), MeasureSpec.EXACTLY))
            }
        }

        override fun onLayout(changed: Boolean, l: Int, t: Int, r: Int, b: Int) {
            for (i in 0 until childCount) {
                val c = getChildAt(i)
                val x = (r - l - c.measuredWidth) / 2
                c.layout(x, 0, x + c.measuredWidth, c.measuredHeight)
            }
            post { loadIfNeeded() }
        }
    }

    override val view: View get() = box

    init {
        box.addOnAttachStateChangeListener(object : View.OnAttachStateChangeListener {
            override fun onViewAttachedToWindow(v: View) { ad?.resume(); box.requestLayout() }
            override fun onViewDetachedFromWindow(v: View) { ad?.pause() }
        })
    }

    override fun setUnit(value: String) { unit = value; changed() }
    override fun setSize(value: BannerSize) { size = value; changed() }
    override fun setRequest(value: Request) { request = value; changed() }

    private fun changed() {
        stale = true
        box.requestLayout()
    }

    /** The ad's size for a width in pixels. */
    private fun adSize(widthPx: Int): AdSize = when (size) {
        BannerSize.BANNER -> AdSize.BANNER
        BannerSize.LARGE_BANNER -> AdSize.LARGE_BANNER
        BannerSize.MEDIUM_RECTANGLE -> AdSize.MEDIUM_RECTANGLE
        BannerSize.FULL_BANNER -> AdSize.FULL_BANNER
        BannerSize.LEADERBOARD -> AdSize.LEADERBOARD
        else -> AdSize.getCurrentOrientationAnchoredAdaptiveBannerAdSize(context, (widthPx / context.resources.displayMetrics.density).toInt())
    }

    private fun loadIfNeeded() {
        val w = box.width
        if (!box.isAttachedToWindow || w <= 0 || unit.isEmpty()) return
        if (size == BannerSize.ADAPTIVE && w != loadedFor) stale = true
        if (!stale) return
        stale = false
        loadedFor = w
        ad?.let { box.removeView(it); it.destroy() }
        val v = AdView(AsukaModules.activity ?: context)
        v.setAdSize(adSize(w))
        v.adUnitId = unit
        v.adListener = object : AdListener() {
            override fun onAdLoaded() = events.loaded()
            override fun onAdFailedToLoad(e: LoadAdError) {
                val ex = adsException(e.code, e.domain, e.message)
                events.error((ex as? AdsException)?.error?.message ?: ex.message ?: e.message)
            }
            override fun onAdImpression() = events.impression()
            override fun onAdClicked() = events.click()
            override fun onAdOpened() = events.open()
            override fun onAdClosed() = events.close()
        }
        box.addView(v)
        ad = v
        v.loadAd(adRequest(request))
    }
}

fun makeMobile(): Mobile = GoogleMobile()

fun makeLoaded(): Loaded = throw AsukaUnsupportedException("ads: a full-screen ad is loaded with interstitial(), rewarded()...")

fun makeBanner(events: BannerEvents): Banner = GoogleBanner(events)
