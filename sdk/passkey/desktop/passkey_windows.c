// platform/passkey on Windows: webauthn.dll's WebAuthn (Windows 10 1903 and
// later, Windows 11 on x64 and ARM64): Windows Hello's passkeys, a
// security key, a phone's by its QR code (Windows 11 22H2), in Windows'
// own dialog over the app's window. Its functions are C, loaded from the
// system's DLL as the app runs (an older Windows has none: available()
// is false). Windows signs the client data the app gives it, which
// Asuka's side writes as a browser does, its origin https:// and the
// relying party's id. Each call blocks while the dialog shows: on a
// thread of its own; cancelling the task cancels it. Linux has no
// passkeys: the package has no C for it, and Asuka's side answers
// notAvailable.
#include "asuka_windows.h"
#include "passkey.asuka.h"

#include <stdlib.h>
#include <string.h>
#include <webauthn.h>

typedef DWORD(WINAPI *version_fn)(void);
typedef HRESULT(WINAPI *uvpaa_fn)(BOOL *);
typedef HRESULT(WINAPI *make_fn)(HWND, PCWEBAUTHN_RP_ENTITY_INFORMATION, PCWEBAUTHN_USER_ENTITY_INFORMATION, PCWEBAUTHN_COSE_CREDENTIAL_PARAMETERS,
                                 PCWEBAUTHN_CLIENT_DATA, PCWEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS, PWEBAUTHN_CREDENTIAL_ATTESTATION *);
typedef HRESULT(WINAPI *get_fn)(HWND, LPCWSTR, PCWEBAUTHN_CLIENT_DATA, PCWEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS, PWEBAUTHN_ASSERTION *);
typedef void(WINAPI *free_att_fn)(PWEBAUTHN_CREDENTIAL_ATTESTATION);
typedef void(WINAPI *free_assert_fn)(PWEBAUTHN_ASSERTION);
typedef HRESULT(WINAPI *cancel_id_fn)(GUID *);
typedef HRESULT(WINAPI *cancel_fn)(const GUID *);

static struct {
	bool loaded;
	DWORD api;
	uvpaa_fn uvpaa;
	make_fn make;
	get_fn get;
	free_att_fn free_att;
	free_assert_fn free_assert;
	cancel_id_fn cancel_id;
	cancel_fn cancel;
} wa;

// load finds webauthn.dll's functions (main thread, once): api 0 if it
// has none.
static void load(void) {
	if (wa.loaded) return;
	wa.loaded = true;
	HMODULE m = LoadLibraryExW(L"webauthn.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!m) return;
	version_fn version = (version_fn)(void *)GetProcAddress(m, "WebAuthNGetApiVersionNumber");
	wa.uvpaa = (uvpaa_fn)(void *)GetProcAddress(m, "WebAuthNIsUserVerifyingPlatformAuthenticatorAvailable");
	wa.make = (make_fn)(void *)GetProcAddress(m, "WebAuthNAuthenticatorMakeCredential");
	wa.get = (get_fn)(void *)GetProcAddress(m, "WebAuthNAuthenticatorGetAssertion");
	wa.free_att = (free_att_fn)(void *)GetProcAddress(m, "WebAuthNFreeCredentialAttestation");
	wa.free_assert = (free_assert_fn)(void *)GetProcAddress(m, "WebAuthNFreeAssertion");
	wa.cancel_id = (cancel_id_fn)(void *)GetProcAddress(m, "WebAuthNGetCancellationId");
	wa.cancel = (cancel_fn)(void *)GetProcAddress(m, "WebAuthNCancelCurrentOperation");
	if (version && wa.make && wa.get && wa.free_att && wa.free_assert) wa.api = version();
}

static void fail_declared(asuka_call *call, int declared) {
	asuka_error e = {0};
	asuka_error_declared(&e, (uint32_t)declared);
	asuka_call_fail(call, &e);
}

// fail answers call with what hr says: the package's error where it's
// one of them (as WebAuthNGetW3CExceptionDOMError maps them).
static void fail(asuka_call *call, HRESULT hr, bool cancelled) {
	if (cancelled) {
		asuka_error e = {0};
		asuka_error_cancelled(&e);
		asuka_call_fail(call, &e);
		return;
	}
	switch ((unsigned long)hr) {
	case 0x80090036UL: // NTE_USER_CANCELLED
	case 0x800704C7UL: // HRESULT_FROM_WIN32(ERROR_CANCELLED)
	case 0x800705B4UL: // HRESULT_FROM_WIN32(ERROR_TIMEOUT): a browser's NotAllowedError
		fail_declared(call, passkey_PasskeyError_canceled);
		return;
	case 0x8009000FUL: // NTE_EXISTS: an excluded credential
		fail_declared(call, passkey_PasskeyError_exists);
		return;
	case 0x80090011UL: // NTE_NOT_FOUND
		fail_declared(call, passkey_PasskeyError_noCredentials);
		return;
	case 0x80090029UL: // NTE_NOT_SUPPORTED
		fail_declared(call, passkey_PasskeyError_notAvailable);
		return;
	}
	asuka_error e = {0};
	asuka_win_fail(&e, hr, "passkey");
	asuka_call_fail(call, &e);
}

static DWORD user_verification(const char *s) {
	if (!s) return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_ANY;
	if (strcmp(s, "required") == 0) return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_REQUIRED;
	if (strcmp(s, "preferred") == 0) return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_PREFERRED;
	if (strcmp(s, "discouraged") == 0) return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_DISCOURAGED;
	return WEBAUTHN_USER_VERIFICATION_REQUIREMENT_ANY;
}

static DWORD attachment(const char *s) {
	if (s && strcmp(s, "platform") == 0) return WEBAUTHN_AUTHENTICATOR_ATTACHMENT_PLATFORM;
	if (s && strcmp(s, "cross-platform") == 0) return WEBAUTHN_AUTHENTICATOR_ATTACHMENT_CROSS_PLATFORM;
	return WEBAUTHN_AUTHENTICATOR_ATTACHMENT_ANY;
}

static DWORD attestation(const char *s) {
	if (s && strcmp(s, "direct") == 0) return WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_DIRECT;
	if (s && strcmp(s, "indirect") == 0) return WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_INDIRECT;
	return WEBAUTHN_ATTESTATION_CONVEYANCE_PREFERENCE_NONE;
}

// A credential list (WEBAUTHN_CREDENTIAL_LIST) of ids.
typedef struct {
	WEBAUTHN_CREDENTIAL_EX *items;
	PWEBAUTHN_CREDENTIAL_EX *ptrs;
	WEBAUTHN_CREDENTIAL_LIST list;
} cred_list;

static void cred_list_make(cred_list *l, const passkey_list_bytes *ids) {
	memset(l, 0, sizeof *l);
	if (ids->len == 0) return;
	l->items = calloc(ids->len, sizeof *l->items);
	l->ptrs = calloc(ids->len, sizeof *l->ptrs);
	if (!l->items || !l->ptrs) return;
	for (size_t i = 0; i < ids->len; i++) {
		l->items[i] = (WEBAUTHN_CREDENTIAL_EX){WEBAUTHN_CREDENTIAL_EX_CURRENT_VERSION, (DWORD)ids->data[i].len, ids->data[i].data,
		                                       WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, 0};
		l->ptrs[i] = &l->items[i];
	}
	l->list = (WEBAUTHN_CREDENTIAL_LIST){(DWORD)ids->len, l->ptrs};
}

static void cred_list_free(cred_list *l) {
	free(l->items);
	free(l->ptrs);
}

// transports are what dwUsedTransport says, WebAuthn's names.
static passkey_list_string transports(DWORD t) {
	static const struct {
		DWORD bit;
		const char *name;
	} names[] = {{WEBAUTHN_CTAP_TRANSPORT_USB, "usb"}, {WEBAUTHN_CTAP_TRANSPORT_NFC, "nfc"}, {WEBAUTHN_CTAP_TRANSPORT_BLE, "ble"},
	             {WEBAUTHN_CTAP_TRANSPORT_INTERNAL, "internal"}, {0x20, "hybrid"}};
	passkey_list_string out = {calloc(5, sizeof(char *)), 0};
	if (!out.data) return out;
	for (size_t i = 0; i < 5; i++) {
		if (t & names[i].bit) out.data[out.len++] = asuka_strdup(names[i].name);
	}
	return out;
}

// One call to webauthn.dll: what it takes, copied (the arguments go when
// the method returns), and its cancellation.
typedef struct {
	asuka_call *call;
	HWND window;
	bool get;
	GUID cancel_id;
	bool can_cancel;
	volatile LONG cancelled;
	// Both.
	wchar_t *rp_id;
	asuka_bytes client_data;
	DWORD uv, timeout;
	cred_list creds;
	passkey_list_bytes ids;
	// Registration's.
	wchar_t *rp_name, *user_name, *user_display;
	asuka_bytes user_id;
	WEBAUTHN_COSE_CREDENTIAL_PARAMETER *params;
	size_t nparams;
	DWORD attachment, attestation;
	bool require_rk, prefer_rk;
} op;

static void op_free(op *o) {
	free(o->rp_id);
	free(o->client_data.data);
	cred_list_free(&o->creds);
	passkey_list_bytes_free(&o->ids);
	free(o->rp_name);
	free(o->user_name);
	free(o->user_display);
	free(o->user_id.data);
	free(o->params);
	free(o);
}

static void on_cancel(void *p) {
	op *o = p;
	InterlockedExchange(&o->cancelled, 1);
	if (o->can_cancel && wa.cancel) wa.cancel(&o->cancel_id);
}

static passkey_list_bytes copy_ids(const passkey_list_bytes *in) {
	passkey_list_bytes out = {calloc(in->len ? in->len : 1, sizeof(asuka_bytes)), 0};
	if (!out.data) return out;
	for (size_t i = 0; i < in->len; i++) out.data[out.len++] = asuka_bytes_copy(in->data[i].data, in->data[i].len);
	return out;
}

static void run(void *p) {
	op *o = p;
	WEBAUTHN_CLIENT_DATA cd = {WEBAUTHN_CLIENT_DATA_CURRENT_VERSION, (DWORD)o->client_data.len, o->client_data.data, WEBAUTHN_HASH_ALGORITHM_SHA_256};
	HRESULT hr;
	bool cancelled;
	if (!o->get) {
		WEBAUTHN_RP_ENTITY_INFORMATION rp = {WEBAUTHN_RP_ENTITY_INFORMATION_CURRENT_VERSION, o->rp_id, o->rp_name, NULL};
		WEBAUTHN_USER_ENTITY_INFORMATION user = {WEBAUTHN_USER_ENTITY_INFORMATION_CURRENT_VERSION, (DWORD)o->user_id.len, o->user_id.data,
		                                         o->user_name, NULL, o->user_display};
		WEBAUTHN_COSE_CREDENTIAL_PARAMETERS params = {(DWORD)o->nparams, o->params};
		WEBAUTHN_AUTHENTICATOR_MAKE_CREDENTIAL_OPTIONS opts;
		memset(&opts, 0, sizeof opts);
		// Version 4 (API 3: Windows 10 21H2, 11) prefers a discoverable
		// credential; 3 (the first API's) excludes and cancels.
		opts.dwVersion = wa.api >= 3 ? 4 : 3;
		opts.dwTimeoutMilliseconds = o->timeout;
		opts.dwAuthenticatorAttachment = o->attachment;
		opts.bRequireResidentKey = o->require_rk;
		opts.dwUserVerificationRequirement = o->uv;
		opts.dwAttestationConveyancePreference = o->attestation;
		opts.pCancellationId = o->can_cancel ? &o->cancel_id : NULL;
		opts.pExcludeCredentialList = o->creds.list.cCredentials ? &o->creds.list : NULL;
		opts.bPreferResidentKey = o->prefer_rk;
		PWEBAUTHN_CREDENTIAL_ATTESTATION att = NULL;
		hr = wa.make(o->window, &rp, &user, &params, &cd, &opts, &att);
		cancelled = InterlockedCompareExchange(&o->cancelled, 0, 0) != 0;
		if (SUCCEEDED(hr) && att && !cancelled) {
			passkey_Response r = {0};
			r.id = asuka_bytes_copy(att->pbCredentialId, att->cbCredentialId);
			r.clientData = asuka_bytes_copy(o->client_data.data, o->client_data.len);
			r.attestationObject = asuka_bytes_copy(att->pbAttestationObject, att->cbAttestationObject);
			r.authenticatorData = asuka_bytes_copy(att->pbAuthenticatorData, att->cbAuthenticatorData);
			DWORD used = att->dwVersion >= WEBAUTHN_CREDENTIAL_ATTESTATION_VERSION_3 ? att->dwUsedTransport : 0;
			r.transports = transports(used);
			r.attachment = asuka_strdup(used == 0 ? "" : used & WEBAUTHN_CTAP_TRANSPORT_INTERNAL ? "platform" : "cross-platform");
			wa.free_att(att);
			passkey_Authenticator_register_done(o->call, r);
			op_free(o);
			return;
		}
		if (att) wa.free_att(att);
	} else {
		WEBAUTHN_AUTHENTICATOR_GET_ASSERTION_OPTIONS opts;
		memset(&opts, 0, sizeof opts);
		opts.dwVersion = wa.api >= 3 ? 5 : 4;
		opts.dwTimeoutMilliseconds = o->timeout;
		opts.dwUserVerificationRequirement = o->uv;
		opts.pCancellationId = o->can_cancel ? &o->cancel_id : NULL;
		opts.pAllowCredentialList = o->creds.list.cCredentials ? &o->creds.list : NULL;
		PWEBAUTHN_ASSERTION a = NULL;
		hr = wa.get(o->window, o->rp_id, &cd, &opts, &a);
		cancelled = InterlockedCompareExchange(&o->cancelled, 0, 0) != 0;
		if (SUCCEEDED(hr) && a && !cancelled) {
			passkey_Response r = {0};
			r.id = asuka_bytes_copy(a->Credential.pbId, a->Credential.cbId);
			r.clientData = asuka_bytes_copy(o->client_data.data, o->client_data.len);
			r.authenticatorData = asuka_bytes_copy(a->pbAuthenticatorData, a->cbAuthenticatorData);
			r.signature = asuka_bytes_copy(a->pbSignature, a->cbSignature);
			r.userHandle = asuka_bytes_copy(a->pbUserId, a->cbUserId);
			r.transports = (passkey_list_string){NULL, 0};
			r.attachment = asuka_strdup("");
			wa.free_assert(a);
			passkey_Authenticator_signIn_done(o->call, r);
			op_free(o);
			return;
		}
		if (a) wa.free_assert(a);
	}
	fail(o->call, SUCCEEDED(hr) ? E_FAIL : hr, cancelled);
	op_free(o);
}

// start runs o on a thread of its own, cancelled with the task.
static void start(op *o) {
	o->window = (HWND)asuka_desktop_window(0);
	o->can_cancel = wa.cancel_id && wa.cancel && SUCCEEDED(wa.cancel_id(&o->cancel_id));
	o->creds.list.cCredentials = 0;
	cred_list_make(&o->creds, &o->ids);
	asuka_call_on_cancel(o->call, on_cancel, o);
	if (!asuka_desktop_spawn(run, o)) {
		asuka_call_off_cancel(o->call);
		asuka_error e = {0};
		asuka_error_failed(&e, "passkey", 0, "no thread");
		asuka_call_fail(o->call, &e);
		op_free(o);
	}
}

static void p_available(passkey_Authenticator *self, asuka_call *call) {
	(void)self;
	load();
	passkey_Authenticator_available_done(call, wa.api >= 1);
}

static void p_register(passkey_Authenticator *self, const passkey_Creation *c, asuka_call *call) {
	(void)self;
	load();
	if (wa.api < 1) {
		fail_declared(call, passkey_PasskeyError_notAvailable);
		return;
	}
	op *o = calloc(1, sizeof *o);
	if (!o) {
		fail(call, E_OUTOFMEMORY, false);
		return;
	}
	o->call = call;
	o->rp_id = asuka_wide(c->rpId);
	o->rp_name = asuka_wide(c->rpName);
	o->user_name = asuka_wide(c->userName);
	o->user_display = asuka_wide(c->userDisplayName);
	o->user_id = asuka_bytes_copy(c->userId.data, c->userId.len);
	o->client_data = asuka_bytes_copy(c->clientData.data, c->clientData.len);
	o->uv = user_verification(c->userVerification);
	o->timeout = c->timeoutMs > 0 ? (DWORD)c->timeoutMs : 120000;
	o->attachment = attachment(c->attachment);
	o->attestation = attestation(c->attestation);
	o->require_rk = c->residentKey && strcmp(c->residentKey, "required") == 0;
	o->prefer_rk = c->residentKey && strcmp(c->residentKey, "preferred") == 0;
	o->params = calloc(c->algorithms.len ? c->algorithms.len : 1, sizeof *o->params);
	if (o->params) {
		for (size_t i = 0; i < c->algorithms.len; i++) {
			o->params[o->nparams++] = (WEBAUTHN_COSE_CREDENTIAL_PARAMETER){WEBAUTHN_COSE_CREDENTIAL_PARAMETER_CURRENT_VERSION,
			                                                               WEBAUTHN_CREDENTIAL_TYPE_PUBLIC_KEY, (LONG)c->algorithms.data[i]};
		}
	}
	o->ids = copy_ids(&c->exclude);
	start(o);
}

static void p_sign_in(passkey_Authenticator *self, const passkey_Request *r, asuka_call *call) {
	(void)self;
	load();
	if (wa.api < 1) {
		fail_declared(call, passkey_PasskeyError_notAvailable);
		return;
	}
	op *o = calloc(1, sizeof *o);
	if (!o) {
		fail(call, E_OUTOFMEMORY, false);
		return;
	}
	o->call = call;
	o->get = true;
	o->rp_id = asuka_wide(r->rpId);
	o->client_data = asuka_bytes_copy(r->clientData.data, r->clientData.len);
	o->uv = user_verification(r->userVerification);
	o->timeout = r->timeoutMs > 0 ? (DWORD)r->timeoutMs : 120000;
	o->ids = copy_ids(&r->allow);
	start(o);
}

static void p_release(passkey_Authenticator *self) { free(self); }

static const passkey_Authenticator_vtable vt = {
    .release = p_release,
    .available = p_available,
    .register_ = p_register,
    .signIn = p_sign_in,
};

passkey_Authenticator *passkey_make_Authenticator(asuka_error *err) {
	passkey_Authenticator *a = malloc(sizeof *a);
	if (!a) {
		asuka_error_failed(err, "passkey", 0, "out of memory");
		return NULL;
	}
	a->vt = &vt;
	return a;
}
