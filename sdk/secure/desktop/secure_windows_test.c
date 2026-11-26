// secure_windows_test.c: the keychain on Windows (platform/windows'
// TestStdViews, in the signed-in session): secure_windows.c, included,
// with its calls' answers caught here. A value written reads back (UTF-8
// whole) from Credential Manager, unprotected; a protected credential
// carries its mark, and writing either way replaces the other. Sealing:
// a value sealed under a signature's key opens under the same signature's
// and under no other, nor for another name, nor tampered with. A protected
// write is SecureError.notAvailable, keeping nothing, where Windows Hello
// isn't set up (a VM); where it is, it would ask, so it isn't driven. A
// value too long fails; remove forgets, and nothing there is no error.
#define secure_Keychain_get_done t_get_done
#define secure_Keychain_set_done t_set_done
#define secure_Keychain_remove_done t_remove_done
#define asuka_call_fail t_fail
#include "secure_windows.c"

#include <stdio.h>

static int failures;

#define CHECK(c, ...)                                      \
	do {                                                   \
		if (!(c)) {                                        \
			printf("%s:%d: %s: ", __FILE__, __LINE__, #c); \
			printf(__VA_ARGS__);                           \
			printf("\n");                                  \
			failures++;                                    \
		}                                                  \
	} while (0)

// What the last call answered (from the calling thread, or a worker's).
static struct {
	volatile LONG answers;
	char *value;
	asuka_error error;
	bool failed;
} got;

static void reset(void) {
	free(got.value);
	asuka_error_free(&got.error);
	memset((void *)&got, 0, sizeof got);
}

void t_get_done(asuka_call *call, char *value) {
	got.value = value;
	InterlockedIncrement(&got.answers);
}
void t_set_done(asuka_call *call) { InterlockedIncrement(&got.answers); }
void t_remove_done(asuka_call *call) { InterlockedIncrement(&got.answers); }
void t_fail(asuka_call *call, asuka_error *e) {
	got.error = *e;
	memset(e, 0, sizeof *e);
	got.failed = true;
	InterlockedIncrement(&got.answers);
}

// A call no task holds: zeroed, so never cancelled.
static _Alignas(16) unsigned char fake[512];
#define CALL ((asuka_call *)fake)

// answered waits for the call's one answer (a protected write's comes
// from a worker, once Windows Hello has said whether it's set up).
static bool answered(void) {
	for (int i = 0; i < 600 && InterlockedCompareExchange(&got.answers, 0, 0) == 0; i++) Sleep(50);
	return InterlockedCompareExchange(&got.answers, 0, 0) == 1;
}

static const secure_Keychain_vtable *kc;
static secure_Keychain *self;

static void get(const char *key) {
	reset();
	kc->get(self, key, "", CALL);
}

static void set(const char *key, const char *value, bool prot) {
	reset();
	kc->set(self, key, value, prot, CALL);
}

static void del(const char *key) {
	reset();
	kc->remove(self, key, CALL);
}

// mark is key's credential's: 1 protected, 0 not, -1 none; value its blob.
static int mark(const char *key, char **value) {
	asuka_error e = {0};
	bool prot = false;
	uint8_t *b = NULL;
	size_t n = 0;
	int r = stored(key, &b, &n, &prot, &e);
	asuka_error_free(&e);
	*value = (char *)b;
	return r <= 0 ? -1 : prot;
}

static bool contains(const uint8_t *b, size_t n, const char *s) {
	size_t k = strlen(s);
	for (size_t i = 0; i + k <= n; i++)
		if (memcmp(b + i, s, k) == 0) return true;
	return false;
}

// The sealing, under a fixed stand-in for Windows Hello's signature.
static void sealing(const char *text) {
	uint8_t sig[256], other[256], key[32], wrong[32], challenge[challenge_size];
	for (int i = 0; i < 256; i++) sig[i] = (uint8_t)(i * 7 + 1), other[i] = sig[i];
	other[100] ^= 1;
	for (int i = 0; i < challenge_size; i++) challenge[i] = (uint8_t)(0xA0 + i);
	CHECK(derive(sig, sizeof sig, key) && derive(other, sizeof other, wrong), "derive");
	uint8_t again[32];
	CHECK(derive(sig, sizeof sig, again) && memcmp(key, again, 32) == 0, "the same signature, another key");
	CHECK(memcmp(key, wrong, 32) != 0, "another signature, the same key");

	size_t n = 0;
	uint8_t *b = seal(key, challenge, "vault", text, &n);
	CHECK(b && n == SEALED_HEAD + strlen(text), "seal: %zu bytes", n);
	if (!b) return;
	CHECK(sealed_challenge(b, n) && memcmp(sealed_challenge(b, n), challenge, challenge_size) == 0, "the challenge isn't kept");
	CHECK(!contains(b, n, text), "the value is kept in the clear");
	char *v = unseal(key, "vault", b, n);
	CHECK(v && strcmp(v, text) == 0, "unseal: %s", v ? v : "(none)");
	free(v);
	v = unseal(wrong, "vault", b, n);
	CHECK(v == NULL, "another key opened it");
	free(v);
	v = unseal(key, "other", b, n);
	CHECK(v == NULL, "another name opened it");
	free(v);
	b[n - 1] ^= 0x80;
	v = unseal(key, "vault", b, n);
	CHECK(v == NULL, "a tampered value opened");
	free(v);
	b[n - 1] ^= 0x80;
	b[0] = 9;
	CHECK(sealed_challenge(b, n) == NULL && unseal(key, "vault", b, n) == NULL, "another version read");
	free(b);
	CHECK(sealed_challenge(challenge, 3) == NULL, "a short blob read");

	// Two seals of one value: another nonce each.
	size_t n1 = 0, n2 = 0;
	uint8_t *b1 = seal(key, challenge, "vault", text, &n1), *b2 = seal(key, challenge, "vault", text, &n2);
	CHECK(b1 && b2 && n1 == n2 && memcmp(b1 + 1 + challenge_size, b2 + 1 + challenge_size, nonce_size) != 0, "the nonce repeats");
	free(b1);
	free(b2);

	// Nothing.
	b = seal(key, challenge, "vault", "", &n);
	v = b ? unseal(key, "vault", b, n) : NULL;
	CHECK(b && n == SEALED_HEAD && v && v[0] == 0, "an empty value");
	free(v);
	free(b);
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	asuka_error err = {0};
	self = secure_make_Keychain(&err);
	kc = self->vt;
	const char *key = "asuka-secure-test";
	const char *text = "s3cret caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac";
	char *v = NULL;

	sealing(text);

	// None there.
	del(key);
	CHECK(got.answers == 1 && !got.failed, "remove: %s", got.error.message ? got.error.message : "");
	get(key);
	CHECK(got.answers == 1 && !got.failed && got.value == NULL, "get: %s", got.value ? got.value : "");

	// Unprotected: written and read on the calling thread.
	set(key, text, false);
	CHECK(got.answers == 1 && !got.failed, "set: %s", got.error.message ? got.error.message : "");
	get(key);
	CHECK(got.answers == 1 && !got.failed && got.value && strcmp(got.value, text) == 0, "get: %s", got.value ? got.value : "(none)");
	CHECK(mark(key, &v) == 0, "an unprotected write is marked protected");
	free(v);

	// The mark: written protected, then not, each replacing the other.
	CHECK(keep(key, "locked", 6, true, &err), "keep: %s", err.message ? err.message : "");
	CHECK(mark(key, &v) == 1 && v && strcmp(v, "locked") == 0, "not marked protected: %s", v ? v : "(none)");
	free(v);
	CHECK(keep(key, "open", 4, false, &err), "keep: %s", err.message ? err.message : "");
	CHECK(mark(key, &v) == 0 && v && strcmp(v, "open") == 0, "still marked protected: %s", v ? v : "(none)");
	free(v);
	asuka_error_free(&err);

	// A protected credential that isn't a seal (no challenge): notAvailable,
	// without asking.
	CHECK(keep(key, "plain", 5, true, &err), "keep: %s", err.message ? err.message : "");
	get(key);
	CHECK(answered() && got.failed && got.error.status == ASUKA_DECLARED && got.error.index == secure_SecureError_notAvailable, "status %d index %u: %s",
	      got.error.status, got.error.index, got.error.message ? got.error.message : "");
	CHECK(keep(key, "open", 4, false, &err), "keep: %s", err.message ? err.message : "");
	asuka_error_free(&err);

	// Protected: where Windows Hello isn't set up, notAvailable, nothing
	// kept. Where it is, the write asks: not driven here.
	bool hello_set_up = supported(NULL);
	printf("Windows Hello: %s\n", hello_set_up ? "set up (the protected write isn't driven)" : "not set up");
	if (!hello_set_up) {
		set(key, "protected", true);
		CHECK(answered(), "a protected write answered %ld times", got.answers);
		CHECK(got.failed && got.error.status == ASUKA_DECLARED && got.error.index == secure_SecureError_notAvailable, "status %d index %u: %s",
		      got.error.status, got.error.index, got.error.message ? got.error.message : "");
		CHECK(mark(key, &v) == 0 && v && strcmp(v, "open") == 0, "a refused protected write changed the value: %s", v ? v : "(none)");
		free(v);
	}

	// Too long for Credential Manager, protected or not (a protected one
	// fails before asking).
	char *big = malloc(CRED_MAX_CREDENTIAL_BLOB_SIZE + 2);
	memset(big, 'x', CRED_MAX_CREDENTIAL_BLOB_SIZE + 1);
	big[CRED_MAX_CREDENTIAL_BLOB_SIZE + 1] = 0;
	set(key, big, false);
	CHECK(got.answers == 1 && got.failed && got.error.status != ASUKA_DECLARED, "a value too long: status %d", got.error.status);
	big[CRED_MAX_CREDENTIAL_BLOB_SIZE - SEALED_HEAD + 1] = 0;
	set(key, big, true);
	CHECK(got.answers == 1 && got.failed && got.error.status != ASUKA_DECLARED, "a protected value too long: status %d", got.error.status);
	free(big);

	// Removed: gone, and again is no error.
	del(key);
	CHECK(got.answers == 1 && !got.failed, "remove: %s", got.error.message ? got.error.message : "");
	CHECK(mark(key, &v) == -1, "still there");
	del(key);
	CHECK(got.answers == 1 && !got.failed, "remove again: %s", got.error.message ? got.error.message : "");
	reset();
	free(self);

	printf(failures ? "FAIL\n" : "ok\n");
	return failures ? 1 : 0;
}
