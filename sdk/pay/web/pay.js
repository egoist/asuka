// platform/pay in the browser:
//
//   - Apple Pay where the browser has it (Safari, the browsers of an
//     iPhone or iPad: ApplePaySession.canMakePayments), through the
//     Payment Request API's Apple Pay method. Its events
//     (merchantvalidation, shippingaddresschange, shippingoptionchange,
//     paymentmethodchange, couponcodechange) are asked of the app, whose
//     answers update the sheet; the response's retry shows the
//     authorization's errors. The merchant's validation is the app's
//     server's (onValidateMerchant), for a domain registered with the
//     merchant ID.
//   - Google Pay elsewhere, with Google's own web API (pay.js, loaded
//     once, from pay.google.com): the same PaymentDataRequest and
//     callbacks as on Android.
//   - The button: Safari's own Apple Pay button (the CSS
//     -apple-pay-button), or the one Google's API draws.
import { Wallet, ItemKind, Network, CardKind, ContactField, Field, AskKind, ButtonType, ButtonVariant, Interval, PayError } from "./pay.asuka.js";

const appleMethod = "https://apple.com/apple-pay";
const googleScript = "https://pay.google.com/gp/p/js/pay.js";

const name = (enumObj, v) => Object.keys(enumObj).find((k) => enumObj[k] === v);

const appleNetworks = {
  visa: "visa", mastercard: "masterCard", amex: "amex", discover: "discover", jcb: "jcb", interac: "interac",
  unionPay: "chinaUnionPay", maestro: "maestro", electron: "electron", elo: "elo", cartesBancaires: "cartesBancaires",
  eftpos: "eftpos", mada: "mada", mir: "mir", girocard: "girocard", bancontact: "bancontact", dankort: "dankort",
  privateLabel: "privateLabel",
};
const googleNetworks = { visa: "VISA", mastercard: "MASTERCARD", amex: "AMEX", discover: "DISCOVER", jcb: "JCB", interac: "INTERAC",
  maestro: "MAESTRO", electron: "ELECTRON", elo: "ELO" };
const appleFields = { name: "name", email: "email", phone: "phone", address: "postalAddress", phoneticName: "phoneticName" };

const defaultNetworks = [Network.visa, Network.mastercard, Network.amex, Network.discover];
const networksOf = (r) => (r.networks.length ? r.networks : defaultNetworks).map((n) => name(Network, n));

function hasApplePay() {
  try {
    return !!globalThis.ApplePaySession && globalThis.ApplePaySession.canMakePayments() && !!globalThis.PaymentRequest;
  } catch {
    return false;
  }
}

let googleLoad = null;

function loadGoogle() {
  if (globalThis.google?.payments?.api) return Promise.resolve(globalThis.google.payments.api);
  googleLoad ??= new Promise((resolve, reject) => {
    const s = document.createElement("script");
    s.src = googleScript;
    s.async = true;
    s.onload = () => (globalThis.google?.payments?.api ? resolve(globalThis.google.payments.api) : reject(new PayError("notAvailable")));
    s.onerror = () => {
      googleLoad = null;
      reject(new PayError("notAvailable"));
    };
    document.head.appendChild(s);
  });
  return googleLoad;
}

function googleClient(r, callbacks) {
  return loadGoogle().then((api) => new api.PaymentsClient({ environment: r.google.production ? "PRODUCTION" : "TEST", paymentDataCallbacks: callbacks }));
}

// --- Google Pay's JSON (as sdk/pay's Kotlin makes it) ---

function googleCard(r) {
  const c = {
    allowedAuthMethods: r.google.deviceTokensOnly ? ["CRYPTOGRAM_3DS"] : ["PAN_ONLY", "CRYPTOGRAM_3DS"],
    allowedCardNetworks: networksOf(r).map((n) => googleNetworks[n]).filter(Boolean),
  };
  if (r.cards.length) {
    if (!r.cards.includes(CardKind.credit)) c.allowCreditCards = false;
    if (!r.cards.includes(CardKind.prepaid)) c.allowPrepaidCards = false;
  }
  if (r.cardCountries.length) c.allowedIssuerCountryCodes = r.cardCountries;
  if (r.google.assuranceDetails) c.assuranceDetailsRequired = true;
  if (r.billing.some((f) => f === ContactField.name || f === ContactField.address || f === ContactField.phone)) {
    c.billingAddressRequired = true;
    c.billingAddressParameters = { format: r.billing.includes(ContactField.address) ? "FULL" : "MIN", phoneNumberRequired: r.billing.includes(ContactField.phone) };
  }
  return c;
}

const googleReady = (r) => ({ apiVersion: 2, apiVersionMinor: 0, allowedPaymentMethods: [{ type: "CARD", parameters: googleCard(r) }] });

const itemTypes = ["LINE_ITEM", "SUBTOTAL", "TAX", "DISCOUNT", "SHIPPING_OPTION"];
const googleItems = (its) => its.map((i) => ({ label: i.label, type: itemTypes[i.kind % 5], price: i.amount, status: i.pending ? "PENDING" : "FINAL" }));
const priceStatus = (pending, amount) => (!amount ? "NOT_CURRENTLY_KNOWN" : pending ? "ESTIMATED" : "FINAL");
const rfc3339 = (day) => (day ? `${day}T00:00:00Z` : undefined);

function googleTransaction(r, items, total) {
  const t = { currencyCode: r.currency, countryCode: r.country, totalPrice: total.amount, totalPriceStatus: total.pending ? "ESTIMATED" : "FINAL",
    checkoutOption: !total.pending && r.google.payNow ? "COMPLETE_IMMEDIATE_PURCHASE" : "DEFAULT", displayItems: googleItems(items) };
  if (r.google.transactionId) t.transactionId = r.google.transactionId;
  if (items.length) t.totalPriceLabel = r.google.totalLabel || "Total";
  return t;
}

const googleOptions = (ms, selected) => ({ defaultSelectedOptionId: selected, shippingOptions: ms.map((m) => ({ id: m.id, label: m.label, description: m.detail })) });

function googleRequest(r) {
  const m = { type: "CARD", parameters: googleCard(r) };
  if (r.google.gateway) m.tokenizationSpecification = { type: "PAYMENT_GATEWAY", parameters: { gateway: r.google.gateway, ...r.google.gatewayParams } };
  else if (r.google.directKey) m.tokenizationSpecification = { type: "DIRECT", parameters: { protocolVersion: "ECv2", publicKey: r.google.directKey } };
  else throw new Error("pay: Request.google has no gateway (or directKey): the processor's gateway that tokenizes the card");
  const intents = ["PAYMENT_AUTHORIZATION"];
  const g = { apiVersion: 2, apiVersionMinor: 0, allowedPaymentMethods: [m],
    merchantInfo: { merchantId: r.google.merchantId || undefined, merchantName: r.google.merchantName || r.total.label },
    emailRequired: r.shipping.includes(ContactField.email) || r.billing.includes(ContactField.email) };
  if (r.shipping.some((f) => f === ContactField.address || f === ContactField.name || f === ContactField.phone) || r.shippingMethods.length) {
    g.shippingAddressRequired = true;
    g.shippingAddressParameters = { phoneNumberRequired: r.shipping.includes(ContactField.phone) };
    if (r.shippingCountries.length) g.shippingAddressParameters.allowedCountryCodes = r.shippingCountries;
    if (r.asks.includes(AskKind.shippingContact)) intents.push("SHIPPING_ADDRESS");
  }
  if (r.shippingMethods.length) {
    g.shippingOptionRequired = true;
    g.shippingOptionParameters = googleOptions(r.shippingMethods, r.shippingMethods[0].id);
    if (r.asks.includes(AskKind.shippingMethod)) intents.push("SHIPPING_OPTION");
  }
  if (r.asks.includes(AskKind.paymentMethod)) intents.push("PAYMENT_METHOD");
  g.callbackIntents = intents;
  const info = (x) => ({ currencyCode: r.currency, countryCode: r.country, immediateTotalPrice: r.total.amount, immediateDisplayItems: googleItems(r.items),
    transactionId: r.google.transactionId || undefined, managementUrl: x.managementURL, billingAgreement: x.billingAgreement,
    tokenUpdateUrl: x.tokenNotificationURL || undefined });
  if (r.recurring) {
    const x = r.recurring, b = x.regular;
    g.recurringTransactionInfo = { ...info(x), recurrenceItems: [{ start: rfc3339(b.start), end: rfc3339(b.end), label: b.label, price: b.amount,
      priceStatus: priceStatus(b.pending, b.amount), period: ["MONTH", "DAY", "WEEK", "YEAR"][b.interval % 4], periodCount: Math.max(b.every, 1) }] };
    if (x.trial) g.recurringTransactionInfo.introductoryPeriod = { start: rfc3339(x.trial.start), end: rfc3339(x.trial.end), label: x.trial.label, totalPrice: x.trial.amount || "0.00" };
  } else if (r.deferred) {
    const x = r.deferred;
    g.deferredTransactionInfo = { ...info(x), billingDateTime: rfc3339(x.date), label: x.billing.label, price: x.billing.amount, priceStatus: priceStatus(x.billing.pending, x.billing.amount) };
  } else if (r.automaticReload) {
    const x = r.automaticReload;
    g.automaticReloadTransactionInfo = { ...info(x), minimumBalanceAmount: x.threshold, reloadAmount: x.billing.amount, label: x.billing.label };
  } else {
    g.transactionInfo = googleTransaction(r, r.items, r.total);
  }
  return g;
}

function googleError(errs, intent) {
  const e = errs[0];
  if (!e) return undefined;
  const f = name(Field, e.field) ?? "payment";
  if (f === "unserviceable") return { reason: "SHIPPING_ADDRESS_UNSERVICEABLE", message: e.message, intent: "SHIPPING_ADDRESS" };
  if (f === "shippingMethod") return { reason: "SHIPPING_OPTION_INVALID", message: e.message, intent: "SHIPPING_OPTION" };
  if (f.startsWith("shipping")) return { reason: "SHIPPING_ADDRESS_INVALID", message: e.message, intent: "SHIPPING_ADDRESS" };
  return { reason: "PAYMENT_DATA_INVALID", message: e.message, intent: intent === "PAYMENT_METHOD" || intent === "PAYMENT_AUTHORIZATION" ? intent : "PAYMENT_AUTHORIZATION" };
}

function googleContact(a, email = "") {
  if (!a) return { email };
  return { name: a.name ?? "", email, phone: a.phoneNumber ?? "", address: { lines: [a.address1, a.address2, a.address3].filter(Boolean),
    city: a.locality ?? "", state: a.administrativeArea ?? "", postalCode: a.postalCode ?? "", countryCode: (a.countryCode ?? "").toUpperCase(), sortingCode: a.sortingCode ?? "" } };
}

function googleCardOf(m) {
  const info = m?.info ?? {};
  const net = (info.cardNetwork ?? "").toLowerCase();
  const n = Object.keys(googleNetworks).find((k) => googleNetworks[k].toLowerCase() === net);
  return { network: n ? Network[n] : Network.other, kind: { CREDIT: CardKind.credit, DEBIT: CardKind.debit, PREPAID: CardKind.prepaid }[info.cardFundingSource] ?? CardKind.unknown,
    name: m?.description ?? "", last4: info.cardDetails ?? "", billing: googleContact(info.billingAddress) };
}

function googlePayment(d) {
  const m = d.paymentMethodData ?? {};
  return { wallet: Wallet.googlePay, token: m.tokenizationData?.token ?? "", card: googleCardOf(m), shipping: googleContact(d.shippingAddress, d.email ?? ""),
    billing: googleContact(m.info?.billingAddress, d.email ?? ""), shippingMethod: d.shippingOptionData?.id ?? "", raw: JSON.stringify(d) };
}

// --- Apple Pay's (Payment Request API) ---

function appleData(r) {
  const d = { version: 3, merchantIdentifier: r.apple.merchantId, merchantCapabilities: ["supports3DS"].concat(r.apple.emv ? ["supportsEMV"] : []),
    supportedNetworks: networksOf(r).map((n) => appleNetworks[n]).filter(Boolean), countryCode: r.country,
    requiredShippingContactFields: r.shipping.map((f) => appleFields[name(ContactField, f)]),
    requiredBillingContactFields: r.billing.map((f) => appleFields[name(ContactField, f)]),
    shippingType: name(ShippingTypeNames, r.shippingType) ?? "shipping" };
  if (r.cardCountries.length) d.supportedCountries = r.cardCountries;
  if (r.coupons) d.supportsCouponCode = true;
  if (r.coupon) d.couponCode = r.coupon;
  return d;
}
const ShippingTypeNames = { shipping: 0, delivery: 1, storePickup: 2, servicePickup: 3 };

const appleItem = (i) => ({ label: i.label, amount: { currency: "XXX", value: i.amount || "0" }, pending: i.pending });

function appleDetails(r, items, total, methods) {
  return {
    total: { label: total.label, amount: { currency: r.currency, value: total.amount || "0" }, pending: total.pending },
    displayItems: items.map((i) => ({ ...appleItem(i), amount: { currency: r.currency, value: i.amount || "0" } })),
    shippingOptions: methods.map((m, i) => ({ id: m.id, label: m.label, amount: { currency: r.currency, value: m.amount || "0" }, selected: i === 0 })),
  };
}

function appleContact(c) {
  if (!c) return {};
  return { name: c.recipient ?? [c.givenName, c.familyName].filter(Boolean).join(" "), givenName: c.givenName ?? "", familyName: c.familyName ?? "",
    email: c.emailAddress ?? "", phone: c.phone ?? c.phoneNumber ?? "",
    address: { lines: c.addressLine ?? [], city: c.city ?? c.locality ?? "", state: c.region ?? c.administrativeArea ?? "", postalCode: c.postalCode ?? "",
      countryCode: (c.country ?? c.countryCode ?? "").toUpperCase(), sortingCode: c.sortingCode ?? "" } };
}

// --- The sheet ---

let showing = false;
const pending = new Map();
let nextAsk = 0;

function ask(asked, a) {
  return new Promise((resolve) => {
    const id = ++nextAsk;
    pending.set(id, resolve);
    asked({ ...a, id });
  });
}

async function payGoogle(signal, r, asked) {
  let items = r.items, total = r.total, methods = r.shippingMethods, selected = methods[0]?.id ?? "", paid = null;
  const keep = (a) => {
    const u = a.update;
    if (u.items) items = u.items;
    if (u.total) total = { ...total, label: u.total.label || total.label, amount: u.total.amount || total.amount, pending: u.total.pending };
    if (u.shippingMethods) {
      methods = u.shippingMethods;
      if (!methods.some((m) => m.id === selected)) selected = methods[0]?.id ?? "";
    }
  };
  const client = await googleClient(r, {
    onPaymentDataChanged: async (d) => {
      if (d.shippingOptionData?.id && d.shippingOptionData.id !== "shipping_option_unselected") selected = d.shippingOptionData.id;
      let a = null;
      const trigger = d.callbackTrigger;
      if ((trigger === "INITIALIZE" || trigger === "SHIPPING_ADDRESS") && d.shippingAddress && r.asks.includes(AskKind.shippingContact)) {
        a = await ask(asked, { kind: AskKind.shippingContact, contact: googleContact(d.shippingAddress) });
      } else if (trigger === "SHIPPING_OPTION" && r.asks.includes(AskKind.shippingMethod)) {
        const m = methods.find((m) => m.id === selected);
        if (m) a = await ask(asked, { kind: AskKind.shippingMethod, method: m });
      } else if (trigger === "PAYMENT_METHOD" && r.asks.includes(AskKind.paymentMethod)) {
        a = await ask(asked, { kind: AskKind.paymentMethod, card: googleCardOf(d.paymentMethodData) });
      }
      if (a) keep(a);
      const u = {};
      const e = googleError(a?.update.errors ?? [], trigger === "INITIALIZE" ? "SHIPPING_ADDRESS" : trigger);
      if (e) u.error = e;
      if (!r.recurring && !r.deferred && !r.automaticReload) u.newTransactionInfo = googleTransaction(r, items, total);
      if (methods.length) u.newShippingOptionParameters = googleOptions(methods, selected);
      return u;
    },
    onPaymentAuthorized: async (d) => {
      const p = { ...googlePayment(d), shippingMethod: selected };
      const a = await ask(asked, { kind: AskKind.authorize, payment: p });
      const errs = a.failed && !a.update.errors.length ? [{ field: Field.payment, message: a.message }] : a.update.errors;
      if (errs.length) return { transactionState: "ERROR", error: googleError(errs, "PAYMENT_AUTHORIZATION") };
      paid = p;
      return { transactionState: "SUCCESS" };
    },
  });
  try {
    const d = await client.loadPaymentData(googleRequest(r));
    return paid ?? googlePayment(d);
  } catch (e) {
    if (e?.statusCode === "CANCELED") throw new PayError("canceled");
    throw e;
  }
}

async function payApple(signal, r, asked) {
  let items = r.items, total = r.total, methods = r.shippingMethods;
  const req = new PaymentRequest([{ supportedMethods: appleMethod, data: appleData(r) }], appleDetails(r, items, total, methods),
    { requestShipping: r.shipping.includes(ContactField.address) || methods.length > 0, requestPayerName: r.billing.includes(ContactField.name),
      requestPayerEmail: r.shipping.includes(ContactField.email) || r.billing.includes(ContactField.email), requestPayerPhone: r.shipping.includes(ContactField.phone) });
  const updated = (a) => {
    const u = a.update;
    if (u.items) items = u.items;
    if (u.total) total = { ...total, label: u.total.label || total.label, amount: u.total.amount || total.amount, pending: u.total.pending };
    if (u.shippingMethods) methods = u.shippingMethods;
    const d = appleDetails(r, items, total, methods);
    if (u.errors.length) d.error = u.errors[0].message;
    return d;
  };
  req.onmerchantvalidation = (e) => {
    if (!r.asks.includes(AskKind.validateMerchant)) return;
    e.complete(ask(asked, { kind: AskKind.validateMerchant, validationURL: e.validationURL }).then((a) => JSON.parse(new TextDecoder().decode(a.session))));
  };
  req.onshippingaddresschange = (e) => {
    if (r.asks.includes(AskKind.shippingContact)) e.updateWith(ask(asked, { kind: AskKind.shippingContact, contact: appleContact(req.shippingAddress) }).then(updated));
  };
  req.onshippingoptionchange = (e) => {
    const m = methods.find((m) => m.id === req.shippingOption);
    if (m && r.asks.includes(AskKind.shippingMethod)) e.updateWith(ask(asked, { kind: AskKind.shippingMethod, method: m }).then(updated));
  };
  req.onpaymentmethodchange = (e) => {
    if (r.asks.includes(AskKind.paymentMethod)) {
      const m = e.methodDetails ?? {};
      e.updateWith(ask(asked, { kind: AskKind.paymentMethod, card: { network: Network[Object.keys(appleNetworks).find((k) => appleNetworks[k].toLowerCase() === String(m.network ?? "").toLowerCase())] ?? Network.other,
        kind: CardKind[m.type] ?? CardKind.unknown, name: m.displayName ?? "", last4: "", billing: appleContact(m.billingContact) } }).then(updated));
    }
  };
  req.oncouponcodechange = (e) => {
    if (r.asks.includes(AskKind.coupon)) e.updateWith(ask(asked, { kind: AskKind.coupon, coupon: e.couponCode }).then(updated));
  };
  let res;
  try {
    res = await req.show();
  } catch (e) {
    if (e?.name === "AbortError") throw new PayError("canceled");
    throw e;
  }
  for (;;) {
    const d = res.details ?? {};
    const p = { wallet: Wallet.applePay, token: JSON.stringify(d.token?.paymentData ?? {}), transactionId: d.token?.transactionIdentifier ?? "",
      card: { network: Network.other, kind: CardKind.unknown, name: d.token?.paymentMethod?.displayName ?? "", last4: "" },
      shipping: appleContact(d.shippingContact ?? res.shippingAddress), billing: appleContact(d.billingContact), shippingMethod: res.shippingOption ?? "" };
    const a = await ask(asked, { kind: AskKind.authorize, payment: p });
    const errs = a.failed && !a.update.errors.length ? [{ field: Field.payment, message: a.message }] : a.update.errors;
    if (!errs.length) {
      await res.complete("success");
      return p;
    }
    // Shown, and the user tries again (or closes the sheet: retry rejects).
    try {
      await res.retry({ error: errs.map((e) => e.message).join("\n") });
    } catch {
      throw new PayError("canceled");
    }
  }
}

export function makeWallets() {
  return {
    async available(signal, r) {
      if (hasApplePay()) {
        let card = false;
        try {
          card = await globalThis.ApplePaySession.canMakePaymentsWithActiveCard?.(r.apple.merchantId) ?? true;
        } catch { /* no merchant ID */ }
        return { wallet: Wallet.applePay, canPay: true, hasCard: card };
      }
      try {
        const c = await googleClient(r);
        const can = (await c.isReadyToPay(googleReady(r))).result;
        if (!can) return { wallet: Wallet.googlePay, canPay: false, hasCard: false };
        const card = await c.isReadyToPay({ ...googleReady(r), existingPaymentMethodRequired: true });
        return { wallet: Wallet.googlePay, canPay: true, hasCard: !!card.paymentMethodPresent };
      } catch {
        return { wallet: Wallet.none, canPay: false, hasCard: false };
      }
    },

    async pay(signal, r, asked) {
      if (showing) throw new PayError("busy");
      showing = true;
      try {
        return hasApplePay() ? await payApple(signal, r, asked) : await payGoogle(signal, r, asked);
      } finally {
        showing = false;
        pending.clear();
      }
    },

    answer(id, a) {
      const f = pending.get(id);
      pending.delete(id);
      f?.(a);
    },

    async setUp() {
      throw new PayError("notAvailable");
    },
  };
}

// --- The button ---

const appleTypes = { plain: "plain", buy: "buy", checkout: "check-out", book: "book", donate: "donate", order: "order", subscribe: "subscribe",
  pay: "plain", continueWith: "continue", setUp: "set-up", reload: "reload", addMoney: "add-money", topUp: "top-up", rent: "rent",
  support: "support", contribute: "contribute", tip: "tip" };
const googleTypes = { plain: "plain", buy: "buy", checkout: "checkout", book: "book", donate: "donate", order: "order", subscribe: "subscribe",
  pay: "pay", setUp: "plain", rent: "book", support: "donate", contribute: "donate" };

export function makePayButton(events) {
  const el = document.createElement("div");
  el.style.display = "flex";
  let type = ButtonType.buy, variant = ButtonVariant.automatic, radius = -1, networks = [];
  let queued = false;
  const make = () => {
    queued = false;
    el.replaceChildren();
    const t = name(ButtonType, type) ?? "buy";
    const dark = matchMedia("(prefers-color-scheme: dark)").matches;
    if (hasApplePay()) {
      const b = document.createElement("button");
      b.type = "button";
      const s = { black: "black", white: "white", whiteOutline: "white-outline" }[name(ButtonVariant, variant)] ?? (dark ? "white" : "black");
      b.style.cssText = `-webkit-appearance: -apple-pay-button; -apple-pay-button-type: ${appleTypes[t] ?? "buy"}; -apple-pay-button-style: ${s}; width: 100%; height: 100%; border: 0;`;
      if (radius >= 0) b.style.borderRadius = `${radius}px`;
      b.onclick = () => events.press();
      el.append(b);
      return;
    }
    loadGoogle().then((api) => {
      const c = new api.PaymentsClient({ environment: "TEST" });
      const theme = variant === ButtonVariant.black ? "black" : variant === ButtonVariant.white || variant === ButtonVariant.whiteOutline ? "white" : dark ? "white" : "black";
      const b = c.createButton({ buttonType: googleTypes[t] ?? "pay", buttonColor: theme, buttonSizeMode: "fill", buttonRadius: radius >= 0 ? radius : undefined,
        allowedPaymentMethods: googleReady({ networks, cards: [], cardCountries: [], billing: [], google: {} }).allowedPaymentMethods, onClick: () => events.press() });
      el.replaceChildren(b);
    }, () => {});
  };
  const remake = () => {
    if (!queued) {
      queued = true;
      queueMicrotask(make);
    }
  };
  remake();
  return {
    el,
    setType(v) { type = v; remake(); },
    setVariant(v) { variant = v; remake(); },
    setCornerRadius(v) { radius = v; remake(); },
    setNetworks(v) { networks = v; remake(); },
    measure: (c) => [Math.max(Math.min(c?.width || 300, 600), 140), hasApplePay() ? 44 : 48],
  };
}
