// platform/pay on Android: Google Pay (play-services-wallet). available
// asks isReadyToPay, and with existingPaymentMethodRequired whether the
// user has a card; pay runs loadPaymentData through the Activity Result
// API, the PaymentDataRequest made here from the package's request.
// Google Pay's sheet calls PayCallbacks (the module's manifest) as the
// user changes the address, the option or the card, and pays: each call
// is asked of the app, and completed with its answer, as Google's JSON.
package platform.pay

import android.content.Context
import android.content.res.Configuration
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.FrameLayout
import androidx.activity.ComponentActivity
import androidx.activity.result.ActivityResultLauncher
import com.google.android.gms.common.api.CommonStatusCodes
import com.google.android.gms.tasks.Task
import com.google.android.gms.wallet.IsReadyToPayRequest
import com.google.android.gms.wallet.PaymentData
import com.google.android.gms.wallet.PaymentDataRequest
import com.google.android.gms.wallet.PaymentsClient
import com.google.android.gms.wallet.Wallet
import com.google.android.gms.wallet.WalletConstants
import com.google.android.gms.wallet.button.ButtonConstants
import com.google.android.gms.wallet.button.ButtonOptions
import com.google.android.gms.wallet.callback.BasePaymentDataCallbacks
import com.google.android.gms.wallet.callback.BasePaymentDataCallbacksService
import com.google.android.gms.wallet.callback.IntermediatePaymentData
import com.google.android.gms.wallet.callback.OnCompleteListener
import com.google.android.gms.wallet.callback.PaymentAuthorizationResult
import com.google.android.gms.wallet.callback.PaymentDataRequestUpdate
import com.google.android.gms.wallet.contract.TaskResultContracts
import dev.asuka.AsukaModules
import kotlinx.coroutines.suspendCancellableCoroutine
import org.json.JSONArray
import org.json.JSONObject
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import com.google.android.gms.wallet.button.PayButton as GooglePayButton

private val main = Handler(Looper.getMainLooper())

/** A PaymentsClient in the request's environment, its sheet themed as the app is. */
private fun client(production: Boolean): PaymentsClient {
    val context: Context = AsukaModules.context
    val night = context.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK == Configuration.UI_MODE_NIGHT_YES
    val o = Wallet.WalletOptions.Builder()
        .setEnvironment(if (production) WalletConstants.ENVIRONMENT_PRODUCTION else WalletConstants.ENVIRONMENT_TEST)
        .setTheme(if (night) WalletConstants.THEME_DARK else WalletConstants.THEME_LIGHT)
        .build()
    val act = AsukaModules.activity
    return if (act != null) Wallet.getPaymentsClient(act, o) else Wallet.getPaymentsClient(context, o)
}

class GooglePayWallets : Wallets {
    override suspend fun available(request: Sheet): Availability {
        val ready = GPay.ready(request)
        val c = client(request.google.production)
        val can = ask(c, ready.put("existingPaymentMethodRequired", false))
        if (!can) return Availability(wallet = Wallet_.GOOGLE_PAY, canPay = false)
        val card = ask(c, ready.put("existingPaymentMethodRequired", true))
        return Availability(wallet = Wallet_.GOOGLE_PAY, canPay = true, hasCard = card)
    }

    private suspend fun ask(c: PaymentsClient, r: JSONObject): Boolean = suspendCancellableCoroutine { k ->
        c.isReadyToPay(IsReadyToPayRequest.fromJson(r.toString())).addOnCompleteListener { t ->
            if (k.isActive) k.resume(t.isSuccessful && t.result == true)
        }
    }

    override suspend fun pay(request: Sheet, asked: (Ask) -> Unit): Payment = suspendCancellableCoroutine { k ->
        main.post { Session.present(request, asked) { r -> if (k.isActive) r.fold({ k.resume(it) }, { k.resumeWithException(it) }) } }
    }

    override fun answer(id: Long, answer: Answer) {
        main.post { Session.answered(id, answer) }
    }

    override suspend fun setUp() = throw PayException(PayError.NOT_AVAILABLE)
}

/** The package's Wallet enum, beside Google's Wallet class. */
private typealias Wallet_ = platform.pay.Wallet

/** The sheet that shows: its questions waiting for the app's answers. Main thread. */
internal object Session {
    private var showing = false
    private var request = Sheet()
    private var asked: ((Ask) -> Unit)? = null
    private var done: ((kotlin.Result<Payment>) -> Unit)? = null
    private var next = 0L
    private val pending = HashMap<Long, (Answer) -> Unit>()
    private var items: List<Item> = emptyList()
    private var total = Item()
    private var methods: List<ShippingMethod> = emptyList()
    private var selected = ""
    private var paid: Payment? = null
    private var launches = 0

    fun present(r: Sheet, ask: (Ask) -> Unit, finish: (kotlin.Result<Payment>) -> Unit) {
        if (showing) return finish(kotlin.Result.failure(PayException(PayError.BUSY)))
        val act = AsukaModules.activity as? ComponentActivity ?: return finish(kotlin.Result.failure(Exception("pay: no screen to show Google Pay's sheet on")))
        val json = runCatching { GPay.request(r) }.getOrElse { return finish(kotlin.Result.failure(it)) }
        val pdr = runCatching { PaymentDataRequest.fromJson(json.toString()) }
            .getOrElse { return finish(kotlin.Result.failure(Exception("pay: Google Pay's request: ${it.message}"))) }
        request = r
        asked = ask
        done = finish
        items = r.items
        total = r.total
        methods = r.shippingMethods
        selected = methods.firstOrNull()?.id ?: ""
        paid = null
        pending.clear()
        showing = true
        var l: ActivityResultLauncher<Task<PaymentData>>? = null
        l = act.activityResultRegistry.register("asuka-pay-${launches++}", TaskResultContracts.GetPaymentDataResult()) { res ->
            l?.unregister()
            val code = res.status.statusCode
            val data = res.result
            if (code == CommonStatusCodes.SUCCESS && paid == null && data != null) {
                // Paid without the callback (Google Pay didn't call it): the
                // app authorizes it now.
                val p = GPay.payment(JSONObject(data.toJson()))
                send(Ask(kind = AskKind.AUTHORIZE, payment = p)) { a -> end(if (a.failed || a.update.errors.isNotEmpty()) null else p, a.message) }
                return@register
            }
            val error = when (code) {
                CommonStatusCodes.SUCCESS, CommonStatusCodes.CANCELED -> ""
                else -> res.status.statusMessage ?: CommonStatusCodes.getStatusCodeString(code)
            }
            end(paid, error)
        }
        try {
            l.launch(client(r.google.production).loadPaymentData(pdr))
        } catch (e: Exception) {
            l.unregister()
            showing = false
            done = null
            finish(kotlin.Result.failure(e))
        }
    }

    private fun end(p: Payment?, error: String) {
        if (!showing) return
        showing = false
        pending.clear()
        val d = done
        done = null
        when {
            p != null -> d?.invoke(kotlin.Result.success(p))
            error.isNotEmpty() -> d?.invoke(kotlin.Result.failure(Exception("pay: $error")))
            else -> d?.invoke(kotlin.Result.failure(PayException(PayError.CANCELED)))
        }
    }

    private fun send(a: Ask, then: (Answer) -> Unit) {
        next++
        pending[next] = then
        asked?.invoke(a.copy(id = next))
    }

    fun answered(id: Long, a: Answer) {
        pending.remove(id)?.invoke(a)
    }

    private fun keep(a: Answer) {
        a.update.items?.let { items = it }
        a.update.total?.let { t -> total = Item(label = t.label.ifEmpty { total.label }, amount = t.amount.ifEmpty { total.amount }, pending = t.pending, kind = t.kind) }
        a.update.shippingMethods?.let { ms ->
            methods = ms
            if (ms.none { it.id == selected }) selected = ms.firstOrNull()?.id ?: ""
        }
    }

    /** IntermediatePaymentData, answered with a PaymentDataRequestUpdate. Any thread. */
    fun changed(data: JSONObject, complete: (String) -> Unit) = main.post {
        if (!showing) return@post complete("{}")
        data.optJSONObject("shippingOptionData")?.optString("id")?.takeIf { it.isNotEmpty() && it != "shipping_option_unselected" }?.let { selected = it }
        val trigger = data.optString("callbackTrigger")
        val ask: Ask? = when (trigger) {
            "INITIALIZE", "SHIPPING_ADDRESS" -> data.optJSONObject("shippingAddress")?.takeIf { AskKind.SHIPPING_CONTACT in request.asks }?.let {
                Ask(kind = AskKind.SHIPPING_CONTACT, contact = GPay.contact(it))
            }
            "SHIPPING_OPTION" -> methods.firstOrNull { it.id == selected }?.takeIf { AskKind.SHIPPING_METHOD in request.asks }?.let {
                Ask(kind = AskKind.SHIPPING_METHOD, method = it)
            }
            "PAYMENT_METHOD" -> data.optJSONObject("paymentMethodData")?.takeIf { AskKind.PAYMENT_METHOD in request.asks }?.let {
                Ask(kind = AskKind.PAYMENT_METHOD, card = GPay.card(it))
            }
            else -> null
        }
        val intent = if (trigger == "INITIALIZE") "SHIPPING_ADDRESS" else trigger
        val reply = { a: Answer? ->
            if (a != null) keep(a)
            val u = JSONObject()
            GPay.error(a?.update?.errors ?: emptyList(), intent)?.let { u.put("error", it) }
            if (request.recurring == null && request.deferred == null && request.automaticReload == null) {
                u.put("newTransactionInfo", GPay.transaction(request, items, total))
            }
            if (methods.isNotEmpty()) u.put("newShippingOptionParameters", GPay.options(methods, selected))
            complete(u.toString())
        }
        if (ask == null) reply(null) else send(ask) { reply(it) }
    }

    /** PaymentData, answered with a PaymentAuthorizationResult. Any thread. */
    fun authorized(data: JSONObject, complete: (String) -> Unit) = main.post {
        val gone = """{"transactionState":"ERROR","error":{"reason":"OTHER_ERROR","message":"Try again","intent":"PAYMENT_AUTHORIZATION"}}"""
        if (!showing) return@post complete(gone)
        val p = GPay.payment(data).copy(shippingMethod = selected)
        send(Ask(kind = AskKind.AUTHORIZE, payment = p)) { a ->
            val errs = if (a.failed && a.update.errors.isEmpty()) listOf(FieldError(field = Field.PAYMENT, message = a.message)) else a.update.errors
            if (errs.isEmpty()) {
                paid = p
                complete("""{"transactionState":"SUCCESS"}""")
            } else {
                complete(JSONObject().put("transactionState", "ERROR").put("error", GPay.error(errs, "PAYMENT_AUTHORIZATION")).toString())
            }
        }
    }
}

/** The service Google Pay's sheet calls (this module's manifest). */
class PayCallbacks : BasePaymentDataCallbacksService() {
    override fun createPaymentDataCallbacks(): BasePaymentDataCallbacks = object : BasePaymentDataCallbacks() {
        override fun onPaymentDataChanged(data: IntermediatePaymentData?, listener: OnCompleteListener<PaymentDataRequestUpdate>) {
            Session.changed(JSONObject(data?.toJson() ?: "{}")) { listener.complete(PaymentDataRequestUpdate.fromJson(it)) }
        }

        override fun onPaymentAuthorized(data: PaymentData?, listener: OnCompleteListener<PaymentAuthorizationResult>) {
            Session.authorized(JSONObject(data?.toJson() ?: "{}")) { listener.complete(PaymentAuthorizationResult.fromJson(it)) }
        }
    }
}

/** The package's request as Google Pay's JSON, and Google's answers as the package's types. */
internal object GPay {
    private val networks = mapOf(
        Network.VISA to "VISA", Network.MASTERCARD to "MASTERCARD", Network.AMEX to "AMEX", Network.DISCOVER to "DISCOVER",
        Network.JCB to "JCB", Network.INTERAC to "INTERAC", Network.MAESTRO to "MAESTRO", Network.ELECTRON to "ELECTRON", Network.ELO to "ELO",
    )

    private fun card(r: Sheet): JSONObject {
        val ns = (if (r.networks.isEmpty()) listOf(Network.VISA, Network.MASTERCARD, Network.AMEX, Network.DISCOVER) else r.networks).mapNotNull { networks[it] }
        val c = JSONObject()
            .put("allowedAuthMethods", JSONArray(if (r.google.deviceTokensOnly) listOf("CRYPTOGRAM_3DS") else listOf("PAN_ONLY", "CRYPTOGRAM_3DS")))
            .put("allowedCardNetworks", JSONArray(ns))
        if (r.cards.isNotEmpty()) {
            if (CardKind.CREDIT !in r.cards) c.put("allowCreditCards", false)
            if (CardKind.PREPAID !in r.cards) c.put("allowPrepaidCards", false)
        }
        if (r.cardCountries.isNotEmpty()) c.put("allowedIssuerCountryCodes", JSONArray(r.cardCountries))
        if (r.google.assuranceDetails) c.put("assuranceDetailsRequired", true)
        if (r.billing.any { it == ContactField.NAME || it == ContactField.ADDRESS || it == ContactField.PHONE }) {
            c.put("billingAddressRequired", true)
            c.put("billingAddressParameters", JSONObject().put("format", if (ContactField.ADDRESS in r.billing) "FULL" else "MIN")
                .put("phoneNumberRequired", ContactField.PHONE in r.billing))
        }
        return c
    }

    fun ready(r: Sheet): JSONObject = JSONObject().put("apiVersion", 2).put("apiVersionMinor", 0)
        .put("allowedPaymentMethods", JSONArray().put(JSONObject().put("type", "CARD").put("parameters", card(r))))

    private val itemTypes = listOf("LINE_ITEM", "SUBTOTAL", "TAX", "DISCOUNT", "SHIPPING_OPTION")

    private fun items(its: List<Item>) = JSONArray(its.map { i ->
        JSONObject().put("label", i.label).put("type", itemTypes[(i.kind.value % 5).toInt()]).put("price", i.amount)
            .put("status", if (i.pending) "PENDING" else "FINAL")
    })

    private fun priceStatus(pending: Boolean, amount: String) = when {
        amount.isEmpty() -> "NOT_CURRENTLY_KNOWN"
        pending -> "ESTIMATED"
        else -> "FINAL"
    }

    /** "2026-11-20" as RFC 3339, midnight UTC. */
    private fun rfc3339(day: String) = if (day.isEmpty()) "" else "${day}T00:00:00Z"

    fun transaction(r: Sheet, its: List<Item>, total: Item): JSONObject {
        val t = JSONObject().put("currencyCode", r.currency).put("countryCode", r.country).put("totalPrice", total.amount)
            .put("totalPriceStatus", if (total.pending) "ESTIMATED" else "FINAL")
            .put("checkoutOption", if (!total.pending && r.google.payNow) "COMPLETE_IMMEDIATE_PURCHASE" else "DEFAULT")
            .put("displayItems", items(its))
        if (r.google.transactionId.isNotEmpty()) t.put("transactionId", r.google.transactionId)
        if (its.isNotEmpty()) t.put("totalPriceLabel", r.google.totalLabel.ifEmpty { "Total" })
        return t
    }

    fun options(ms: List<ShippingMethod>, selected: String) = JSONObject().put("defaultSelectedOptionId", selected)
        .put("shippingOptions", JSONArray(ms.map { JSONObject().put("id", it.id).put("label", it.label).put("description", it.detail) }))

    private fun info(r: Sheet, x: JSONObject): JSONObject {
        x.put("currencyCode", r.currency).put("countryCode", r.country).put("immediateTotalPrice", r.total.amount).put("immediateDisplayItems", items(r.items))
        if (r.google.transactionId.isNotEmpty()) x.put("transactionId", r.google.transactionId)
        return x
    }

    /** The request as Google Pay takes it (a PaymentDataRequest). */
    fun request(r: Sheet): JSONObject {
        val m = JSONObject().put("type", "CARD").put("parameters", card(r))
        when {
            r.google.gateway.isNotEmpty() -> {
                val params = JSONObject().put("gateway", r.google.gateway)
                for ((k, v) in r.google.gatewayParams) params.put(k, v)
                m.put("tokenizationSpecification", JSONObject().put("type", "PAYMENT_GATEWAY").put("parameters", params))
            }
            r.google.directKey.isNotEmpty() -> m.put("tokenizationSpecification", JSONObject().put("type", "DIRECT")
                .put("parameters", JSONObject().put("protocolVersion", "ECv2").put("publicKey", r.google.directKey)))
            else -> throw Exception("pay: Request.google has no gateway (or directKey): the processor's gateway that tokenizes the card")
        }
        if (r.google.production && r.google.merchantId.isEmpty()) throw Exception("pay: Request.google.merchantId is empty, which production needs")
        val intents = mutableListOf("PAYMENT_AUTHORIZATION")
        val g = JSONObject().put("apiVersion", 2).put("apiVersionMinor", 0).put("allowedPaymentMethods", JSONArray().put(m))
            .put("merchantInfo", JSONObject().put("merchantId", r.google.merchantId).put("merchantName", r.google.merchantName.ifEmpty { r.total.label }))
            .put("emailRequired", ContactField.EMAIL in r.shipping || ContactField.EMAIL in r.billing)
        if (r.shipping.any { it == ContactField.ADDRESS || it == ContactField.NAME || it == ContactField.PHONE } || r.shippingMethods.isNotEmpty()) {
            g.put("shippingAddressRequired", true)
            val p = JSONObject().put("phoneNumberRequired", ContactField.PHONE in r.shipping)
            if (r.shippingCountries.isNotEmpty()) p.put("allowedCountryCodes", JSONArray(r.shippingCountries))
            g.put("shippingAddressParameters", p)
            if (AskKind.SHIPPING_CONTACT in r.asks) intents.add("SHIPPING_ADDRESS")
        }
        if (r.shippingMethods.isNotEmpty()) {
            g.put("shippingOptionRequired", true).put("shippingOptionParameters", options(r.shippingMethods, r.shippingMethods[0].id))
            if (AskKind.SHIPPING_METHOD in r.asks) intents.add("SHIPPING_OPTION")
        }
        if (AskKind.PAYMENT_METHOD in r.asks) intents.add("PAYMENT_METHOD")
        g.put("callbackIntents", JSONArray(intents))
        val rec = r.recurring
        val def = r.deferred
        val rel = r.automaticReload
        when {
            rec != null -> {
                val x = info(r, JSONObject()).put("managementUrl", rec.managementURL).put("billingAgreement", rec.billingAgreement)
                if (rec.tokenNotificationURL.isNotEmpty()) x.put("tokenUpdateUrl", rec.tokenNotificationURL)
                rec.trial?.let { t ->
                    x.put("introductoryPeriod", JSONObject().put("start", rfc3339(t.start)).put("end", rfc3339(t.end)).put("label", t.label)
                        .put("totalPrice", t.amount.ifEmpty { "0.00" }))
                }
                val b = rec.regular
                x.put("recurrenceItems", JSONArray().put(JSONObject().put("start", rfc3339(b.start)).put("end", rfc3339(b.end)).put("label", b.label)
                    .put("price", b.amount).put("priceStatus", priceStatus(b.pending, b.amount))
                    .put("period", listOf("MONTH", "DAY", "WEEK", "YEAR")[(b.interval.value % 4).toInt()]).put("periodCount", maxOf(b.every, 1))))
                g.put("recurringTransactionInfo", x)
            }
            def != null -> g.put("deferredTransactionInfo", info(r, JSONObject()).put("managementUrl", def.managementURL)
                .put("billingAgreement", def.billingAgreement).put("billingDateTime", rfc3339(def.date)).put("label", def.billing.label)
                .put("price", def.billing.amount).put("priceStatus", priceStatus(def.billing.pending, def.billing.amount)))
            rel != null -> g.put("automaticReloadTransactionInfo", info(r, JSONObject()).put("managementUrl", rel.managementURL)
                .put("billingAgreement", rel.billingAgreement).put("minimumBalanceAmount", rel.threshold).put("reloadAmount", rel.billing.amount)
                .put("label", rel.billing.label))
            else -> g.put("transactionInfo", transaction(r, r.items, r.total))
        }
        return g
    }

    fun error(errs: List<FieldError>, intent: String): JSONObject? {
        val e = errs.firstOrNull() ?: return null
        var reason = "OTHER_ERROR"
        var i = intent
        when (e.field) {
            Field.UNSERVICEABLE -> { reason = "SHIPPING_ADDRESS_UNSERVICEABLE"; i = "SHIPPING_ADDRESS" }
            Field.SHIPPING_METHOD -> { reason = "SHIPPING_OPTION_INVALID"; i = "SHIPPING_OPTION" }
            Field.SHIPPING_ADDRESS, Field.SHIPPING_STREET, Field.SHIPPING_CITY, Field.SHIPPING_STATE, Field.SHIPPING_POSTAL_CODE,
            Field.SHIPPING_COUNTRY, Field.SHIPPING_NAME, Field.SHIPPING_EMAIL, Field.SHIPPING_PHONE -> { reason = "SHIPPING_ADDRESS_INVALID"; i = "SHIPPING_ADDRESS" }
            else -> {
                reason = "PAYMENT_DATA_INVALID"
                if (intent != "PAYMENT_METHOD" && intent != "PAYMENT_AUTHORIZATION") i = "PAYMENT_AUTHORIZATION"
            }
        }
        return JSONObject().put("reason", reason).put("message", e.message).put("intent", i)
    }

    fun contact(a: JSONObject?, email: String = ""): Contact {
        if (a == null) return Contact(email = email)
        val lines = listOf("address1", "address2", "address3").map { a.optString(it) }.filter { it.isNotEmpty() }
        return Contact(name = a.optString("name"), email = email, phone = a.optString("phoneNumber"),
            address = PostalAddress(lines = lines, city = a.optString("locality"), state = a.optString("administrativeArea"),
                postalCode = a.optString("postalCode"), countryCode = a.optString("countryCode").uppercase(), sortingCode = a.optString("sortingCode")))
    }

    fun card(m: JSONObject): Card {
        val info = m.optJSONObject("info") ?: JSONObject()
        val net = info.optString("cardNetwork").lowercase()
        val network = networks.entries.firstOrNull { it.value.lowercase() == net }?.key ?: Network.OTHER
        val details = info.optString("cardDetails")
        val kind = when (info.optJSONObject("cardFundingSource")?.toString() ?: info.optString("cardFundingSource")) {
            "CREDIT" -> CardKind.CREDIT
            "DEBIT" -> CardKind.DEBIT
            "PREPAID" -> CardKind.PREPAID
            else -> CardKind.UNKNOWN
        }
        return Card(network = network, kind = kind, name = m.optString("description").ifEmpty { "${info.optString("cardNetwork")} •••• $details" },
            last4 = details, billing = contact(info.optJSONObject("billingAddress")))
    }

    fun payment(d: JSONObject): Payment {
        val m = d.optJSONObject("paymentMethodData") ?: JSONObject()
        return Payment(wallet = Wallet_.GOOGLE_PAY, token = m.optJSONObject("tokenizationData")?.optString("token") ?: "",
            card = card(m), shipping = contact(d.optJSONObject("shippingAddress"), d.optString("email")),
            billing = m.optJSONObject("info")?.optJSONObject("billingAddress")?.let { contact(it, d.optString("email")) } ?: Contact(),
            shippingMethod = d.optJSONObject("shippingOptionData")?.optString("id") ?: "", raw = d.toString())
    }
}

/** Google's PayButton, made again when its props change (its options are fixed once initialized), and for automatic, as the app goes dark or light. */
class AndroidPayButton(private val events: PayButtonEvents) : PayButton {
    private var type = ButtonType.BUY
    private var variant = ButtonVariant.AUTOMATIC
    private var radius = -1.0
    private var networks: List<Network> = emptyList()
    private var madeDark = false
    private var dirty = false

    private val box: FrameLayout = object : FrameLayout(events.context) {
        override fun onMeasure(widthSpec: Int, heightSpec: Int) {
            val d = resources.displayMetrics.density
            val w = if (MeasureSpec.getMode(widthSpec) == MeasureSpec.UNSPECIFIED) (300 * d).toInt() else MeasureSpec.getSize(widthSpec)
            val h = if (MeasureSpec.getMode(heightSpec) == MeasureSpec.EXACTLY) MeasureSpec.getSize(heightSpec) else (48 * d).toInt()
            super.onMeasure(MeasureSpec.makeMeasureSpec(w, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(h, MeasureSpec.EXACTLY))
        }

        override fun onConfigurationChanged(c: Configuration?) {
            super.onConfigurationChanged(c)
            if (variant == ButtonVariant.AUTOMATIC && dark() != madeDark) post { make() }
        }
    }

    override val view: View get() = box

    init {
        make()
    }

    private fun dark(): Boolean = box.resources.configuration.uiMode and Configuration.UI_MODE_NIGHT_MASK == Configuration.UI_MODE_NIGHT_YES

    override fun setType(value: ButtonType) { type = value; remake() }
    override fun setVariant(value: ButtonVariant) { variant = value; remake() }
    override fun setCornerRadius(value: Double) { radius = value; remake() }
    override fun setNetworks(value: List<Network>) { networks = value; remake() }

    /** The props come together: made once they have. */
    private fun remake() {
        if (dirty) return
        dirty = true
        box.post { dirty = false; make() }
    }

    private fun googleType(t: ButtonType) = when (t) {
        ButtonType.PLAIN, ButtonType.SET_UP -> ButtonConstants.ButtonType.PLAIN
        ButtonType.BOOK, ButtonType.RENT -> ButtonConstants.ButtonType.BOOK
        ButtonType.CHECKOUT -> ButtonConstants.ButtonType.CHECKOUT
        ButtonType.DONATE, ButtonType.SUPPORT, ButtonType.CONTRIBUTE -> ButtonConstants.ButtonType.DONATE
        ButtonType.ORDER -> ButtonConstants.ButtonType.ORDER
        ButtonType.SUBSCRIBE -> ButtonConstants.ButtonType.SUBSCRIBE
        ButtonType.BUY -> ButtonConstants.ButtonType.BUY
        else -> ButtonConstants.ButtonType.PAY
    }

    private fun make() {
        madeDark = dark()
        val theme = when (variant) {
            ButtonVariant.BLACK -> ButtonConstants.ButtonTheme.DARK
            ButtonVariant.WHITE, ButtonVariant.WHITE_OUTLINE -> ButtonConstants.ButtonTheme.LIGHT
            else -> if (madeDark) ButtonConstants.ButtonTheme.LIGHT else ButtonConstants.ButtonTheme.DARK
        }
        val methods = GPay.ready(Sheet(networks = networks)).getJSONArray("allowedPaymentMethods").toString()
        val o = ButtonOptions.newBuilder().setButtonType(googleType(type)).setButtonTheme(theme).setAllowedPaymentMethods(methods)
        if (radius >= 0) o.setCornerRadius(radius.toInt())
        val b = GooglePayButton(box.context)
        b.initialize(o.build())
        b.setOnClickListener { events.press() }
        box.removeAllViews()
        box.addView(b, FrameLayout.LayoutParams(FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT))
    }
}

fun makeWallets(): Wallets = GooglePayWallets()

fun makePayButton(events: PayButtonEvents): PayButton = AndroidPayButton(events)
