// locale.c: the language the app shows and which way it goes, the user's
// languages and formats, and Asuka's own words.
//
// Frames from the core are physical, and a host never mirrors what the
// core places: a view's window and its DC stay left to right, and what the
// host draws itself takes the direction from wh_is_rtl. The top-level
// windows alone are mirrored, for what Windows draws around the app (the
// title bar, the menu bar and its menus); WS_EX_NOINHERITLAYOUT keeps
// that from their children, and the one child each has fills its client
// area, the same rectangle in either direction.
#include "win.h"

static struct {
	char locale[64];
	char languages[512];
	uint8_t first_weekday, hour_cycle;
} intl;

void wh_read_intl(void) {
	wh_locale_name(intl.locale, sizeof intl.locale);
	wh_preferred_languages(intl.languages, sizeof intl.languages);
	intl.first_weekday = wh_first_weekday();
	intl.hour_cycle = wh_hour_cycle();
}

// wh_intl_env fills the environment's languages and formats: the app's
// own language first (ui.SetLocale's, kept), then the user's without it.
void wh_intl_env(wh_env *e) {
	snprintf(e->locale, sizeof e->locale, "%s", intl.locale);
	char tz[64];
	wh_time_zone(tz, sizeof tz);
	snprintf(e->time_zone, sizeof e->time_zone, "%s", tz);
	e->env.first_weekday = intl.first_weekday, e->env.hour_cycle = intl.hour_cycle;
	if (!H.saved_locale[0]) {
		snprintf(e->languages, sizeof e->languages, "%s", intl.languages);
		return;
	}
	size_t at = (size_t)snprintf(e->languages, sizeof e->languages, "%s", H.saved_locale);
	const char *p = intl.languages;
	while (*p && at < sizeof e->languages) {
		size_t n = strcspn(p, ",");
		if (!(n == strlen(H.saved_locale) && !_strnicmp(p, H.saved_locale, n)))
			at += (size_t)snprintf(e->languages + at, sizeof e->languages - at, ",%.*s", (int)n, p);
		p += n;
		if (*p == ',') p++;
	}
}

// ---- The app's own language (ui.SetLocale): Windows has no per-app
// language, so the host keeps it, in the app's data directory, and
// reports it first in Languages from the next launch on. ----

void wh_read_saved_locale(void) {
	H.saved_locale[0] = 0;
	size_t n;
	char *b = H.locale_path ? dsk_read_file(H.locale_path, &n) : NULL;
	if (!b) return;
	char *s = b;
	while (*s && isspace((unsigned char)*s)) s++;
	size_t l = strlen(s);
	while (l && isspace((unsigned char)s[l - 1])) s[--l] = 0;
	// A tag of letters, digits and dashes.
	bool ok = l > 1 && l < sizeof H.saved_locale;
	for (size_t i = 0; i < l && ok; i++) ok = isalnum((unsigned char)s[i]) || s[i] == '-' || s[i] == '_';
	if (ok) snprintf(H.saved_locale, sizeof H.saved_locale, "%s", s);
	free(b);
}

// wh_save_locale is the SaveLocale command: the language the app chose,
// "" to follow the user's languages again.
void wh_save_locale(const char *tag, size_t len) {
	char t[64];
	snprintf(t, sizeof t, "%.*s", (int)(len < 63 ? len : 63), tag);
	if (!strcmp(t, H.saved_locale)) return;
	snprintf(H.saved_locale, sizeof H.saved_locale, "%s", t);
	if (H.locale_path) {
		if (!t[0]) {
			wchar_t *w = wh_wide(H.locale_path, (int)strlen(H.locale_path));
			DeleteFileW(w);
			free(w);
		} else {
			dsk_write_file(H.locale_path, t, strlen(t));
		}
	}
	wh_environment_changed();
}

#define MIRRORED (WS_EX_LAYOUTRTL | WS_EX_NOINHERITLAYOUT | WS_EX_RTLREADING)

// wh_top_level_style is the extended style of the app's top-level
// windows, in the app's direction.
DWORD wh_top_level_style(void) { return H.rtl ? MIRRORED : 0; }

// mirror_window gives a top-level window the app's direction, whose
// content (its one child) keeps its rectangle and its own coordinates.
void wh_mirror_window(HWND win, node *content) {
	if (!win) return;
	LONG_PTR old = GetWindowLongPtrW(win, GWL_EXSTYLE), next = (old & ~(LONG_PTR)MIRRORED) | (LONG_PTR)wh_top_level_style();
	if (next == old) return;
	SetWindowLongPtrW(win, GWL_EXSTYLE, next);
	// The frame and the menu bar again, in the new direction.
	SetWindowPos(win, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
	if (content && content->hwnd) {
		RECT r;
		GetClientRect(win, &r);
		SetWindowPos(content->hwnd, NULL, 0, 0, r.right, r.bottom, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
	}
	wh_layout_chrome(wh_ctl_of(win));  // a hidden title bar's buttons to the other side
	RedrawWindow(win, NULL, NULL, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_ERASE | RDW_FRAME);
	if (GetMenu(win)) DrawMenuBar(win);
}

// wh_set_locale is the Locale command: the language the app shows (u8
// flags, 1 right to left, then the tag), and its direction.
void wh_set_locale(const uint8_t *args, size_t len) {
	if (len < 1) return;
	bool rtl = args[0] & 1;
	char tag[64];
	snprintf(tag, sizeof tag, "%.*s", (int)(len - 1 < 63 ? len - 1 : 63), (const char *)args + 1);
	if (strcmp(tag, H.locale) != 0) {
		snprintf(H.locale, sizeof H.locale, "%s", tag);
		wh_set_thread_language(tag);
		wh_say_words();
	}
	if (rtl == H.rtl) return;
	H.rtl = rtl;
	wh_mirror_window(H.window, H.root);
	for (uint32_t i = 0; i < H.nsheets; i++) wh_mirror_window(wh_nav_window(H.sheets[i]), H.sheets[i]);
	for (uint32_t i = 0; i < H.nwins; i++) wh_mirror_window(wh_nav_window(H.wins[i]), H.wins[i]);
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v))
		if (((node *)v)->dir == AK_LAYOUT_DIRECTION_INHERIT) wh_direction_changed(v);
	wh_build_menu_bar();
}

// ---- Asuka's own words ----

static const char *word_locale(void) { return H.locale[0] ? H.locale : intl.locale; }

typedef struct {
	char key[32];
	char locale[64];
	wchar_t *w;
} word_entry;

static word_entry words[64];
static uint32_t nwords;

// wh_word is a word of Asuka's in the app's language, UTF-16 (valid until
// the language changes).
const wchar_t *wh_word(const char *key) {
	const char *loc = word_locale();
	for (uint32_t i = 0; i < nwords; i++)
		if (!strcmp(words[i].key, key) && !strcmp(words[i].locale, loc)) return words[i].w;
	if (nwords == WH_LEN(words)) {
		for (uint32_t i = 0; i < nwords; i++) free(words[i].w);
		nwords = 0;
	}
	word_entry *e = &words[nwords++];
	snprintf(e->key, sizeof e->key, "%s", key);
	snprintf(e->locale, sizeof e->locale, "%s", loc);
	const char *s = dsk_word(loc, key);
	e->w = wh_wide(s, (int)strlen(s));
	return e->w;
}

// wh_word_app writes a word naming the app ({app}) into buf.
void wh_word_app(wchar_t *buf, size_t cap, const char *key) {
	char out[512];
	const char *vars[] = {"app", H.app_name};
	dsk_word_format(out, sizeof out, word_locale(), key, vars, 2);
	MultiByteToWideChar(CP_UTF8, 0, out, -1, buf, (int)cap);
	buf[cap - 1] = 0;
}

// wh_word_vars writes a word with its {name}s filled from vars (name,
// value pairs, nvars strings) into buf.
void wh_word_vars(wchar_t *buf, size_t cap, const char *key, const char *const *vars, size_t nvars) {
	char out[512];
	dsk_word_format(out, sizeof out, word_locale(), key, vars, nvars);
	MultiByteToWideChar(CP_UTF8, 0, out, -1, buf, (int)cap);
	buf[cap - 1] = 0;
}

// wh_say_words says the host's own words in the app's language again.
void wh_say_words(void) {
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (n->kind == AK_KIND_STEPPER) wh_name_stepper(n);
		else wh_nav_say_words(n);
	}
}
