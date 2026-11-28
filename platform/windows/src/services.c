// services.c: the smaller of the host's modules, answering as the Mac
// host's do where Windows has the same, and saying it hasn't where it
// hasn't:
//
//   - recent: the shell's recent items for the app, which its jump list's
//     Recent shows, and its own list, newest first, in its data directory.
//   - login: a value of the user's Run key, named for the app, whose
//     command starts it with --asuka-opened-at-login; Task Manager's
//     Startup apps lists it, and turns it off in StartupApproved.
//   - background: a Windows app keeps running out of sight; a task keeps
//     the system from sleeping until the last one ends.
//   - contacts, calendar: Windows gives the user's contacts and calendars
//     to packaged apps only (denied); adding opens the contact's vCard or
//     the event's iCalendar file in the user's app, where they save it.
//   - library: files go to the user's Downloads, Videos, Music and
//     Pictures folders.
//   - compose: a mail through the mailto: app; with attachments, Simple
//     MAPI's composer. No messages.
//   - photoLibrary: the user's Pictures and Videos folders, read as the
//     library is on a phone (Pictures' folders its albums, deleting to
//     the Recycle Bin, changes heard with ReadDirectoryChangesW); a
//     Windows app needs no permission to read them.
//   - push: WNS gives channels to packaged apps only. widget,
//     liveActivity: a phone's. icloud: Apple's.
#include "modules.h"

#include <time.h>
#include <wchar.h>
#include <wincodec.h>

extern bool wh_opened_at_login;  // instance.c

static void ok_nil(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_nil(r);
}

static dsk_json *parse_args(const uint8_t *a, size_t n) {
	dsk_json *j = dsk_json_parse((const char *)a, n);
	if (j && j->type != DSK_JOBJ) {
		dsk_json_free(j);
		return NULL;
	}
	return j;
}

static wchar_t *exe_path(void) {
	wchar_t *p = wh_alloc(MAX_PATH * 4 * sizeof(wchar_t));
	GetModuleFileNameW(NULL, p, MAX_PATH * 4);
	return p;
}

static bool set_user_string(const wchar_t *key, const wchar_t *name, const wchar_t *value) {
	HKEY k;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
	LSTATUS s = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return s == ERROR_SUCCESS;
}

static void delete_user_value(const wchar_t *key, const wchar_t *name) {
	HKEY k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
	RegDeleteValueW(k, name);
	RegCloseKey(k);
}

// ---- Recent documents ----

#define MAX_RECENT 10

static char *recent_path(void) { return dsk_join(H.dirs.data, "recent.json"); }

static dsk_json *read_recent(void) {
	char *p = recent_path();
	size_t n = 0;
	char *s = dsk_read_file(p, &n);
	free(p);
	dsk_json *j = s ? dsk_json_parse(s, n) : NULL;
	free(s);
	if (j && j->type != DSK_JARR) {
		dsk_json_free(j);
		j = NULL;
	}
	return j;
}

static void m_recent_add(wh_reply *r, const uint8_t *a, size_t n) {
	char *path = wh_strndup((const char *)a, n);
	wchar_t *wp = wh_wide(path, -1);
	SHAddToRecentDocs(SHARD_PATHW, wp);
	free(wp);
	// The list with path first (moved up if it's there: Windows paths
	// match whatever their case), at most MAX_RECENT long.
	dsk_json *old = read_recent();
	dsk_jw w = {0};
	dsk_jw_arr(&w);
	dsk_jw_str(&w, path);
	int count = 1;
	for (size_t i = 0; old && i < old->n && count < MAX_RECENT; i++) {
		const dsk_json *v = &old->kids[i];
		if (v->type != DSK_JSTR || !_stricmp(v->str, path)) continue;
		dsk_jw_str(&w, v->str);
		count++;
	}
	dsk_jw_aend(&w);
	dsk_json_free(old);
	size_t len = 0;
	char *s = dsk_jw_take(&w, &len);
	char *p = recent_path();
	bool ok = dsk_write_file(p, s, len);
	free(p);
	free(s);
	free(path);
	if (ok) wh_reply_nil(r);
	else wh_reply_fail(r, "recent: couldn't write the list");
}

static const GUID CLSID_ApplicationDestinations_ = {0x86C14003, 0x4D6B, 0x4EF3, {0xA7, 0xB4, 0x05, 0x06, 0x66, 0x3B, 0x2E, 0x68}};
static const GUID IID_IApplicationDestinations_ = {0x12337D35, 0x94C6, 0x48A0, {0xBC, 0xE7, 0x6A, 0x9C, 0x69, 0xD4, 0xD6, 0x00}};

static void m_recent_clear(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	char *p = recent_path();
	wchar_t *wp = wh_wide(p, -1);
	DeleteFileW(wp);
	free(wp);
	free(p);
	// The jump list's documents (IApplicationDestinations.RemoveAllDestinations),
	// not the user's other recent items.
	void *d = NULL;
	HRESULT hr = CoCreateInstance(&CLSID_ApplicationDestinations_, NULL, CLSCTX_INPROC_SERVER, &IID_IApplicationDestinations_, &d);
	if (SUCCEEDED(hr) && d) {
		typedef HRESULT(STDMETHODCALLTYPE * remove_all_fn)(void *);
		hr = ASUKA_SLOT(d, 5, remove_all_fn)(d);
		asuka_com_release(d);
	}
	if (FAILED(hr)) wh_reply_fail(r, "clearing the jump list: %#lx", (unsigned long)hr);
	else wh_reply_nil(r);
}

static void m_recent_list(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	dsk_json *list = read_recent();
	dsk_jw w = {0};
	dsk_jw_arr(&w);
	for (size_t i = 0; list && i < list->n; i++)
		if (list->kids[i].type == DSK_JSTR && wh_file_exists(list->kids[i].str)) dsk_jw_str(&w, list->kids[i].str);
	dsk_jw_aend(&w);
	dsk_json_free(list);
	wh_reply_json(r, &w);
}

static const wh_method_def recent_methods[] = {
	{"add", m_recent_add}, {"clear", m_recent_clear}, {"list", m_recent_list}, {NULL, NULL},
};
const wh_module_def wh_recent_module = {"recent", recent_methods};

// ---- Start at login ----

#define RUN_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define STARTUP_APPROVED_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"

static wchar_t *login_name(void) { return wh_wide(H.app_id && *H.app_id ? H.app_id : H.app_name ? H.app_name : "Asuka", -1); }

// startup_disabled reports whether Task Manager turned the value off: a
// binary value whose first byte is odd.
static bool startup_disabled(const wchar_t *name) {
	uint8_t b[16];
	DWORD n = sizeof b;
	return wh_reg_binary(HKEY_CURRENT_USER, STARTUP_APPROVED_KEY, name, b, &n) && n > 0 && (b[0] & 1);
}

static void m_login_set(wh_reply *r, const uint8_t *a, size_t n) {
	wchar_t *name = login_name();
	if (!n || !a[0]) {
		delete_user_value(RUN_KEY, name);
		free(name);
		wh_reply_nil(r);
		return;
	}
	wchar_t *exe = exe_path();
	wchar_t cmd[MAX_PATH * 4 + 64];
	swprintf(cmd, WH_LEN(cmd), L"\"%ls\" --asuka-opened-at-login", exe);
	free(exe);
	if (!set_user_string(RUN_KEY, name, cmd)) {
		free(name);
		wh_reply_fail(r, "login: couldn't write the Run key");
		return;
	}
	// Turned on again: no longer off in Task Manager's list.
	if (startup_disabled(name)) delete_user_value(STARTUP_APPROVED_KEY, name);
	free(name);
	wh_reply_nil(r);
}

// names_exe reports whether a Run value's command starts exe.
static bool names_exe(const wchar_t *cmd, const wchar_t *exe) {
	while (*cmd == ' ') cmd++;
	size_t el = wcslen(exe);
	if (*cmd == '"') {
		const wchar_t *end = wcschr(cmd + 1, '"');
		return end && (size_t)(end - cmd - 1) == el && !_wcsnicmp(cmd + 1, exe, el);
	}
	size_t l = wcscspn(cmd, L" \t");
	return l == el && !_wcsnicmp(cmd, exe, el);
}

static void m_login_enabled(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wchar_t *name = login_name();
	wchar_t *cmd = wh_reg_string(HKEY_CURRENT_USER, RUN_KEY, name);
	bool on = false;
	if (cmd) {
		wchar_t *exe = exe_path();
		on = names_exe(cmd, exe) && !startup_disabled(name);
		free(exe);
		free(cmd);
	}
	free(name);
	wh_reply_u8(r, on);
}

static void m_opened_at_login(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_u8(r, wh_opened_at_login);
}

static const wh_method_def login_methods[] = {
	{"set", m_login_set}, {"enabled", m_login_enabled}, {"openedAtLogin", m_opened_at_login}, {NULL, NULL},
};
const wh_module_def wh_login_module = {"login", login_methods};

// ---- Background tasks ----

static struct {
	uint32_t *ids;
	uint32_t n;
	wh_replies next;
} bg;

static uint32_t bg_id(const uint8_t *a, size_t n) {
	dsk_json *j = parse_args(a, n);
	uint32_t id = (uint32_t)dsk_json_num(j, "id", 0);
	dsk_json_free(j);
	return id;
}

static void m_bg_start(wh_reply *r, const uint8_t *a, size_t n) {
	uint32_t id = bg_id(a, n);
	bool have = false;
	for (uint32_t i = 0; i < bg.n; i++) have |= bg.ids[i] == id;
	if (!have) {
		bg.ids = wh_realloc(bg.ids, (bg.n + 1) * sizeof *bg.ids);
		bg.ids[bg.n++] = id;
	}
	wh_keep_system_awake(true);
	wh_reply_nil(r);
}

static void m_bg_end(wh_reply *r, const uint8_t *a, size_t n) {
	uint32_t id = bg_id(a, n);
	for (uint32_t i = 0; i < bg.n; i++)
		if (bg.ids[i] == id) {
			bg.ids[i] = bg.ids[--bg.n];
			if (!bg.n) {
				wh_keep_system_awake(false);
				wh_replies_fail(&bg.next, "ended");
			}
			break;
		}
	wh_reply_nil(r);
}

// next (actions tapped) waits until the last task ends, then fails
// "ended": there's no notification whose actions are tapped.
static void m_bg_next(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	if (!bg.n) {
		wh_reply_fail(r, "ended");
		return;
	}
	wh_replies_add(&bg.next, r);
}

static void m_bg_stop(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_replies_fail(&bg.next, "stopped");
	wh_reply_nil(r);
}

static const wh_method_def background_methods[] = {
	{"start", m_bg_start}, {"update", ok_nil}, {"end", m_bg_end}, {"next", m_bg_next}, {"stop", m_bg_stop}, {NULL, NULL},
};
const wh_module_def wh_background_module = {"background", background_methods};

// ---- Contacts and calendar ----

static void denied(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_u8(r, 1);
}

static void not_allowed(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_fail(r, "not allowed");
}

static void no_picker(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_fail(r, "no contact picker");
}

// open_for_saving writes a file for the user's app of its type (a vCard,
// an iCalendar event) to the caches and opens it there: saved or not, the
// app doesn't say.
static void open_for_saving(wh_reply *r, const char *name, const char *data, size_t len, const char *none) {
	char *dir = wh_cache_path("asuka-personal");
	dsk_mkdirs(dir);
	char *path = dsk_join(dir, name);
	free(dir);
	if (!dsk_write_file(path, data, len)) {
		wh_reply_fail(r, "couldn't write %s", path);
		free(path);
		return;
	}
	DWORD e = wh_shell_open(H.window, NULL, path);
	free(path);
	if (e == ERROR_NO_ASSOCIATION) {
		wh_reply_fail(r, "%s", none);
		return;
	}
	if (e) {
		char *m = wh_win_error(e);
		wh_reply_fail(r, "%s", m);
		free(m);
		return;
	}
	wh_reply_u8(r, 0);
}

// esc appends s escaped for a vCard's or an iCalendar's text.
static void esc(ak_buf *b, const char *s) {
	for (; s && *s; s++) {
		switch (*s) {
		case '\\': ak_buf_put(b, "\\\\", 2); break;
		case ',': ak_buf_put(b, "\\,", 2); break;
		case ';': ak_buf_put(b, "\\;", 2); break;
		case '\n': ak_buf_put(b, "\\n", 2); break;
		default: ak_buf_u8(b, (uint8_t)*s);
		}
	}
}

static const char *vcard_label(const char *l) {
	if (!_stricmp(l, "mobile") || !_stricmp(l, "cell") || !_stricmp(l, "iphone")) return "CELL";
	if (!_stricmp(l, "work")) return "WORK";
	if (!_stricmp(l, "home")) return "HOME";
	return "OTHER";
}

// jstr is a member's string, found whatever its case (Go's JSON was
// case-insensitive).
static const char *jstr(const dsk_json *o, const char *key) {
	for (size_t i = 0; o && o->type == DSK_JOBJ && i < o->n; i++)
		if (!_stricmp(o->keys[i], key) && o->kids[i].type == DSK_JSTR) return o->kids[i].str;
	return "";
}

static const dsk_json *jget(const dsk_json *o, const char *key) {
	for (size_t i = 0; o && o->type == DSK_JOBJ && i < o->n; i++)
		if (!_stricmp(o->keys[i], key)) return &o->kids[i];
	return NULL;
}

static void m_contacts_add(wh_reply *r, const uint8_t *a, size_t n) {
	dsk_json *c = parse_args(a, n);
	if (!c) {
		wh_reply_fail(r, "contacts.add: bad arguments");
		return;
	}
	ak_buf b = {0};
	ak_buf_printf(&b, "BEGIN:VCARD\r\nVERSION:3.0\r\n");
	const char *given = jstr(c, "givenName"), *family = jstr(c, "familyName"), *org = jstr(c, "organization");
	char *fn = wh_strdup(jstr(c, "name"));
	if (!*fn) {
		free(fn);
		fn = wh_alloc(strlen(given) + strlen(family) + 2);
		sprintf(fn, "%s %s", given, family);
		char *s = fn;
		while (*s == ' ') s++;
		memmove(fn, s, strlen(s) + 1);
		size_t l = strlen(fn);
		while (l && fn[l - 1] == ' ') fn[--l] = 0;
	}
	if (!*fn) {
		free(fn);
		fn = wh_strdup(org);
	}
	ak_buf_put(&b, "N:", 2), esc(&b, family), ak_buf_u8(&b, ';'), esc(&b, given), ak_buf_put(&b, ";;;\r\nFN:", 8), esc(&b, fn), ak_buf_put(&b, "\r\n", 2);
	if (*org) ak_buf_put(&b, "ORG:", 4), esc(&b, org), ak_buf_put(&b, "\r\n", 2);
	if (*jstr(c, "jobTitle")) ak_buf_put(&b, "TITLE:", 6), esc(&b, jstr(c, "jobTitle")), ak_buf_put(&b, "\r\n", 2);
	const char *kinds[2] = {"phones", "emails"};
	for (int k = 0; k < 2; k++) {
		const dsk_json *list = jget(c, kinds[k]);
		for (size_t i = 0; list && list->type == DSK_JARR && i < list->n; i++) {
			const dsk_json *it = &list->kids[i];
			if (k == 0) ak_buf_printf(&b, "TEL;TYPE=%s:", vcard_label(jstr(it, "label")));
			else ak_buf_printf(&b, "EMAIL;TYPE=INTERNET,%s:", vcard_label(jstr(it, "label")));
			esc(&b, jstr(it, "value"));
			ak_buf_put(&b, "\r\n", 2);
		}
	}
	if (*jstr(c, "birthday")) ak_buf_printf(&b, "BDAY:%s\r\n", jstr(c, "birthday"));
	if (*jstr(c, "note")) ak_buf_put(&b, "NOTE:", 5), esc(&b, jstr(c, "note")), ak_buf_put(&b, "\r\n", 2);
	ak_buf_printf(&b, "END:VCARD\r\n");
	char *safe = wh_safe_file_name(fn, "contact");
	char *name = wh_alloc(strlen(safe) + 8);
	sprintf(name, "%s.vcf", safe);
	open_for_saving(r, name, (const char *)b.data, b.len, "no contacts");
	free(name);
	free(safe);
	free(fn);
	ak_buf_free(&b);
	dsk_json_free(c);
}

static const wh_method_def contacts_methods[] = {
	{"request", denied}, {"list", not_allowed}, {"pick", no_picker}, {"add", m_contacts_add}, {NULL, NULL},
};
const wh_module_def wh_contacts_module = {"contacts", contacts_methods};

static void ics_time(char *out, size_t cap, int64_t ms, bool date) {
	time_t t = (time_t)(ms / 1000);
	struct tm tm;
	gmtime_s(&tm, &t);
	strftime(out, cap, date ? "%Y%m%d" : "%Y%m%dT%H%M%SZ", &tm);
}

static void m_calendar_add(wh_reply *r, const uint8_t *a, size_t n) {
	dsk_json *e = parse_args(a, n);
	if (!e) {
		wh_reply_fail(r, "calendar.add: bad arguments");
		return;
	}
	const char *title = jstr(e, "title"), *location = jstr(e, "location"), *notes = jstr(e, "notes"), *url = jstr(e, "url");
	const dsk_json *sm = jget(e, "startMs"), *em = jget(e, "endMs"), *ad = jget(e, "allDay");
	int64_t start = sm && sm->type == DSK_JNUM ? (int64_t)sm->num : 0, end = em && em->type == DSK_JNUM ? (int64_t)em->num : 0;
	bool all_day = ad && ad->type == DSK_JTRUE;
	if (end <= start) end = start + (all_day ? 86400000LL : 3600000LL);
	ak_buf b = {0};
	ak_buf_printf(&b, "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Asuka//EN\r\nBEGIN:VEVENT\r\n");
	char now[32], s[32], t[32];
	ics_time(now, sizeof now, (int64_t)time(NULL) * 1000, false);
	ak_buf_printf(&b, "UID:%lld@asuka\r\nDTSTAMP:%s\r\n", (long long)wh_now_ns(), now);
	ics_time(s, sizeof s, start, all_day);
	ics_time(t, sizeof t, end, all_day);
	if (all_day) ak_buf_printf(&b, "DTSTART;VALUE=DATE:%s\r\nDTEND;VALUE=DATE:%s\r\n", s, t);
	else ak_buf_printf(&b, "DTSTART:%s\r\nDTEND:%s\r\n", s, t);
	ak_buf_put(&b, "SUMMARY:", 8), esc(&b, title), ak_buf_put(&b, "\r\n", 2);
	if (*location) ak_buf_put(&b, "LOCATION:", 9), esc(&b, location), ak_buf_put(&b, "\r\n", 2);
	if (*notes) ak_buf_put(&b, "DESCRIPTION:", 12), esc(&b, notes), ak_buf_put(&b, "\r\n", 2);
	if (*url) ak_buf_printf(&b, "URL:%s\r\n", url);
	ak_buf_printf(&b, "END:VEVENT\r\nEND:VCALENDAR\r\n");
	char *safe = wh_safe_file_name(title, "event");
	char *name = wh_alloc(strlen(safe) + 8);
	sprintf(name, "%s.ics", safe);
	open_for_saving(r, name, (const char *)b.data, b.len, "no calendar");
	free(name);
	free(safe);
	ak_buf_free(&b);
	dsk_json_free(e);
}

static const wh_method_def calendar_methods[] = {
	{"request", denied}, {"calendars", not_allowed}, {"events", not_allowed}, {"add", m_calendar_add}, {NULL, NULL},
};
const wh_module_def wh_calendar_module = {"calendar", calendar_methods};

// ---- The library ----

// save_file copies (or moves) src into dir\folder as name, numbered
// "name (1).ext"... if it's taken: the new path, or NULL with *err.
static char *save_file(const char *src, const char *dir, const char *folder, const char *name, bool move, char **err) {
	char *d = wh_strdup(dir);
	if (folder && *folder) {
		char *f = wh_safe_file_name(folder, "Asuka");
		char *j = dsk_join(d, f);
		free(f), free(d);
		d = j;
	}
	if (!dsk_mkdirs(d)) {
		*err = wh_alloc(strlen(d) + 32);
		sprintf(*err, "couldn't make %s", d);
		free(d);
		return NULL;
	}
	const char *dot = strrchr(name, '.');
	size_t base = dot && dot != name ? (size_t)(dot - name) : strlen(name);
	char *dst = dsk_join(d, name);
	for (int i = 1; wh_file_exists(dst); i++) {
		free(dst);
		char *nm = wh_alloc(strlen(name) + 16);
		sprintf(nm, "%.*s (%d)%s", (int)base, name, i, name + base);
		dst = dsk_join(d, nm);
		free(nm);
	}
	free(d);
	wchar_t *ws = wh_wide(src, -1), *wd = wh_wide(dst, -1);
	bool ok = false;
	if (move) ok = MoveFileExW(ws, wd, 0);  // the same volume
	if (!ok) {
		ok = CopyFileExW(ws, wd, NULL, NULL, NULL, COPY_FILE_FAIL_IF_EXISTS);
		if (ok && move) DeleteFileW(ws);
	}
	if (!ok) {
		*err = wh_win_error(GetLastError());
		DeleteFileW(wd);
		free(dst);
		dst = NULL;
	}
	free(ws), free(wd);
	return dst;
}

typedef struct {
	wh_reply *r;
	char *path, *dir, *folder, *name;
	bool move;
	char *out, *err;
} save_job;

static void save_work(void *ctx) {
	save_job *j = ctx;
	j->out = save_file(j->path, j->dir, j->folder, j->name, j->move, &j->err);
}

static void save_done(void *ctx) {
	save_job *j = ctx;
	if (j->out) wh_reply_str(j->r, j->out);
	else wh_reply_fail(j->r, "%s", j->err ? j->err : "couldn't save the file");
	free(j->path), free(j->dir), free(j->folder), free(j->name), free(j->out), free(j->err);
	free(j);
}

static void m_library_save(wh_reply *r, const uint8_t *a, size_t n) {
	dsk_json *o = parse_args(a, n);
	const char *path = dsk_json_str(o, "path", "");
	if (!o || !*path) {
		dsk_json_free(o);
		wh_reply_fail(r, "bad arguments");
		return;
	}
	if (!wh_file_exists(path)) {
		wh_reply_fail(r, "no file at %s", path);
		dsk_json_free(o);
		return;
	}
	// Downloads (0), Videos (1), Music (2), Pictures (3).
	static const GUID *folders[] = {&FOLDERID_Downloads, &FOLDERID_Videos, &FOLDERID_Music, &FOLDERID_Pictures};
	int c = (int)dsk_json_num(o, "collection", 0);
	char *dir = wh_known_folder(folders[c >= 0 && c < 4 ? c : 0]);
	if (!dir) {
		dsk_json_free(o);
		wh_reply_fail(r, "no such folder");
		return;
	}
	const char *name = dsk_json_str(o, "name", "");
	save_job *j = wh_alloc(sizeof *j);
	j->r = r, j->path = wh_strdup(path), j->dir = dir, j->folder = wh_strdup(dsk_json_str(o, "folder", ""));
	j->name = wh_safe_file_name(*name ? name : dsk_base(path), "file");
	j->move = dsk_json_bool(o, "move", false);
	dsk_json_free(o);
	wh_off_main(save_work, save_done, j);
}

static const wh_method_def library_methods[] = {{"save", m_library_save}, {NULL, NULL}};
const wh_module_def wh_library_module = {"library", library_methods};

// ---- Compose ----

static void m_compose_can(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	uint8_t b[4] = {wh_scheme_has_app("mailto"), 0, 0, 0};
	wh_reply_ok(r, b, 4);
}

// Simple MAPI's constants (mapi.h) and structures (its W ones).
enum {
	MAPI_TO_ = 1, MAPI_CC_ = 2, MAPI_BCC_ = 3,
	MAPI_LOGON_UI_ = 0x1, MAPI_DIALOG_ = 0x8,
	MAPI_OK_ = 0, MAPI_USER_ABORT_ = 1, MAPI_E_FAILURE_ = 2, MAPI_E_LOGIN_FAILURE_ = 3, MAPI_E_NOT_SUPPORTED_ = 26,
};

typedef struct {
	ULONG reserved, recip_class;
	PWSTR name, address;
	ULONG eid_size;
	void *entry_id;
} mapi_recip;

typedef struct {
	ULONG reserved, flags, position;
	PWSTR path, name;
	void *file_type;
} mapi_file;

typedef struct {
	ULONG reserved;
	PWSTR subject, note_text, message_type, date_received, conversation_id;
	ULONG flags;
	mapi_recip *originator;
	ULONG recip_count;
	mapi_recip *recips;
	ULONG file_count;
	mapi_file *files;
} mapi_message;

typedef ULONG(WINAPI *mapi_send_fn)(ULONG_PTR session, ULONG_PTR ui, mapi_message *m, ULONG flags, ULONG reserved);

typedef struct {
	wh_reply *r;
	dsk_json *args;
	char **paths;
	uint32_t n;
	char *err;
	ULONG result;
	bool unavailable;
} mail_job;

static void add_recips(mapi_recip **v, ULONG *n, const dsk_json *list, ULONG cls) {
	for (size_t i = 0; list && list->type == DSK_JARR && i < list->n; i++) {
		if (list->kids[i].type != DSK_JSTR) continue;
		*v = wh_realloc(*v, (*n + 1) * sizeof **v);
		mapi_recip *r = &(*v)[(*n)++];
		memset(r, 0, sizeof *r);
		r->recip_class = cls;
		r->name = wh_wide(list->kids[i].str, -1);
		char *addr = wh_alloc(strlen(list->kids[i].str) + 8);
		sprintf(addr, "SMTP:%s", list->kids[i].str);
		r->address = wh_wide(addr, -1);
		free(addr);
	}
}

// mail_work finds the attachments, then shows the user's Simple MAPI mail
// app's composer (Outlook, Thunderbird), waiting for it.
static void mail_work(void *ctx) {
	mail_job *j = ctx;
	const dsk_json *atts = jget(j->args, "attachments");
	uint32_t na = atts && atts->type == DSK_JARR ? (uint32_t)atts->n : 0;
	j->paths = wh_alloc((na ? na : 1) * sizeof *j->paths);
	for (uint32_t i = 0; i < na; i++) {
		j->paths[i] = wh_local_file(jstr(&atts->kids[i], "source"), &j->err);
		j->n = i + 1;
		if (!j->paths[i]) return;
	}
	HMODULE m = LoadLibraryExW(L"mapi32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	mapi_send_fn send = m ? (mapi_send_fn)(void *)GetProcAddress(m, "MAPISendMailW") : NULL;
	if (!send) {
		j->unavailable = true;
		return;
	}
	mapi_recip *recips = NULL;
	ULONG nr = 0;
	add_recips(&recips, &nr, jget(j->args, "to"), MAPI_TO_);
	add_recips(&recips, &nr, jget(j->args, "cc"), MAPI_CC_);
	add_recips(&recips, &nr, jget(j->args, "bcc"), MAPI_BCC_);
	mapi_file *files = wh_alloc((na ? na : 1) * sizeof *files);
	for (uint32_t i = 0; i < na; i++) {
		const char *name = jstr(&atts->kids[i], "name");
		files[i].position = (ULONG)-1;
		files[i].path = wh_wide(j->paths[i], -1);
		files[i].name = wh_wide(*name ? name : dsk_base(j->paths[i]), -1);
	}
	mapi_message msg = {0};
	msg.subject = wh_wide(jstr(j->args, "subject"), -1);
	msg.note_text = wh_wide(jstr(j->args, "text"), -1);
	msg.recip_count = nr, msg.recips = nr ? recips : NULL;
	msg.file_count = na, msg.files = na ? files : NULL;
	// MAPI_LOGON_UI | MAPI_DIALOG: the composer, signed in if need be.
	j->result = send(0, 0, &msg, MAPI_LOGON_UI_ | MAPI_DIALOG_, 0);
	for (ULONG i = 0; i < nr; i++) free(recips[i].name), free(recips[i].address);
	for (uint32_t i = 0; i < na; i++) free(files[i].path), free(files[i].name);
	free(recips), free(files), free(msg.subject), free(msg.note_text);
}

static void mail_done(void *ctx) {
	mail_job *j = ctx;
	if (j->err) wh_reply_fail(j->r, "%s", j->err);
	else if (j->unavailable) wh_reply_fail(j->r, "unavailable");
	else if (j->result == MAPI_OK_) wh_reply_u8(j->r, 3);
	else if (j->result == MAPI_USER_ABORT_) wh_reply_u8(j->r, 0);
	else if (j->result == MAPI_E_FAILURE_ || j->result == MAPI_E_LOGIN_FAILURE_ || j->result == MAPI_E_NOT_SUPPORTED_) wh_reply_fail(j->r, "unavailable");
	else wh_reply_fail(j->r, "MAPI error %lu", j->result);
	for (uint32_t i = 0; i < j->n; i++) free(j->paths[i]);
	free(j->paths);
	free(j->err);
	dsk_json_free(j->args);
	free(j);
}

// mail hands the mail to the user's mail app: its mailto: link; with
// attachments, Simple MAPI, on a thread of its own while the composer
// shows. Answered 3, unknown, or 0 if the user cancelled MAPI's.
static void m_compose_mail(wh_reply *r, const uint8_t *a, size_t n) {
	dsk_json *o = parse_args(a, n);
	if (!o) {
		wh_reply_fail(r, "compose.mail: bad arguments");
		return;
	}
	const dsk_json *atts = jget(o, "attachments");
	if (!atts || atts->type != DSK_JARR || !atts->n) {
		if (!wh_scheme_has_app("mailto")) {
			dsk_json_free(o);
			wh_reply_fail(r, "unavailable");
			return;
		}
		DWORD e = wh_shell_open(H.window, "open", jstr(o, "mailto"));
		dsk_json_free(o);
		if (e) {
			char *m = wh_win_error(e);
			wh_reply_fail(r, "%s", m);
			free(m);
			return;
		}
		wh_reply_u8(r, 3);
		return;
	}
	mail_job *j = wh_alloc(sizeof *j);
	j->r = r, j->args = o;
	wh_off_main(mail_work, mail_done, j);
}

static void unavailable(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_fail(r, "unavailable");
}

static const wh_method_def compose_methods[] = {{"can", m_compose_can}, {"mail", m_compose_mail}, {"message", unavailable}, {NULL, NULL}};
const wh_module_def wh_compose_module = {"compose", compose_methods};

// ---- The photo library: the Pictures and Videos folders ----
//
// The library is what the user's Pictures folder holds (photos, and the
// videos a phone's import puts there) and their Videos folder's videos:
// an asset's id is its path under Pictures ("Camera Roll/IMG_1.jpg"), or
// under Videos after "videos:". Its albums are Pictures' folders (Camera
// Roll and Screenshots are Windows', system ones: nothing's added to
// them); adding to one copies the files in. A favorite is a file rated
// five stars (as the Photos app's heart writes it). Deleting sends the
// files to the Recycle Bin, with the shell's question if the user has it
// ask (Windows doesn't, as it comes). Changes are ReadDirectoryChangesW's
// on both folders, from the first nextChange on.

typedef struct {
	char *rel, *path;  // its id, and its file
	int64_t created;   // ms
	bool shot, video;
} photo;

typedef struct {
	photo *v;
	uint32_t n, cap;
} photos;

static bool ext_in(const wchar_t *name, const wchar_t *const *exts, size_t n) {
	const wchar_t *e = wcsrchr(name, '.');
	if (!e) return false;
	for (size_t i = 0; i < n; i++)
		if (!_wcsicmp(e, exts[i])) return true;
	return false;
}

static bool photo_ext(const wchar_t *name) {
	static const wchar_t *exts[] = {L".jpg", L".jpeg", L".png", L".heic", L".heif", L".webp", L".gif", L".bmp", L".tif", L".tiff", L".avif", L".jxr", L".dng"};
	return ext_in(name, exts, WH_LEN(exts));
}

static bool video_ext(const wchar_t *name) {
	static const wchar_t *exts[] = {L".mp4", L".m4v", L".mov", L".wmv", L".avi", L".mkv", L".webm", L".3gp", L".mts", L".m2ts"};
	return ext_in(name, exts, WH_LEN(exts));
}

static int64_t filetime_ms(FILETIME t) {
	uint64_t v = ((uint64_t)t.dwHighDateTime << 32) | t.dwLowDateTime;
	return (int64_t)(v / 10000) - 11644473600000LL;
}

// What a walk keeps: its kind (0 photos, 1 videos, 2 both), only
// screenshots; the root's ids' prefix ("videos:"), and whether the root
// holds photos at all (Videos doesn't).
typedef struct {
	int kind;
	bool only_shots, photos_too;
	const char *prefix;
	const wchar_t *root, *shots;
	bool count_only;
	uint32_t counted;
} walking;

// media_kind is 1 for a photo, 2 for a video, 0 for neither.
static int media_kind(const walking *q, const wchar_t *name) {
	if (q->photos_too && photo_ext(name)) return 1;
	if (video_ext(name)) return 2;
	return 0;
}

static void walk(photos *out, walking *q, const wchar_t *dir) {
	wchar_t pat[MAX_PATH * 2];
	swprintf(pat, WH_LEN(pat), L"%ls\\*", dir);
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pat, &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
		wchar_t p[MAX_PATH * 2];
		swprintf(p, WH_LEN(p), L"%ls\\%ls", dir, fd.cFileName);
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (fd.cFileName[0] != '.' && !(fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) walk(out, q, p);
			continue;
		}
		int k = media_kind(q, fd.cFileName);
		if (!k || (q->kind == 0 && k != 1) || (q->kind == 1 && k != 2)) continue;
		const wchar_t *shots = q->shots;
		size_t dl = wcslen(dir), sl = shots ? wcslen(shots) : 0;
		const wchar_t *parent = wcsrchr(dir, '\\');
		bool shot = k == 1 && ((sl && !_wcsnicmp(dir, shots, sl) && (dl == sl || dir[sl] == '\\')) || (parent && !_wcsicmp(parent + 1, L"Screenshots")));
		if (q->only_shots && !shot) continue;
		if (q->count_only) {
			q->counted++;
			continue;
		}
		if (out->n == out->cap) {
			out->cap = out->cap ? out->cap * 2 : 64;
			out->v = wh_realloc(out->v, out->cap * sizeof *out->v);
		}
		photo *ph = &out->v[out->n++];
		char *rel = wh_utf8(p + wcslen(q->root) + 1, -1, NULL);
		for (char *c = rel; *c; c++)
			if (*c == '\\') *c = '/';
		ph->rel = wh_alloc(strlen(q->prefix) + strlen(rel) + 1);
		sprintf(ph->rel, "%s%s", q->prefix, rel);
		free(rel);
		ph->path = wh_utf8(p, -1, NULL);
		ph->created = filetime_ms(fd.ftCreationTime);
		ph->shot = shot;
		ph->video = k == 2;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
}

static int newest_first(const void *a, const void *b) {
	const photo *x = a, *y = b;
	return x->created < y->created ? 1 : x->created > y->created ? -1 : 0;
}

static wchar_t *known_root(const GUID *id, bool create) {
	PWSTR p = NULL;
	if (FAILED(SHGetKnownFolderPath(id, create ? KF_FLAG_CREATE : 0, NULL, &p)) || !p) {
		if (p) CoTaskMemFree(p);
		return NULL;
	}
	wchar_t *s = _wcsdup(p);
	CoTaskMemFree(p);
	return s;
}

static wchar_t *pictures_root(void) { return known_root(&FOLDERID_Pictures, true); }
static wchar_t *videos_root(void) { return known_root(&FOLDERID_Videos, false); }

#define VIDEOS_PREFIX "videos:"

// safe_rel reports whether a relative path stays inside its root.
static bool safe_rel(const char *rel) {
	return *rel && !strstr(rel, "..") && !strchr(rel, ':') && rel[0] != '/' && rel[0] != '\\';
}

// asset_path is an asset's file, by its id (never outside the library).
static char *photo_path(const char *id) {
	bool video = !strncmp(id, VIDEOS_PREFIX, strlen(VIDEOS_PREFIX));
	const char *rel = video ? id + strlen(VIDEOS_PREFIX) : id;
	if (!safe_rel(rel)) return NULL;
	wchar_t *root = video ? videos_root() : pictures_root();
	if (!root) return NULL;
	char *r8 = wh_utf8(root, -1, NULL);
	free(root);
	char *rel2 = wh_strdup(rel);
	for (char *c = rel2; *c; c++)
		if (*c == '/') *c = '\\';
	char *p = dsk_join(r8, rel2);
	free(r8), free(rel2);
	if (!wh_file_exists(p)) {
		free(p);
		return NULL;
	}
	return p;
}

// Albums: Pictures' folders. Windows' own (the camera's, screenshots')
// are system albums.
static bool system_album(const wchar_t *path) {
	static const GUID *ids[] = {&FOLDERID_CameraRoll, &FOLDERID_Screenshots};
	for (size_t i = 0; i < WH_LEN(ids); i++) {
		wchar_t *k = known_root(ids[i], false);
		bool same = k && !_wcsicmp(k, path);
		free(k);
		if (same) return true;
	}
	return false;
}

// album_dir is an album's folder, by its id (a folder of Pictures').
static wchar_t *album_dir(const char *id) {
	if (!safe_rel(id) || strchr(id, '/') || strchr(id, '\\')) return NULL;
	wchar_t *root = pictures_root();
	if (!root) return NULL;
	wchar_t *name = wh_wide(id, -1);
	size_t n = wcslen(root) + wcslen(name) + 2;
	wchar_t *p = wh_alloc(n * sizeof *p);
	swprintf(p, n, L"%ls\\%ls", root, name);
	free(root), free(name);
	DWORD a = GetFileAttributesW(p);
	if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
		free(p);
		return NULL;
	}
	return p;
}

typedef struct {
	wh_reply *r;
	bool shots;
	int offset, limit, size, kind;
	char *id, *album, *title;
	dsk_json *args;
	bool count;
	char *out, *err;
} photo_job;

// Shell properties: what the system's index knows of a file.
static const PROPERTYKEY key_duration = {{0x64440490, 0x4c8b, 0x11d1, {0x8b, 0x70, 0x08, 0x00, 0x36, 0xb1, 0x1a, 0x03}}, 3};
static const PROPERTYKEY key_frame_width = {{0x64440491, 0x4c8b, 0x11d1, {0x8b, 0x70, 0x08, 0x00, 0x36, 0xb1, 0x1a, 0x03}}, 3};
static const PROPERTYKEY key_frame_height = {{0x64440491, 0x4c8b, 0x11d1, {0x8b, 0x70, 0x08, 0x00, 0x36, 0xb1, 0x1a, 0x03}}, 4};
static const PROPERTYKEY key_video_orientation = {{0x64440491, 0x4c8b, 0x11d1, {0x8b, 0x70, 0x08, 0x00, 0x36, 0xb1, 0x1a, 0x03}}, 99};
static const PROPERTYKEY key_rating = {{0x64440492, 0x4c8b, 0x11d1, {0x8b, 0x70, 0x08, 0x00, 0x36, 0xb1, 0x1a, 0x03}}, 9};

static bool prop_u64(IPropertyStore *ps, const PROPERTYKEY *k, uint64_t *out) {
	PROPVARIANT v;
	PropVariantInit(&v);
	bool ok = SUCCEEDED(IPropertyStore_GetValue(ps, k, &v));
	if (ok && v.vt == VT_UI4) *out = v.ulVal;
	else if (ok && v.vt == VT_UI8) *out = v.uhVal.QuadPart;
	else ok = false;
	PropVariantClear(&v);
	return ok;
}

// file_props reads a file's rating and, for a video, its frame (turned
// as it plays) and length.
static void file_props(const char *path, bool video, int *w, int *h, int64_t *ms, bool *favorite) {
	wchar_t *wp = wh_wide(path, -1);
	IPropertyStore *ps = NULL;
	if (SUCCEEDED(SHGetPropertyStoreFromParsingName(wp, NULL, GPS_DEFAULT, &IID_IPropertyStore, (void **)&ps)) && ps) {
		uint64_t v = 0;
		*favorite = prop_u64(ps, &key_rating, &v) && v >= 99;  // five stars
		if (video) {
			uint64_t fw = 0, fh = 0, rot = 0;
			if (prop_u64(ps, &key_duration, &v)) *ms = (int64_t)(v / 10000);  // 100 ns
			prop_u64(ps, &key_frame_width, &fw);
			prop_u64(ps, &key_frame_height, &fh);
			prop_u64(ps, &key_video_orientation, &rot);
			bool turned = rot == 90 || rot == 270;
			*w = (int)(turned ? fh : fw);
			*h = (int)(turned ? fw : fh);
		}
		IPropertyStore_Release(ps);
	}
	free(wp);
}

// gather walks what a job asks for: the album's folder, or both roots.
static bool gather(photo_job *j, photos *all, uint32_t *count) {
	wchar_t *pics = pictures_root(), *vids = videos_root();
	if (!pics) {
		free(vids);
		j->err = wh_strdup("no Pictures folder");
		return false;
	}
	wchar_t *shots = known_root(&FOLDERID_Screenshots, false);
	walking q = {.kind = j->kind, .only_shots = j->shots, .photos_too = true, .prefix = "", .root = pics, .shots = shots, .count_only = count != NULL};
	bool ok = true;
	if (j->album && *j->album) {
		wchar_t *dir = album_dir(j->album);
		if (dir) walk(all, &q, dir);
		else ok = false, j->err = wh_strdup("photos: no such album");
		free(dir);
	} else {
		walk(all, &q, pics);
		// Videos' folder, unless it's in Pictures' (walked already).
		size_t pl = wcslen(pics);
		if (vids && !(j->shots) && j->kind != 0 && !(!_wcsnicmp(vids, pics, pl) && (vids[pl] == '\\' || !vids[pl]))) {
			walking v = q;
			v.photos_too = false;
			v.prefix = VIDEOS_PREFIX;
			v.root = vids;
			walk(all, &v, vids);
			q.counted += v.counted;
		}
	}
	if (count) *count = q.counted;
	free(pics), free(vids), free(shots);
	return ok;
}

static void free_photos(photos *all) {
	for (uint32_t i = 0; i < all->n; i++) free(all->v[i].rel), free(all->v[i].path);
	free(all->v);
}

static void list_work(void *ctx) {
	photo_job *j = ctx;
	photos all = {0};
	if (j->count) {
		uint32_t n = 0;
		if (gather(j, &all, &n)) {
			char b[16];
			snprintf(b, sizeof b, "%u", n);
			j->out = wh_strdup(b);
		}
		return;
	}
	if (!gather(j, &all, NULL)) return;
	if (all.n) qsort(all.v, all.n, sizeof *all.v, newest_first);
	dsk_jw w = {0};
	dsk_jw_arr(&w);
	int from = j->offset > 0 ? j->offset : 0, to = from + (j->limit > 0 ? j->limit : 100);
	for (int i = from; i < to && (uint32_t)i < all.n; i++) {
		photo *p = &all.v[i];
		int pw = 0, ph = 0;
		int64_t ms = 0;
		bool fav = false;
		if (!p->video) {
			wchar_t *wp = wh_wide(p->path, -1);
			wh_image_file_size(wp, &pw, &ph);
			free(wp);
		}
		file_props(p->path, p->video, &pw, &ph, &ms, &fav);
		dsk_jw_obj(&w);
		dsk_jw_key(&w, "id"), dsk_jw_str(&w, p->rel);
		dsk_jw_key(&w, "width"), dsk_jw_int(&w, pw);
		dsk_jw_key(&w, "height"), dsk_jw_int(&w, ph);
		dsk_jw_key(&w, "created"), dsk_jw_int(&w, p->created);
		dsk_jw_key(&w, "screenshot"), dsk_jw_bool(&w, p->shot);
		dsk_jw_key(&w, "video"), dsk_jw_bool(&w, p->video);
		dsk_jw_key(&w, "duration"), dsk_jw_int(&w, p->video ? ms : 0);
		dsk_jw_key(&w, "favorite"), dsk_jw_bool(&w, fav);
		dsk_jw_end(&w);
	}
	dsk_jw_aend(&w);
	j->out = dsk_jw_take(&w, NULL);
	free_photos(&all);
}

static void photo_done(void *ctx) {
	photo_job *j = ctx;
	if (j->out) wh_reply_str(j->r, j->out);
	else wh_reply_fail(j->r, "%s", j->err ? j->err : "photos: failed");
	free(j->id), free(j->album), free(j->title), free(j->out), free(j->err);
	dsk_json_free(j->args);
	free(j);
}

static void allowed(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_str(r, "allowed");
}

static photo_job *query_job(wh_reply *r, const uint8_t *a, size_t n, bool count) {
	dsk_json *o = parse_args(a, n);
	photo_job *j = wh_alloc(sizeof *j);
	memset(j, 0, sizeof *j);
	j->r = r;
	j->count = count;
	j->shots = dsk_json_bool(o, "screenshots", false);
	j->offset = (int)dsk_json_num(o, "offset", 0);
	j->limit = (int)dsk_json_num(o, "limit", 100);
	j->kind = (int)dsk_json_num(o, "kind", 0);
	if (j->kind < 0 || j->kind > 2) j->kind = 0;
	j->album = wh_strdup(dsk_json_str(o, "album", ""));
	dsk_json_free(o);
	return j;
}

static void m_photo_list(wh_reply *r, const uint8_t *a, size_t n) { wh_off_main(list_work, photo_done, query_job(r, a, n, false)); }
static void m_photo_count(wh_reply *r, const uint8_t *a, size_t n) { wh_off_main(list_work, photo_done, query_job(r, a, n, true)); }

static photo_job *id_job(wh_reply *r, const uint8_t *a, size_t n) {
	dsk_json *o = parse_args(a, n);
	photo_job *j = wh_alloc(sizeof *j);
	memset(j, 0, sizeof *j);
	j->r = r;
	j->id = wh_strdup(dsk_json_str(o, "id", ""));
	j->size = (int)dsk_json_num(o, "size", 300);
	j->title = wh_strdup(dsk_json_str(o, "title", ""));
	j->album = wh_strdup(dsk_json_str(o, "album", ""));
	j->args = o;
	return j;
}

// video_frame is the shell's thumbnail of a video (a frame of it), as
// straight BGRA, at least size on its short side where the shell's is.
static bool video_frame(const wchar_t *path, int size, uint8_t **px, int *w, int *h) {
	IShellItemImageFactory *f = NULL;
	if (FAILED(SHCreateItemFromParsingName(path, NULL, &IID_IShellItemImageFactory, (void **)&f)) || !f) return false;
	SIZE want = {size * 2, size * 2};  // a square box: its short side fits as the long one does
	HBITMAP bmp = NULL;
	HRESULT hr = IShellItemImageFactory_GetImage(f, want, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &bmp);
	IShellItemImageFactory_Release(f);
	if (FAILED(hr) || !bmp) return false;
	BITMAP bm;
	bool ok = GetObjectW(bmp, sizeof bm, &bm) && bm.bmWidth > 0 && bm.bmHeight != 0;
	if (ok) {
		*w = bm.bmWidth;
		*h = bm.bmHeight < 0 ? -bm.bmHeight : bm.bmHeight;
		BITMAPINFO bi = {0};
		bi.bmiHeader.biSize = sizeof bi.bmiHeader;
		bi.bmiHeader.biWidth = *w;
		bi.bmiHeader.biHeight = -*h;  // top-down
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		*px = wh_alloc((size_t)*w * *h * 4);
		HDC dc = GetDC(NULL);
		ok = GetDIBits(dc, bmp, 0, (UINT)*h, *px, &bi, DIB_RGB_COLORS) == *h;
		ReleaseDC(NULL, dc);
		for (int i = 0; ok && i < *w * *h; i++) (*px)[i * 4 + 3] = 255;  // a video's frame is opaque
		if (!ok) free(*px), *px = NULL;
	}
	DeleteObject(bmp);
	return ok;
}

static void thumb_work(void *ctx) {
	photo_job *j = ctx;
	char *p = photo_path(j->id);
	if (!p) {
		j->err = wh_strdup("no such photo");
		return;
	}
	wchar_t *wp = wh_wide(p, -1);
	WIN32_FILE_ATTRIBUTE_DATA fa = {0};
	GetFileAttributesExW(wp, GetFileExInfoStandard, &fa);
	// Named for the photo and its time: kept for the next time.
	uint64_t hash = 1469598103934665603ULL;
	for (const char *c = j->id; *c; c++) hash = (hash ^ (uint8_t)*c) * 1099511628211ULL;
	hash ^= ((uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime) * 1099511628211ULL;
	char *dir = wh_cache_path("asuka-photo-thumbs");
	dsk_mkdirs(dir);
	char name[64];
	snprintf(name, sizeof name, "%016llx-%d.jpg", (unsigned long long)hash, j->size);
	char *out = dsk_join(dir, name);
	free(dir);
	if (wh_file_exists(out)) {
		j->out = out;
	} else {
		int w = 0, h = 0;
		uint8_t *px = NULL;
		bool got;
		if (video_ext(wp)) {
			got = video_frame(wp, j->size, &px, &w, &h);
		} else {
			int longest = 0;  // small already: whole
			if (wh_image_file_size(wp, &w, &h)) {
				int s = w < h ? w : h, l = w > h ? w : h;
				// At least size on its short side.
				if (s > j->size && s > 0) longest = (l * j->size + s - 1) / s;
			}
			got = wh_decode_image_file(wp, longest, &px, &w, &h);
			// Straight alpha for the encoder.
			for (int i = 0; got && i < w * h; i++) {
				unsigned al = px[i * 4 + 3];
				if (al && al < 255)
					for (int c = 0; c < 3; c++) px[i * 4 + c] = (uint8_t)(px[i * 4 + c] * 255 / al);
			}
		}
		if (!got) {
			j->err = wh_strdup("no thumbnail");
			free(out);
		} else {
			char *tmp = wh_alloc(strlen(out) + 8);
			sprintf(tmp, "%s.tmp", out);
			wchar_t *wt = wh_wide(tmp, -1), *wo = wh_wide(out, -1);
			if (wh_encode_image_file(wt, true, px, w, h) && MoveFileExW(wt, wo, MOVEFILE_REPLACE_EXISTING)) j->out = out;
			else {
				j->err = wh_strdup("no thumbnail");
				free(out);
			}
			free(wt), free(wo), free(tmp);
		}
		free(px);
	}
	free(wp);
	free(p);
}

static void m_photo_thumbnail(wh_reply *r, const uint8_t *a, size_t n) { wh_off_main(thumb_work, photo_done, id_job(r, a, n)); }

static void export_work(void *ctx) {
	photo_job *j = ctx;
	char *p = photo_path(j->id);
	if (!p) {
		j->err = wh_strdup("no such photo");
		return;
	}
	char *dir = wh_cache_path("asuka-photo-exports");
	char sub[32];
	snprintf(sub, sizeof sub, "%llu", (unsigned long long)wh_now_ns());
	char *into = dsk_join(dir, sub);
	free(dir);
	const char *name = dsk_base(p);
	char *dst = save_file(p, into, NULL, name, false, &j->err);
	if (dst) {
		dsk_jw w = {0};
		dsk_jw_obj(&w);
		dsk_jw_key(&w, "path"), dsk_jw_str(&w, dst);
		dsk_jw_key(&w, "name"), dsk_jw_str(&w, name);
		dsk_jw_end(&w);
		j->out = dsk_jw_take(&w, NULL);
		free(dst);
	}
	free(into);
	free(p);
}

static void m_photo_export(wh_reply *r, const uint8_t *a, size_t n) { wh_off_main(export_work, photo_done, id_job(r, a, n)); }

// count_in counts the photos and videos in a folder and its own.
static uint32_t count_in(const wchar_t *root, const wchar_t *dir) {
	walking q = {.kind = 2, .photos_too = true, .prefix = "", .root = root, .count_only = true};
	walk(NULL, &q, dir);
	return q.counted;
}

static void album_json(dsk_jw *w, const char *id, uint32_t count, bool sys) {
	dsk_jw_obj(w);
	dsk_jw_key(w, "id"), dsk_jw_str(w, id);
	dsk_jw_key(w, "title"), dsk_jw_str(w, id);
	dsk_jw_key(w, "count"), dsk_jw_int(w, count);
	dsk_jw_key(w, "system"), dsk_jw_bool(w, sys);
	dsk_jw_end(w);
}

typedef struct {
	char *name;
	uint32_t count;
	bool sys;
} album;

static int by_title(const void *a, const void *b) {
	const album *x = a, *y = b;
	if (x->sys != y->sys) return x->sys ? 1 : -1;  // the user's first
	return _stricmp(x->name, y->name);
}

static void albums_work(void *ctx) {
	photo_job *j = ctx;
	wchar_t *root = pictures_root();
	if (!root) {
		j->err = wh_strdup("no Pictures folder");
		return;
	}
	wchar_t pat[MAX_PATH * 2];
	swprintf(pat, WH_LEN(pat), L"%ls\\*", root);
	WIN32_FIND_DATAW fd;
	album *v = NULL;
	size_t n = 0;
	HANDLE h = FindFirstFileW(pat, &fd);
	if (h != INVALID_HANDLE_VALUE) {
		do {
			if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.' ||
			    (fd.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)))
				continue;
			wchar_t p[MAX_PATH * 2];
			swprintf(p, WH_LEN(p), L"%ls\\%ls", root, fd.cFileName);
			bool sys = system_album(p);
			uint32_t c = count_in(root, p);
			if (sys && !c) continue;  // a system album that holds nothing isn't shown
			v = wh_realloc(v, (n + 1) * sizeof *v);
			v[n++] = (album){wh_utf8(fd.cFileName, -1, NULL), c, sys};
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	if (n) qsort(v, n, sizeof *v, by_title);
	dsk_jw w = {0};
	dsk_jw_arr(&w);
	for (size_t i = 0; i < n; i++) {
		album_json(&w, v[i].name, v[i].count, v[i].sys);
		free(v[i].name);
	}
	dsk_jw_aend(&w);
	free(v);
	free(root);
	j->out = dsk_jw_take(&w, NULL);
}

static void m_photo_albums(wh_reply *r, const uint8_t *a, size_t n) { wh_off_main(albums_work, photo_done, id_job(r, a, n)); }

static void create_album_work(void *ctx) {
	photo_job *j = ctx;
	wchar_t *root = pictures_root();
	if (!root) {
		j->err = wh_strdup("no Pictures folder");
		return;
	}
	char *r8 = wh_utf8(root, -1, NULL);
	free(root);
	char *safe = wh_safe_file_name(j->title, "Album");
	// A folder of the title, numbered if one's there ("Trip (2)").
	char *name = wh_strdup(safe);
	for (int i = 2;; i++) {
		char *p = dsk_join(r8, name);
		bool taken = wh_file_exists(p);
		bool made = !taken && dsk_mkdirs(p);
		free(p);
		if (made) break;
		if (!taken || i > 999) {
			j->err = wh_strdup("photos: couldn't make the album's folder");
			break;
		}
		free(name);
		name = wh_alloc(strlen(safe) + 16);
		sprintf(name, "%s (%d)", safe, i);
	}
	if (!j->err) {
		dsk_jw w = {0};
		album_json(&w, name, 0, false);
		j->out = dsk_jw_take(&w, NULL);
	}
	free(name), free(safe), free(r8);
}

static void m_photo_create_album(wh_reply *r, const uint8_t *a, size_t n) { wh_off_main(create_album_work, photo_done, id_job(r, a, n)); }

static void add_work(void *ctx) {
	photo_job *j = ctx;
	wchar_t *dir = album_dir(j->album);
	if (!dir) {
		j->err = wh_strdup("photos: no such album");
		return;
	}
	if (system_album(dir)) {
		free(dir);
		j->err = wh_strdup("photos: nothing can be added to a system album");
		return;
	}
	char *d8 = wh_utf8(dir, -1, NULL);
	free(dir);
	const dsk_json *ids = jget(j->args, "ids");
	// Each found first: none copied if one's missing.
	for (size_t i = 0; ids && ids->type == DSK_JARR && i < ids->n && !j->err; i++) {
		char *p = ids->kids[i].type == DSK_JSTR ? photo_path(ids->kids[i].str) : NULL;
		if (!p) j->err = wh_strdup("photos: no such photo");
		free(p);
	}
	size_t dl = strlen(d8);
	for (size_t i = 0; !j->err && ids && ids->type == DSK_JARR && i < ids->n; i++) {
		char *p = photo_path(ids->kids[i].str);
		// Already in it (in its folder, not a folder of it): nothing to do.
		bool in = p && !_strnicmp(p, d8, dl) && p[dl] == '\\' && !strchr(p + dl + 1, '\\');
		if (p && !in) {
			char *err = NULL, *dst = save_file(p, d8, NULL, dsk_base(p), false, &err);
			if (!dst) j->err = err ? err : wh_strdup("photos: couldn't copy into the album");
			else free(err);
			free(dst);
		}
		free(p);
	}
	free(d8);
	if (!j->err) j->out = wh_strdup("");
}

static void m_photo_add(wh_reply *r, const uint8_t *a, size_t n) { wh_off_main(add_work, photo_done, id_job(r, a, n)); }

// delete_work sends the files to the Recycle Bin, as Explorer does: with
// its question where the user has it ask; "false" if they said no.
static void delete_work(void *ctx) {
	photo_job *j = ctx;
	const dsk_json *ids = jget(j->args, "ids");
	ak_buf from = {0};  // paths, each NUL-ended, then a NUL
	size_t count = 0;
	for (size_t i = 0; ids && ids->type == DSK_JARR && i < ids->n; i++) {
		char *p = ids->kids[i].type == DSK_JSTR ? photo_path(ids->kids[i].str) : NULL;
		if (!p) {
			j->err = wh_strdup("photos: no such photo");
			break;
		}
		wchar_t *w = wh_wide(p, -1);
		ak_buf_put(&from, w, (wcslen(w) + 1) * sizeof *w);
		free(w), free(p);
		count++;
	}
	if (j->err || !count) {
		ak_buf_free(&from);
		if (!j->err) j->out = wh_strdup("true");
		return;
	}
	wchar_t nul = 0;
	ak_buf_put(&from, &nul, sizeof nul);
	SHFILEOPSTRUCTW op;
	memset(&op, 0, sizeof op);
	op.hwnd = H.window;
	op.wFunc = FO_DELETE;
	op.pFrom = (const wchar_t *)from.data;
	op.fFlags = FOF_ALLOWUNDO | FOF_WANTNUKEWARNING;
	int e = SHFileOperationW(&op);
	ak_buf_free(&from);
	if (op.fAnyOperationsAborted) j->out = wh_strdup("false");
	else if (e) j->err = wh_strdup(e == 0x7C /* DE_INVALIDFILES */ ? "photos: no such photo" : "photos: couldn't delete");
	else j->out = wh_strdup("true");
}

static void m_photo_delete(wh_reply *r, const uint8_t *a, size_t n) { wh_off_main(delete_work, photo_done, id_job(r, a, n)); }

// ---- The library's changes: ReadDirectoryChangesW on both folders, on
// a thread of their own, from the first nextChange on; what comes while
// no one waits is kept, as one change ----

typedef struct {
	char **v;
	size_t n;
} ids;

static struct {
	bool watching;
	wh_replies waiting;
	ids inserted, removed, updated;
	bool changed;  // something changed since the last answer
} lib;

#define MAX_CHANGE_IDS 2000  // more: named as none (list again)

static bool ids_drop(ids *s, const char *id) {
	for (size_t i = 0; i < s->n; i++)
		if (!strcmp(s->v[i], id)) {
			free(s->v[i]);
			s->v[i] = s->v[--s->n];
			return true;
		}
	return false;
}

static bool ids_has(const ids *s, const char *id) {
	for (size_t i = 0; i < s->n; i++)
		if (!strcmp(s->v[i], id)) return true;
	return false;
}

static void ids_add(ids *s, const char *id) {
	if (ids_has(s, id)) return;
	s->v = wh_realloc(s->v, (s->n + 1) * sizeof *s->v);
	s->v[s->n++] = wh_strdup(id);
}

static void ids_clear(ids *s) {
	for (size_t i = 0; i < s->n; i++) free(s->v[i]);
	free(s->v);
	*s = (ids){0};
}

// note keeps what happened to an id since the last answer, as one: added
// then removed is nothing; removed then added, changed.
static void note(int action, const char *id) {
	switch (action) {
	case FILE_ACTION_ADDED:
	case FILE_ACTION_RENAMED_NEW_NAME:
		if (ids_drop(&lib.removed, id)) ids_add(&lib.updated, id);
		else ids_add(&lib.inserted, id);
		break;
	case FILE_ACTION_REMOVED:
	case FILE_ACTION_RENAMED_OLD_NAME:
		if (!ids_drop(&lib.inserted, id)) {
			ids_drop(&lib.updated, id);
			ids_add(&lib.removed, id);
		}
		break;
	default:
		if (!ids_has(&lib.inserted, id)) ids_add(&lib.updated, id);
	}
}

static void ids_json(dsk_jw *w, const char *key, const ids *s) {
	dsk_jw_key(w, key);
	dsk_jw_arr(w);
	for (size_t i = 0; i < s->n; i++) dsk_jw_str(w, s->v[i]);
	dsk_jw_aend(w);
}

// answer gives what's changed to every nextChange waiting.
static void answer(void) {
	if (!lib.changed || !lib.waiting.n) return;
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	bool named = lib.inserted.n + lib.removed.n + lib.updated.n <= MAX_CHANGE_IDS;
	ids none = {0};
	ids_json(&w, "inserted", named ? &lib.inserted : &none);
	ids_json(&w, "removed", named ? &lib.removed : &none);
	ids_json(&w, "updated", named ? &lib.updated : &none);
	dsk_jw_end(&w);
	char *s = dsk_jw_take(&w, NULL);
	for (wh_reply *r; (r = wh_replies_take(&lib.waiting));) wh_reply_str(r, s);
	free(s);
	ids_clear(&lib.inserted), ids_clear(&lib.removed), ids_clear(&lib.updated);
	lib.changed = false;
}

// A batch of what a folder said, for the main thread.
typedef struct {
	size_t n;
	int *actions;
	char **ids;  // NULL: a change it names no asset of (a folder's)
} batch;

static void batch_main(void *ctx) {
	batch *b = ctx;
	for (size_t i = 0; i < b->n; i++) {
		if (b->ids[i]) note(b->actions[i], b->ids[i]);
		free(b->ids[i]);
	}
	if (b->n) lib.changed = true;
	free(b->actions), free(b->ids), free(b);
	answer();
}

typedef struct {
	HANDLE dir, event;
	OVERLAPPED ov;
	DWORD buf[16384];  // 64 KiB, DWORD-aligned as it must be
	const char *prefix;
	bool photos_too;
} watched;

static bool watch_read(watched *d) {
	memset(&d->ov, 0, sizeof d->ov);
	d->ov.hEvent = d->event;
	return ReadDirectoryChangesW(d->dir, d->buf, sizeof d->buf, TRUE,
	                             FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE, NULL,
	                             &d->ov, NULL);
}

// watch_said turns what a folder said into a batch for the main thread:
// the photos' and videos' ids (a folder's change names none).
static void watch_said(watched *d, DWORD got) {
	batch *b = wh_alloc(sizeof *b);
	memset(b, 0, sizeof *b);
	if (!got) {  // more than its buffer held: something changed
		b->n = 1;
		b->actions = wh_alloc(sizeof *b->actions);
		b->ids = wh_alloc(sizeof *b->ids);
		b->actions[0] = FILE_ACTION_MODIFIED;
		b->ids[0] = NULL;
		wh_post(batch_main, b);
		return;
	}
	walking q = {.photos_too = d->photos_too};
	const uint8_t *p = (const uint8_t *)d->buf;
	for (;;) {
		const FILE_NOTIFY_INFORMATION *fi = (const FILE_NOTIFY_INFORMATION *)p;
		size_t len = fi->FileNameLength / sizeof(WCHAR);
		wchar_t *name = wh_alloc((len + 1) * sizeof *name);
		wmemcpy(name, fi->FileName, len);
		name[len] = 0;
		const wchar_t *leaf = wcsrchr(name, '\\');
		leaf = leaf ? leaf + 1 : name;
		char *id = NULL;
		if (leaf[0] != '.' && media_kind(&q, leaf)) {
			char *rel = wh_utf8(name, -1, NULL);
			for (char *c = rel; *c; c++)
				if (*c == '\\') *c = '/';
			id = wh_alloc(strlen(d->prefix) + strlen(rel) + 1);
			sprintf(id, "%s%s", d->prefix, rel);
			free(rel);
		} else if (wcschr(leaf, '.') && fi->Action != FILE_ACTION_REMOVED) {
			free(name);  // another kind of file: not the library's
			goto next;
		}
		free(name);
		b->actions = wh_realloc(b->actions, (b->n + 1) * sizeof *b->actions);
		b->ids = wh_realloc(b->ids, (b->n + 1) * sizeof *b->ids);
		b->actions[b->n] = (int)fi->Action;
		b->ids[b->n++] = id;
	next:
		if (!fi->NextEntryOffset) break;
		p += fi->NextEntryOffset;
	}
	if (b->n) wh_post(batch_main, b);
	else free(b);
}

static DWORD WINAPI watch_thread(void *ctx) {
	watched *ds = ctx;  // Pictures', Videos' (its dir NULL if none)
	HANDLE events[2];
	DWORD n = 0;
	watched *of[2];
	for (int i = 0; i < 2; i++)
		if (ds[i].dir && watch_read(&ds[i])) events[n] = ds[i].event, of[n++] = &ds[i];
	while (n) {
		DWORD k = WaitForMultipleObjects(n, events, FALSE, INFINITE);
		if (k >= WAIT_OBJECT_0 + n) break;
		watched *d = of[k - WAIT_OBJECT_0];
		DWORD got = 0;
		if (!GetOverlappedResult(d->dir, &d->ov, &got, FALSE)) got = 0;
		watch_said(d, got);
		if (!watch_read(d)) break;  // the folder went
	}
	return 0;
}

static void watch_start(void) {
	if (lib.watching) return;
	lib.watching = true;
	watched *ds = wh_alloc(2 * sizeof *ds);
	memset(ds, 0, 2 * sizeof *ds);
	wchar_t *roots[2] = {pictures_root(), videos_root()};
	if (roots[0] && roots[1]) {
		size_t pl = wcslen(roots[0]);
		if (!_wcsnicmp(roots[1], roots[0], pl) && (roots[1][pl] == '\\' || !roots[1][pl])) free(roots[1]), roots[1] = NULL;  // in Pictures: heard there
	}
	for (int i = 0; i < 2; i++) {
		ds[i].prefix = i ? VIDEOS_PREFIX : "";
		ds[i].photos_too = i == 0;
		if (!roots[i]) continue;
		ds[i].dir = CreateFileW(roots[i], FILE_LIST_DIRECTORY, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
		                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, NULL);
		if (ds[i].dir == INVALID_HANDLE_VALUE) ds[i].dir = NULL;
		ds[i].event = CreateEventW(NULL, FALSE, FALSE, NULL);
		free(roots[i]);
	}
	// The thread lives with the app, as the folders' watch does.
	HANDLE t = CreateThread(NULL, 0, watch_thread, ds, 0, NULL);
	if (t) CloseHandle(t);
}

static void m_photo_next_change(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	watch_start();
	wh_replies_add(&lib.waiting, r);
	answer();
}

static void m_photo_stop(wh_reply *r, const uint8_t *a, size_t n) {
	if (n == 10 && !memcmp(a, "nextChange", 10)) wh_replies_fail(&lib.waiting, "stopped");
	wh_reply_nil(r);
}

static const wh_method_def photo_methods[] = {
	{"current", allowed},          {"request", allowed},           {"manage", ok_nil},
	{"list", m_photo_list},        {"count", m_photo_count},       {"thumbnail", m_photo_thumbnail},
	{"export", m_photo_export},    {"albums", m_photo_albums},     {"createAlbum", m_photo_create_album},
	{"addToAlbum", m_photo_add},   {"delete", m_photo_delete},     {"nextChange", m_photo_next_change},
	{"stop", m_photo_stop},        {NULL, NULL},
};
const wh_module_def wh_photo_library_module = {"photoLibrary", photo_methods};

// ---- Push, widgets, live activities, iCloud: not on Windows ----

static wh_replies push_received, push_opened;

static void m_push_register(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_fail(r, "platform/push: Windows gives push channels (WNS) to packaged apps only");
}

// The messages "nextReceived" and "nextOpened" wait for: none ever comes,
// but a wait ends with "stop".
static void m_push_received(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_replies_add(&push_received, r);
}

static void m_push_opened(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_replies_add(&push_opened, r);
}

static void m_push_stop(wh_reply *r, const uint8_t *a, size_t n) {
	if (n == 12 && !memcmp(a, "nextReceived", 12)) wh_replies_fail(&push_received, "stopped");
	if (n == 10 && !memcmp(a, "nextOpened", 10)) wh_replies_fail(&push_opened, "stopped");
	wh_reply_nil(r);
}

static void m_push_rewrite(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_fail(r, "platform/push: rewriting needs a notification service extension, which a Windows app doesn't have");
}

static const wh_method_def push_methods[] = {
	{"register", m_push_register}, {"nextReceived", m_push_received}, {"nextOpened", m_push_opened}, {"stop", m_push_stop},
	{"nextRewrite", m_push_rewrite}, {"rewritten", ok_nil}, {NULL, NULL},
};
const wh_module_def wh_push_module = {"push", push_methods};

static void widget_set(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_fail(r, "not available: widgets are iOS's and Android's");
}

static const wh_method_def widget_methods[] = {{"set", widget_set}, {NULL, NULL}};
const wh_module_def wh_widget_module = {"widget", widget_methods};

static void la_unavailable(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_fail(r, "not available: live activities are iOS's and Android's");
}

static void la_enabled(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_u8(r, 0);
}

static void la_running(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_str(r, "[]");
}

static const wh_method_def live_activity_methods[] = {
	{"enabled", la_enabled}, {"start", la_unavailable}, {"update", ok_nil}, {"end", ok_nil}, {"running", la_running},
	{"next", la_unavailable}, {"stop", ok_nil}, {NULL, NULL},
};
const wh_module_def wh_live_activity_module = {"liveActivity", live_activity_methods};

static void icloud_unavailable(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wh_reply_fail(r, "not available: iCloud is Apple's: a Windows app reaches neither its key-value store nor its containers");
}

static const wh_method_def icloud_methods[] = {
	{"account", icloud_unavailable}, {"kvGet", icloud_unavailable}, {"kvSet", icloud_unavailable}, {"kvDelete", icloud_unavailable},
	{"kvAll", icloud_unavailable}, {"kvSync", icloud_unavailable}, {"container", icloud_unavailable}, {"files", icloud_unavailable},
	{"watch", icloud_unavailable}, {"unwatch", icloud_unavailable}, {"download", icloud_unavailable}, {"evict", icloud_unavailable},
	{"read", icloud_unavailable}, {"write", icloud_unavailable}, {"delete", icloud_unavailable}, {"move", icloud_unavailable},
	{"moveIn", icloud_unavailable}, {"moveOut", icloud_unavailable}, {"versions", icloud_unavailable}, {"resolve", icloud_unavailable},
	{"link", icloud_unavailable}, {"next", icloud_unavailable}, {"stop", ok_nil}, {NULL, NULL},
};
const wh_module_def wh_icloud_module = {"icloud", icloud_methods};
