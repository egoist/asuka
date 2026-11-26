// platform/secure on Windows: Credential Manager, a generic credential per
// key, named the app's name / the key (as the Windows host's secure store
// named them). Small and quick: answered on the calling thread.
//
// A protected value is sealed with a key only Windows Hello can give, as
// KeePassXC's and Bitwarden's are: the app has one Windows Hello key
// credential (KeyCredentialManager, named for its ID: an RSA key in the
// TPM, or Windows Hello's software store, that signs only once the user
// passes Windows Hello's prompt). A write makes a random challenge, has
// the key sign it (a prompt: the write asks too, and the first one also
// makes the key), and seals the value with AES-256-GCM under SHA-256 of
// the signature, bound to its key's name; the credential keeps the
// challenge, the nonce, the tag and the ciphertext, and an attribute
// saying it's protected. A read has the same challenge signed again (the
// prompt, saying the read's words: unused where Windows Hello's own
// dialog has no place for them) and opens it: Windows Hello's signatures
// are PKCS#1 v1.5's, so the same challenge gives the same signature, and
// nothing but Windows Hello can make it. Writing either way replaces the
// other: it's the same credential. Both wait on a worker.
//
// The prompt is owned by the app's window where Windows has the
// window-taking calls (RequestCreateForWindowAsync,
// RequestSignForWindowAsync: Windows 11 24H2); before them Windows shows
// it on its own, often under the app's window, so it's brought to the
// front once it appears (as Chromium and KeePassXC do).
#include "asuka_windows.h"
#include <wincred.h>
#include <bcrypt.h>
#include "secure.asuka.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define KCM "Windows.Security.Credentials.KeyCredentialManager"
static const GUID iid_kcm_statics = {0x6AAC468B, 0x0EF1, 0x4CE0, {0x82, 0x90, 0x41, 0x06, 0xDA, 0x6A, 0x63, 0xB5}};  // IKeyCredentialManagerStatics
static const GUID iid_kcm_window = {0x30B1B9C9, 0x61EF, 0x43E8, {0x88, 0xAC, 0xCC, 0x43, 0x3B, 0x38, 0xD1, 0xA6}};  // IKeyCredentialManagerCreateWithWindowStatics
static const GUID iid_kc_window = {0x050DEB3B, 0xB19C, 0x4635, {0x9D, 0xF6, 0x56, 0x50, 0xD6, 0x6C, 0x62, 0xB1}};   // IKeyCredentialWithWindow
static const GUID iid_buffers = {0x320B7E22, 0x3CB0, 0x4CDF, {0x86, 0x63, 0x1D, 0x28, 0x91, 0x00, 0x65, 0xEB}};     // ICryptographicBufferStatics
#define RETRIEVAL "rc(Windows.Security.Credentials.KeyCredentialRetrievalResult;{58cd7703-8d87-4249-9b58-f6598cc9644e})"
#define OPERATION "rc(Windows.Security.Credentials.KeyCredentialOperationResult;{f53786c1-5261-4cdd-976d-cc909ac71620})"

enum { kc_success = 0, kc_unknown, kc_not_found, kc_canceled, kc_prefers_password, kc_exists, kc_locked, kc_algorithm };  // KeyCredentialStatus
enum { fail_if_exists = 1 };                                                                                           // KeyCredentialCreationOption

// Windows.UI.WindowId.
typedef struct {
	UINT64 value;
} window_id;

// What asking Windows Hello came to.
enum { hello_ok, hello_cancelled, hello_unavailable, hello_failed };

// A protected credential's blob: a version, the challenge signed, the
// nonce, the tag, then the ciphertext.
enum { sealed_version = 1, challenge_size = 32, nonce_size = 12, tag_size = 16 };
#define SEALED_HEAD (1 + challenge_size + nonce_size + tag_size)

#ifndef STATUS_AUTH_TAG_MISMATCH
#define STATUS_AUTH_TAG_MISMATCH ((NTSTATUS)0xC000A002L)
#endif

// The attribute a protected credential carries.
static wchar_t protected_keyword[] = L"asuka:protected";

static wchar_t *target(const char *key) {
	char *t = asuka_sprintf("%s/%s", asuka_desktop_app_name(), key);
	wchar_t *w = asuka_wide(t);
	free(t);
	return w;
}

// wipe zeroes n bytes of a secret's copy and frees it.
static void wipe(void *p, size_t n) {
	if (!p) return;
	SecureZeroMemory(p, n);
	free(p);
}

static void wipe_string(char *s) {
	if (s) wipe(s, strlen(s));
}

static void cred_fail(asuka_error *err, DWORD code) { asuka_win_fail(err, HRESULT_FROM_WIN32(code), "Credential Manager"); }

static void fail_declared(asuka_call *call, uint32_t index) {
	asuka_error e = {0};
	asuka_error_declared(&e, index);
	asuka_call_fail(call, &e);
}

// ---- Credential Manager ----

// stored reads key's credential: 1 with its blob (malloc's, n bytes, a
// NUL after) and whether it's protected, 0 if there's none, -1 if it
// failed (err set).
static int stored(const char *key, uint8_t **data, size_t *n, bool *prot, asuka_error *err) {
	wchar_t *t = target(key);
	CREDENTIALW *c = NULL;
	BOOL ok = CredReadW(t, CRED_TYPE_GENERIC, 0, &c);
	DWORD code = ok ? 0 : GetLastError();
	free(t);
	if (!ok) {
		if (code == ERROR_NOT_FOUND) return 0;
		cred_fail(err, code);
		return -1;
	}
	*prot = false;
	for (DWORD i = 0; i < c->AttributeCount; i++) {
		if (c->Attributes[i].Keyword && wcscmp(c->Attributes[i].Keyword, protected_keyword) == 0) *prot = true;
	}
	*n = c->CredentialBlobSize;
	*data = (uint8_t *)asuka_strndup((const char *)c->CredentialBlob, c->CredentialBlobSize);
	SecureZeroMemory(c->CredentialBlob, c->CredentialBlobSize);
	CredFree(c);
	return 1;
}

static bool too_long(size_t n, asuka_error *err) {
	if (n <= CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
	asuka_error_failed(err, "secure", 0, "the value is longer than the Credential Manager keeps (%d bytes, %d protected)", CRED_MAX_CREDENTIAL_BLOB_SIZE,
	                   CRED_MAX_CREDENTIAL_BLOB_SIZE - SEALED_HEAD);
	return true;
}

// keep writes key's credential, replacing what was there (protected or
// not): false if it failed (err set).
static bool keep(const char *key, const void *data, size_t n, bool prot, asuka_error *err) {
	if (too_long(n, err)) return false;
	wchar_t *t = target(key), *user = asuka_wide(key);
	BYTE flag = 1;
	CREDENTIAL_ATTRIBUTEW attr = {.Keyword = protected_keyword, .ValueSize = 1, .Value = &flag};
	CREDENTIALW c;
	memset(&c, 0, sizeof c);
	c.Type = CRED_TYPE_GENERIC;
	c.TargetName = t;
	c.UserName = user;
	c.CredentialBlobSize = (DWORD)n;
	c.CredentialBlob = (LPBYTE)data;
	c.Persist = CRED_PERSIST_LOCAL_MACHINE;
	if (prot) {
		c.AttributeCount = 1;
		c.Attributes = &attr;
	}
	BOOL ok = CredWriteW(&c, 0);
	DWORD code = ok ? 0 : GetLastError();
	free(t);
	free(user);
	if (!ok) cred_fail(err, code);
	return ok;
}

// ---- Sealing (BCrypt) ----

// derive is the AES-256 key of a Windows Hello signature: its SHA-256.
static bool derive(const uint8_t *sig, size_t n, uint8_t key[32]) {
	return BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, NULL, 0, (PUCHAR)sig, (ULONG)n, key, 32));
}

// gcm runs AES-256-GCM under key, nonce and tag, with name as the
// additional data: in to out (n bytes); a status.
static NTSTATUS gcm(bool encrypt, const uint8_t key[32], const char *name, uint8_t *nonce, uint8_t *tag, const uint8_t *in, uint8_t *out,
                    size_t n) {
	BCRYPT_KEY_HANDLE k = NULL;
	NTSTATUS s = BCryptGenerateSymmetricKey(BCRYPT_AES_GCM_ALG_HANDLE, &k, NULL, 0, (PUCHAR)key, 32, 0);
	if (!BCRYPT_SUCCESS(s)) return s;
	BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
	BCRYPT_INIT_AUTH_MODE_INFO(info);
	info.pbNonce = nonce;
	info.cbNonce = nonce_size;
	info.pbAuthData = (PUCHAR)name;
	info.cbAuthData = (ULONG)strlen(name);
	info.pbTag = tag;
	info.cbTag = tag_size;
	ULONG done = 0;
	uint8_t none = 0;  // BCrypt wants buffers, even for nothing
	if (encrypt) s = BCryptEncrypt(k, (PUCHAR)(n ? in : &none), (ULONG)n, &info, NULL, 0, n ? out : &none, (ULONG)n, &done, 0);
	else s = BCryptDecrypt(k, (PUCHAR)(n ? in : &none), (ULONG)n, &info, NULL, 0, n ? out : &none, (ULONG)n, &done, 0);
	BCryptDestroyKey(k);
	return s;
}

// seal is value sealed for name under key: the blob (malloc's, *n bytes)
// with the challenge key's signature was of; NULL if BCrypt failed.
static uint8_t *seal(const uint8_t key[32], const uint8_t challenge[challenge_size], const char *name, const char *value, size_t *n) {
	size_t len = strlen(value);
	uint8_t *b = malloc(SEALED_HEAD + len);
	b[0] = sealed_version;
	memcpy(b + 1, challenge, challenge_size);
	uint8_t *nonce = b + 1 + challenge_size, *tag = nonce + nonce_size;
	if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, nonce, nonce_size, BCRYPT_USE_SYSTEM_PREFERRED_RNG)) ||
	    !BCRYPT_SUCCESS(gcm(true, key, name, nonce, tag, (const uint8_t *)value, b + SEALED_HEAD, len))) {
		wipe(b, SEALED_HEAD + len);
		return NULL;
	}
	*n = SEALED_HEAD + len;
	return b;
}

// sealed_challenge is a blob's challenge, or NULL if it isn't one seal made.
static const uint8_t *sealed_challenge(const uint8_t *b, size_t n) { return n >= SEALED_HEAD && b[0] == sealed_version ? b + 1 : NULL; }

// unseal is the value a blob of seal's keeps (malloc's), or NULL if key
// doesn't open it (another key's, or not seal's).
static char *unseal(const uint8_t key[32], const char *name, const uint8_t *b, size_t n) {
	if (!sealed_challenge(b, n)) return NULL;
	size_t len = n - SEALED_HEAD;
	uint8_t nonce[nonce_size], tag[tag_size];
	memcpy(nonce, b + 1 + challenge_size, nonce_size);
	memcpy(tag, b + 1 + challenge_size + nonce_size, tag_size);
	char *v = malloc(len + 1);
	if (!BCRYPT_SUCCESS(gcm(false, key, name, nonce, tag, b + SEALED_HEAD, (uint8_t *)v, len))) {
		wipe(v, len + 1);
		return NULL;
	}
	v[len] = 0;
	return v;
}

// ---- Windows Hello (KeyCredentialManager) ----

// window_id_of is hwnd's WindowId (windows.ui.interop.h's
// GetWindowIdFromWindow): false where Windows has none.
static HRESULT(WINAPI *get_window_id)(HWND, window_id *);
static BOOL CALLBACK load_window_id(PINIT_ONCE once, void *p, void **ctx) {
	HMODULE m = LoadLibraryExW(L"ext-ms-win-windowing-external-l1-1-0.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (m) *(FARPROC *)&get_window_id = GetProcAddress(m, "GetWindowIdFromWindow");
	return TRUE;
}
static bool window_id_of(HWND hwnd, window_id *id) {
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	InitOnceExecuteOnce(&once, load_window_id, NULL, NULL);
	id->value = 0;
	return hwnd && get_window_id && SUCCEEDED(get_window_id(hwnd, id)) && id->value;
}

// forward brings Windows Hello's dialog to the front once it shows (a
// thread of its own, for a few seconds): shown without an owner, it opens
// under the app's window (Chromium's crypto/user_verifying_key_win.cc).
static void forward_run(void *p) {
	for (int i = 0; i < 50; i++) {
		HWND w = FindWindowW(L"Credential Dialog Xaml Host", NULL);
		if (w) {
			SetForegroundWindow(w);
			return;
		}
		Sleep(100);
	}
}
static void forward(void) { asuka_desktop_spawn(forward_run, NULL); }

static int status_outcome(int status) {
	switch (status) {
	case kc_success: return hello_ok;
	case kc_canceled:
	case kc_prefers_password: return hello_cancelled;
	case kc_not_found:  // the app's key is gone (Windows Hello reset)
	case kc_locked:
	case kc_algorithm: return hello_unavailable;
	default: return hello_failed;
	}
}

static int hr_outcome(HRESULT hr) { return hr == E_ABORT ? hello_cancelled : hello_failed; }

// supported asks KeyCredentialManager whether this user can have a key
// (Windows Hello set up), waiting: a worker's. cancel (or NULL) stops it.
static bool supported(asuka_call *cancel) {
	void *st = NULL, *op = NULL;
	boolean yes = 0;
	if (!asuka_winrt() || FAILED(asuka_winrt_factory(KCM, &iid_kcm_statics, &st))) return false;
	if (SUCCEEDED(ASUKA_SLOT(st, 6, HRESULT(STDMETHODCALLTYPE *)(void *, void **))(st, &op)) && op) {  // IsSupportedAsync
		GUID h = asuka_winrt_iid("pinterface(" ASUKA_WINRT_ASYNC_HANDLER ";b1)");
		if (asuka_winrt_wait(op, &h, cancel) == S_OK) ASUKA_SLOT(op, 8, HRESULT(STDMETHODCALLTYPE *)(void *, boolean *))(op, &yes);  // GetResults
		asuka_com_release(op);
	}
	asuka_com_release(st);
	return yes != 0;
}

// retrieved waits for a KeyCredentialRetrievalResult (op's, released):
// its status, and its credential (a reference) when that's Success.
static HRESULT retrieved(void *op, asuka_call *cancel, int *status, void **cred) {
	void *res = NULL;
	GUID h = asuka_winrt_iid("pinterface(" ASUKA_WINRT_ASYNC_HANDLER ";" RETRIEVAL ")");
	HRESULT hr = asuka_winrt_wait(op, &h, cancel);
	if (hr == S_OK) hr = ASUKA_SLOT(op, 8, HRESULT(STDMETHODCALLTYPE *)(void *, void **))(op, &res);  // GetResults
	asuka_com_release(op);
	if (SUCCEEDED(hr) && !res) hr = E_POINTER;
	if (FAILED(hr)) return hr;
	hr = ASUKA_SLOT(res, 7, HRESULT(STDMETHODCALLTYPE *)(void *, int *))(res, status);                                  // get_Status
	if (SUCCEEDED(hr) && *status == kc_success) hr = ASUKA_SLOT(res, 6, HRESULT(STDMETHODCALLTYPE *)(void *, void **))(res, cred);  // get_Credential
	asuka_com_release(res);
	return hr;
}

static HRESULT open_named(void *st, HSTRING name, asuka_call *cancel, int *status, void **cred) {
	void *op = NULL;
	HRESULT hr = ASUKA_SLOT(st, 9, HRESULT(STDMETHODCALLTYPE *)(void *, HSTRING, void **))(st, name, &op);  // OpenAsync
	return SUCCEEDED(hr) ? retrieved(op, cancel, status, cred) : hr;
}

// credential is the app's key credential (a reference): made, a prompt
// of Windows Hello's, when create and there's none.
static int credential(void *st, HWND window, bool create, asuka_call *cancel, void **cred, HRESULT *hr) {
	char *n = asuka_sprintf("%s.asuka.secure", asuka_desktop_app_id());
	HSTRING name = asuka_hstring(n);
	free(n);
	int status = kc_unknown;
	*hr = open_named(st, name, cancel, &status, cred);
	if (create && SUCCEEDED(*hr) && status == kc_not_found) {
		void *op = NULL, *ws = NULL;
		window_id wid;
		*hr = E_NOTIMPL;
		if (window_id_of(window, &wid) && SUCCEEDED(asuka_winrt_factory(KCM, &iid_kcm_window, &ws))) {
			*hr = ASUKA_SLOT(ws, 6, HRESULT(STDMETHODCALLTYPE *)(void *, window_id, HSTRING, int, void **))(ws, wid, name, fail_if_exists, &op);  // RequestCreateForWindowAsync
			asuka_com_release(ws);
		}
		if (FAILED(*hr)) {
			forward();
			*hr = ASUKA_SLOT(st, 8, HRESULT(STDMETHODCALLTYPE *)(void *, HSTRING, int, void **))(st, name, fail_if_exists, &op);  // RequestCreateAsync
		}
		if (SUCCEEDED(*hr)) *hr = retrieved(op, cancel, &status, cred);
		if (SUCCEEDED(*hr) && status == kc_exists) *hr = open_named(st, name, cancel, &status, cred);  // made meanwhile
	}
	asuka_hstring_free(name);
	if (FAILED(*hr)) return hr_outcome(*hr);
	if (status_outcome(status) == hello_failed) *hr = E_FAIL;
	return status_outcome(status);
}

// signed_key has cred sign challenge (Windows Hello's prompt) and derives
// the AES key of the signature.
static int signed_key(void *cred, HWND window, const uint8_t challenge[challenge_size], asuka_call *cancel, uint8_t key[32], HRESULT *hr) {
	void *bs = NULL, *buf = NULL, *op = NULL, *res = NULL, *out = NULL, *kw = NULL;
	int status = kc_unknown;
	*hr = asuka_winrt_factory("Windows.Security.Cryptography.CryptographicBuffer", &iid_buffers, &bs);
	if (SUCCEEDED(*hr))
		*hr = ASUKA_SLOT(bs, 9, HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, const BYTE *, void **))(bs, challenge_size, challenge, &buf);  // CreateFromByteArray
	if (SUCCEEDED(*hr)) {
		window_id wid;
		*hr = E_NOTIMPL;
		if (window_id_of(window, &wid) && (kw = asuka_com_query(cred, &iid_kc_window))) {
			*hr = ASUKA_SLOT(kw, 6, HRESULT(STDMETHODCALLTYPE *)(void *, window_id, void *, void **))(kw, wid, buf, &op);  // RequestSignForWindowAsync
			asuka_com_release(kw);
		}
		if (FAILED(*hr)) {
			forward();
			*hr = ASUKA_SLOT(cred, 9, HRESULT(STDMETHODCALLTYPE *)(void *, void *, void **))(cred, buf, &op);  // RequestSignAsync
		}
	}
	if (SUCCEEDED(*hr)) {
		GUID h = asuka_winrt_iid("pinterface(" ASUKA_WINRT_ASYNC_HANDLER ";" OPERATION ")");
		*hr = asuka_winrt_wait(op, &h, cancel);
		if (*hr == S_OK) *hr = ASUKA_SLOT(op, 8, HRESULT(STDMETHODCALLTYPE *)(void *, void **))(op, &res);  // GetResults
		if (SUCCEEDED(*hr) && !res) *hr = E_POINTER;
	}
	if (SUCCEEDED(*hr)) *hr = ASUKA_SLOT(res, 7, HRESULT(STDMETHODCALLTYPE *)(void *, int *))(res, &status);  // get_Status
	if (SUCCEEDED(*hr) && status == kc_success) {
		*hr = ASUKA_SLOT(res, 6, HRESULT(STDMETHODCALLTYPE *)(void *, void **))(res, &out);  // get_Result
		UINT32 n = 0;
		BYTE *sig = NULL;
		if (SUCCEEDED(*hr) && out) *hr = ASUKA_SLOT(bs, 10, HRESULT(STDMETHODCALLTYPE *)(void *, void *, UINT32 *, BYTE **))(bs, out, &n, &sig);  // CopyToByteArray
		if (SUCCEEDED(*hr) && (!sig || n == 0 || !derive(sig, n, key))) *hr = E_FAIL;
		if (sig) {
			SecureZeroMemory(sig, n);
			CoTaskMemFree(sig);
		}
	}
	asuka_com_release(out);
	asuka_com_release(res);
	asuka_com_release(op);
	asuka_com_release(buf);
	asuka_com_release(bs);
	if (FAILED(*hr)) return hr_outcome(*hr);
	if (status_outcome(status) == hello_failed) *hr = E_FAIL;
	return status_outcome(status);
}

// hello_key is the AES key Windows Hello gives for challenge: the app's
// key credential (made if create and there's none) signing it, behind
// Windows Hello's prompt. Waits: a worker's.
static int hello_key(HWND window, bool create, const uint8_t challenge[challenge_size], asuka_call *cancel, uint8_t key[32], HRESULT *hr) {
	void *st = NULL, *cred = NULL;
	*hr = S_OK;
	if (!asuka_winrt() || FAILED(asuka_winrt_factory(KCM, &iid_kcm_statics, &st))) return hello_unavailable;
	int r = credential(st, window, create, cancel, &cred, hr);
	if (r == hello_ok) r = signed_key(cred, window, challenge, cancel, key, hr);
	asuka_com_release(cred);
	asuka_com_release(st);
	return r;
}

// ---- The keychain ----

static bool spawn(void (*fn)(void *), void *p, asuka_call *call) {
	if (asuka_desktop_spawn(fn, p)) return true;
	asuka_error e = {0};
	asuka_error_failed(&e, "secure", 0, "no thread");
	asuka_call_fail(call, &e);
	return false;
}

// A protected read or write, on a worker.
typedef struct {
	char *key;
	char *value;  // a write's
	uint8_t *blob;  // a read's
	size_t blob_n;
	HWND window;
	asuka_call *call;
} job;

static void job_free(job *j) {
	free(j->key);
	wipe_string(j->value);
	wipe(j->blob, j->blob_n);
	free(j);
}

static void hello_fail(asuka_call *call, int r, HRESULT hr) {
	if (r == hello_cancelled) return fail_declared(call, secure_SecureError_cancelled);
	if (r == hello_unavailable) return fail_declared(call, secure_SecureError_notAvailable);
	asuka_error e = {0};
	asuka_win_fail(&e, hr, "Windows Hello");
	asuka_call_fail(call, &e);
}

// protect_run seals a value with Windows Hello's key and keeps it. The
// write asks for Windows Hello too: the key signs only behind its prompt.
static void protect_run(void *p) {
	job *j = p;
	uint8_t challenge[challenge_size], key[32];
	HRESULT hr = S_OK;
	if (!asuka_winrt() || !supported(j->call)) {
		fail_declared(j->call, secure_SecureError_notAvailable);
	} else if (!BCRYPT_SUCCESS(BCryptGenRandom(NULL, challenge, challenge_size, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
		hello_fail(j->call, hello_failed, E_FAIL);
	} else {
		int r = hello_key(j->window, true, challenge, j->call, key, &hr);
		if (r != hello_ok) {
			hello_fail(j->call, r, hr);
		} else {
			size_t n = 0;
			uint8_t *b = seal(key, challenge, j->key, j->value, &n);
			asuka_error e = {0};
			if (!b) hello_fail(j->call, hello_failed, E_FAIL);
			else if (keep(j->key, b, n, true, &e)) secure_Keychain_set_done(j->call);
			else asuka_call_fail(j->call, &e);
			wipe(b, n);
		}
	}
	SecureZeroMemory(key, sizeof key);
	job_free(j);
}

// unlock_run has Windows Hello sign the value's challenge again (its
// prompt) and opens it.
static void unlock_run(void *p) {
	job *j = p;
	uint8_t key[32];
	HRESULT hr = S_OK;
	const uint8_t *challenge = sealed_challenge(j->blob, j->blob_n);
	int r = challenge ? hello_key(j->window, false, challenge, j->call, key, &hr) : hello_unavailable;
	if (r != hello_ok) {
		hello_fail(j->call, r, hr);
	} else {
		char *v = unseal(key, j->key, j->blob, j->blob_n);
		// None: the app's Windows Hello key isn't the one that sealed it
		// (Windows Hello was reset, its key made again): it can't be read
		// here any more. A declared error carries no text of its own.
		if (v) secure_Keychain_get_done(j->call, v);
		else fail_declared(j->call, secure_SecureError_notAvailable);
	}
	SecureZeroMemory(key, sizeof key);
	job_free(j);
}

static void k_get(secure_Keychain *self, const char *key, const char *prompt, asuka_call *call) {
	asuka_error e = {0};
	uint8_t *b = NULL;
	size_t n = 0;
	bool prot = false;
	int r = stored(key, &b, &n, &prot, &e);
	if (r < 0) return asuka_call_fail(call, &e);
	if (r == 0) return secure_Keychain_get_done(call, NULL);
	if (!prot) return secure_Keychain_get_done(call, (char *)b);  // NUL-ended
	job *j = calloc(1, sizeof *j);
	j->key = asuka_strdup(key);
	j->blob = b;
	j->blob_n = n;
	j->window = (HWND)asuka_desktop_window(0);
	j->call = call;
	if (!spawn(unlock_run, j, call)) job_free(j);
}

static void k_set(secure_Keychain *self, const char *key, const char *value, bool protected, asuka_call *call) {
	asuka_error e = {0};
	if (protected) {
		if (too_long(SEALED_HEAD + strlen(value), &e)) return asuka_call_fail(call, &e);  // before asking
		job *j = calloc(1, sizeof *j);
		j->key = asuka_strdup(key);
		j->value = asuka_strdup(value);
		j->window = (HWND)asuka_desktop_window(0);
		j->call = call;
		if (!spawn(protect_run, j, call)) job_free(j);
		return;
	}
	if (keep(key, value, strlen(value), false, &e)) secure_Keychain_set_done(call);
	else asuka_call_fail(call, &e);
}

static void k_remove(secure_Keychain *self, const char *key, asuka_call *call) {
	wchar_t *t = target(key);
	BOOL ok = CredDeleteW(t, CRED_TYPE_GENERIC, 0);
	DWORD code = ok ? 0 : GetLastError();
	free(t);
	if (ok || code == ERROR_NOT_FOUND) return secure_Keychain_remove_done(call);
	asuka_error e = {0};
	cred_fail(&e, code);
	asuka_call_fail(call, &e);
}

static const secure_Keychain_vtable vt = {.get = k_get, .set = k_set, .remove = k_remove};

secure_Keychain *secure_make_Keychain(asuka_error *err) {
	secure_Keychain *k = malloc(sizeof *k);
	k->vt = &vt;
	return k;
}
