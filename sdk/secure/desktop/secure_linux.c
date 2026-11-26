// platform/secure on Linux: the user's keyring through libsecret (the Secret
// Service: GNOME Keyring, KWallet, KeePassXC), each key an item of the
// app's own schema (its ID), labeled with the app's name and the key: the
// Linux host's secure store's items. Without libsecret, or a keyring, the
// calls fail saying so. libsecret answers on GLib's main loop: the calls
// start on the main thread. The Secret Service has no way to ask the user
// to prove it's them before an item is read (an unlocked keyring hands
// out its items): a protected write is SecureError.notAvailable, kept
// nowhere, and a read's prompt goes unused.
#include "asuka_linux.h"
#include "secure.asuka.h"

#include <stdlib.h>
#include <string.h>


static struct {
	bool tried;
	const char *missing;
	SecretSchema *schema;
} ls;

// load opens libsecret, once: false with the reason in ls.missing.
static bool load(void) {
	if (ls.tried) return !ls.missing;
	ls.tried = true;
	if (!asuka_linux() || !asuka_linux_load(ASUKA_LIB_SECRET)) {
		ls.missing = "platform/secure: libsecret isn't installed";
		return false;
	}
	if (!G(secret_schema_newv) || !G(secret_value_new) || !G(secret_value_get) || !G(secret_value_unref) || !G(secret_password_storev_binary) ||
	    !G(secret_password_store_finish) || !G(secret_password_lookupv) || !G(secret_password_lookup_binary_finish) || !G(secret_password_clearv) ||
	    !G(secret_password_clear_finish)) {
		ls.missing = "platform/secure: libsecret is too old: 0.19 or later is needed";
		return false;
	}
	return true;
}

// table is a GHashTable of the key's attributes: {"key": key} (or, for
// the schema, {"key": SECRET_SCHEMA_ATTRIBUTE_STRING}).
static GHashTable *table(const char *key) {
	GHashTable *t = G(g_hash_table_new_full)(G(g_str_hash), G(g_str_equal), G(g_free), key ? G(g_free) : NULL);
	G(g_hash_table_insert)(t, G(g_strdup)("key"), key ? G(g_strdup)(key) : NULL);
	return t;
}

// schema is the app's ID's, made once.
static SecretSchema *schema(void) {
	if (!ls.schema) {
		GHashTable *t = table(NULL);
		ls.schema = G(secret_schema_newv)(asuka_desktop_app_id(), 0, t);
		G(g_hash_table_unref)(t);
	}
	return ls.schema;
}

static void keyring_error(asuka_call *call, GError *e) {
	asuka_error err = {0};
	asuka_linux_fail(&err, e, "platform/secure: the keyring");
	asuka_call_fail(call, &err);
}

static bool ready(asuka_call *call) {
	if (asuka_linux() && load()) return true;
	asuka_error e = {0};
	asuka_error_failed(&e, "secure", 0, "%s", ls.missing ? ls.missing : "no GLib");
	asuka_call_fail(call, &e);
	return false;
}

static void looked_up(GObject *src, GAsyncResult *res, gpointer call) {
	GError *e = NULL;
	SecretValue *v = G(secret_password_lookup_binary_finish)(res, &e);
	if (e) return keyring_error(call, e);
	if (!v) return secure_Keychain_get_done(call, NULL);
	gsize n = 0;
	const char *p = G(secret_value_get)(v, &n);
	char *s = asuka_strndup(p ? p : "", p ? n : 0);
	G(secret_value_unref)(v);
	secure_Keychain_get_done(call, s);
}

static void k_get(secure_Keychain *self, const char *key, const char *prompt, asuka_call *call) {
	if (!ready(call)) return;
	GHashTable *t = table(key);
	G(secret_password_lookupv)(schema(), t, NULL, looked_up, call);
	G(g_hash_table_unref)(t);
}

static void stored(GObject *src, GAsyncResult *res, gpointer call) {
	GError *e = NULL;
	if (!G(secret_password_store_finish)(res, &e)) return keyring_error(call, e);
	secure_Keychain_set_done(call);
}

static void k_set(secure_Keychain *self, const char *key, const char *value, bool protected, asuka_call *call) {
	if (protected) {
		asuka_error e = {0};
		asuka_error_declared(&e, secure_SecureError_notAvailable);
		return asuka_call_fail(call, &e);
	}
	if (!ready(call)) return;
	GHashTable *t = table(key);
	SecretValue *v = G(secret_value_new)(value, strlen(value), "application/octet-stream");
	char *label = asuka_sprintf("%s: %s", asuka_desktop_app_name(), key);
	G(secret_password_storev_binary)(schema(), t, NULL, label, v, NULL, stored, call);  // the default collection
	free(label);
	G(secret_value_unref)(v);
	G(g_hash_table_unref)(t);
}

static void cleared(GObject *src, GAsyncResult *res, gpointer call) {
	GError *e = NULL;
	G(secret_password_clear_finish)(res, &e);
	if (e) return keyring_error(call, e);
	secure_Keychain_remove_done(call);  // none there is no error
}

static void k_remove(secure_Keychain *self, const char *key, asuka_call *call) {
	if (!ready(call)) return;
	GHashTable *t = table(key);
	G(secret_password_clearv)(schema(), t, NULL, cleared, call);
	G(g_hash_table_unref)(t);
}

static const secure_Keychain_vtable vt = {.get = k_get, .set = k_set, .remove = k_remove};

secure_Keychain *secure_make_Keychain(asuka_error *err) {
	secure_Keychain *k = malloc(sizeof *k);
	k->vt = &vt;
	return k;
}
