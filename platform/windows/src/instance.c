// instance.c: the app runs once, a second launch handing the first its
// arguments and working directory; the files and URLs the app is opened
// with; its URL scheme and file types registered for the user; quitting
// and relaunching.
//
// An app with an ID runs once unless asuka.toml's singleInstance is
// false: a named mutex says it runs, and a second launch finds its
// message window by title and hands it the launch (WM_COPYDATA). Not when
// automation or asuka dev runs it, which start it again and again.
#include "win.h"

#define INSTANCE_TAG 0x52795549  // the launch's WM_COPYDATA dwData: "Asuka"
#define LOGIN_ARG L"--asuka-opened-at-login"

static wchar_t *instance_title;
bool wh_opened_at_login;
static bool relaunch;

// not_itself reports whether the app runs for something else than its
// user (automation, asuka dev): it neither hands its launch to an instance
// that runs, nor takes the user's links.
static bool not_itself(void) {
	const char *keys[] = {"ASUKA_SNAPSHOT", "ASUKA_SCRIPT", "ASUKA_COMMANDS", "ASUKA_STATE", "ASUKA_DEV"};
	for (size_t i = 0; i < WH_LEN(keys); i++)
		if (getenv(keys[i])) return true;
	return false;
}

const wchar_t *wh_instance_title(void) { return instance_title ? instance_title : L""; }

// payload is the launch: its working directory and arguments, NUL
// between them, UTF-8.
static char *payload(int argc, wchar_t **argv, size_t *len) {
	ak_buf b = {0};
	wchar_t dir[MAX_PATH * 4];
	DWORD n = GetCurrentDirectoryW(WH_LEN(dir), dir);
	char *d = wh_utf8(dir, (int)n, NULL);
	ak_buf_put(&b, d, strlen(d));
	free(d);
	for (int i = 1; i < argc; i++) {
		ak_buf_u8(&b, 0);
		char *a = wh_utf8(argv[i], -1, NULL);
		ak_buf_put(&b, a, strlen(a));
		free(a);
	}
	char *out = wh_strndup((const char *)b.data, b.len);
	*len = b.len;
	ak_buf_free(&b);
	return out;
}

// send_copy_data sends the launch to w, and lets w's process take the
// foreground. It reports whether w has it.
static bool send_copy_data(HWND w, const char *p, size_t n) {
	DWORD pid = 0;
	GetWindowThreadProcessId(w, &pid);
	if (pid) AllowSetForegroundWindow(pid);
	COPYDATASTRUCT cd = {INSTANCE_TAG, (DWORD)n, (void *)p};
	DWORD_PTR r = 0;
	if (SendMessageTimeoutW(w, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 5000, &r)) return true;
	return GetLastError() == ERROR_TIMEOUT && IsWindow(w);  // it's busy: it has it waiting
}

bool wh_take_instance(int argc, wchar_t **argv) {
	if (H.launched_for_toast || H.config.multiple_instances || !H.app_id || not_itself()) return true;
	// A mutex's name takes no backslash past its namespace.
	char *safe = wh_strdup(H.app_id);
	for (char *p = safe; *p; p++)
		if (*p == '\\') *p = '_';
	wchar_t *wid = wh_wide(safe, (int)strlen(safe));
	free(safe);
	wchar_t mutex[512], title[512];
	swprintf(mutex, WH_LEN(mutex), L"Local\\Asuka.%ls", wid);
	swprintf(title, WH_LEN(title), L"Asuka instance %ls", wid);
	free(wid);
	size_t n;
	char *p = payload(argc, argv, &n);
	bool mine = true;
	for (int i = 0; i < 200; i++) {
		HANDLE h = CreateMutexW(NULL, FALSE, mutex);
		if (!h) break;  // no mutex: run, alone or not
		if (GetLastError() != ERROR_ALREADY_EXISTS) break;  // ours, held until the process ends
		CloseHandle(h);
		HWND w = FindWindowExW(HWND_MESSAGE, NULL, WH_CLASS_MESSAGE, title);
		if (w && send_copy_data(w, p, n)) {
			mine = false;
			break;
		}
		Sleep(50);  // it's starting, or quitting
	}
	free(p);
	if (mine) instance_title = _wcsdup(title);
	return mine;
}

// wh_take_login_arg takes the login entry's argument out of the command
// line, and remembers it.
void wh_take_login_arg(int *argc, wchar_t **argv) {
	for (int i = 1; i < *argc; i++) {
		if (wcscmp(argv[i], LOGIN_ARG) != 0) continue;
		memmove(argv + i, argv + i + 1, (size_t)(*argc - i - 1) * sizeof *argv);
		(*argc)--;
		wh_opened_at_login = true;
		return;
	}
}

// ---- The files and URLs a launch opens ----

// file_url appends path's file:// URL: absolute, slashes forward,
// escaped as Go's url.URL{Scheme: "file", Path: p}.String() escapes a
// path (file:///C:/a%20b.txt).
static void file_url(ak_buf *b, const char *path) {
	ak_buf_put(b, "file://", 7);
	if (path[0] != '/' && path[0] != '\\') ak_buf_u8(b, '/');
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		unsigned char c = *p == '\\' ? '/' : *p;
		bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-._~/:!$&'()*+,;=@", c);
		if (keep && c) ak_buf_u8(b, c);
		else ak_buf_printf(b, "%%%02X", c);
	}
}

static bool exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

// opens is the IncomingURL string of a launch's arguments: its files, as
// file:// URLs, and the URLs of the app's scheme, in order, one per line
// (internal/fileurl's FromArgs). argv[0] isn't one.
static char *opens(int argc, wchar_t **argv, const wchar_t *dir, size_t *len) {
	ak_buf b = {0};
	const char *scheme = H.config.scheme;
	size_t sl = scheme ? strlen(scheme) : 0;
	for (int i = 1; i < argc; i++) {
		const wchar_t *a = argv[i];
		if (!a[0] || a[0] == '-') continue;
		char *u = wh_utf8(a, -1, NULL);
		size_t ul = strlen(u);
		if (sl && ul > sl && _strnicmp(u, scheme, sl) == 0 && u[sl] == ':') {
			if (b.len) ak_buf_u8(&b, '\n');
			for (size_t k = 0; k < ul; k++) {
				if (u[k] == '\r') ak_buf_put(&b, "%0D", 3);
				else if (u[k] == '\n') ak_buf_put(&b, "%0A", 3);
				else ak_buf_u8(&b, (uint8_t)u[k]);
			}
		} else if (ul > 7 && _strnicmp(u, "file://", 7) == 0) {
			if (b.len) ak_buf_u8(&b, '\n');
			ak_buf_put(&b, u, ul);
		} else {
			wchar_t full[MAX_PATH * 4];
			const wchar_t *p = a;
			wchar_t joined[MAX_PATH * 4];
			bool absolute = (a[0] && a[1] == ':') || a[0] == '\\' || a[0] == '/';
			if (!absolute && dir && *dir) {
				swprintf(joined, WH_LEN(joined), L"%ls\\%ls", dir, a);
				p = joined;
			}
			if (GetFullPathNameW(p, WH_LEN(full), full, NULL) && exists(full)) {
				char *f = wh_utf8(full, -1, NULL);
				if (b.len) ak_buf_u8(&b, '\n');
				file_url(&b, f);
				free(f);
			}
		}
		free(u);
	}
	*len = b.len;
	char *out = wh_strndup(b.data ? (const char *)b.data : "", b.len);
	ak_buf_free(&b);
	return out;
}

// ---- Incoming URLs, once the root is mounted ----

static ak_buf pending_urls;  // u32 length, then each

static void send_url(const char *u, size_t n) {
	ak_event e = {.id = H.root->id, .kind = AK_EVENT_INCOMING_URL, .s = ak_strn(u, n)};
	wh_dispatch(&e);
}

typedef struct {
	size_t n;
	char s[];
} url_job;

static void incoming_later(void *ctx) {
	url_job *j = ctx;
	if (!H.root) {
		ak_buf_u32(&pending_urls, (uint32_t)j->n);
		ak_buf_put(&pending_urls, j->s, j->n);
	} else {
		send_url(j->s, j->n);
	}
	free(j);
}

// wh_incoming_url reports what the app was opened with to the root, once
// it's mounted: a URL, or files together, one per line.
void wh_incoming_url(const char *u, size_t n) {
	url_job *j = wh_alloc(sizeof *j + n + 1);
	j->n = n;
	memcpy(j->s, u, n);
	wh_after_core(incoming_later, j);
}

// wh_send_pending_urls sends what came before the root did.
void wh_send_pending_urls(void) {
	if (!H.root) return;
	size_t at = 0;
	while (at + 4 <= pending_urls.len) {
		uint32_t n = wh_u32(pending_urls.data + at);
		send_url((const char *)pending_urls.data + at + 4, n);
		at += 4 + n;
	}
	ak_buf_reset(&pending_urls);
}

// ---- A second launch ----

typedef struct {
	size_t n;
	char p[];
} launch_job;

static void launched_again(void *ctx) {
	launch_job *j = ctx;
	// Its working directory, then its arguments, NUL between them.
	const char *dir = j->p;
	size_t dl = strnlen(dir, j->n);
	wchar_t *wdir = wh_wide(dir, (int)dl);
	int argc = 1;
	for (size_t i = 0; i < j->n; i++)
		if (!j->p[i]) argc++;
	wchar_t **argv = wh_alloc((size_t)(argc + 1) * sizeof *argv);
	argv[0] = _wcsdup(L"");
	int k = 1;
	for (size_t i = dl; i < j->n && k < argc; i++) {
		if (j->p[i]) continue;
		const char *a = j->p + i + 1;
		argv[k++] = wh_wide(a, (int)strnlen(a, j->n - i - 1));
	}
	argc = k;
	// A jump list task's launch: the Dock menu's item, chosen, which says
	// itself whether the app comes to the front.
	if (!wh_dock_item_launch(argc, argv)) {
		wh_to_front();
		if (H.root) {
			// SecondInstance: u32 count, the arguments, the directory.
			ak_buf b = {0};
			ak_buf_u32(&b, (uint32_t)(argc - 1));
			for (int i = 1; i < argc; i++) {
				char *a = wh_utf8(argv[i], -1, NULL);
				ak_buf_str(&b, a, strlen(a));
				free(a);
			}
			ak_buf_str(&b, dir, dl);
			ak_event e = {.id = H.root->id, .kind = AK_EVENT_SECOND_INSTANCE, .s = ak_strn(b.data, b.len)};
			wh_dispatch(&e);
			ak_buf_free(&b);
		}
		size_t n;
		char *u = opens(argc, argv, wdir, &n);
		if (n) wh_incoming_url(u, n);
		free(u);
	}
	for (int i = 0; i < argc; i++) free(argv[i]);
	free(argv);
	free(wdir);
	free(j);
}

// wh_copy_data takes another launch of the app, handed over: handled after
// the message is answered, so that the launch it came from can quit.
LRESULT wh_copy_data(LPARAM lp, bool *handled) {
	COPYDATASTRUCT *cd = (COPYDATASTRUCT *)lp;
	*handled = false;
	if (!cd || cd->dwData != INSTANCE_TAG) return 0;
	launch_job *j = wh_alloc(sizeof *j + cd->cbData + 1);
	j->n = cd->cbData;
	if (cd->cbData) memcpy(j->p, cd->lpData, cd->cbData);
	wh_post(launched_again, j);
	*handled = true;
	return 1;
}

// wh_to_front brings the main window to the front, shown.
void wh_to_front(void) {
	if (!H.no_main) {
		if (H.main_ctl) wh_focus_ctl(H.main_ctl);
		return;
	}
	// No main window: the window in use last, if it's still open.
	wh_wctl *c = H.in_use && IsWindow(H.in_use) ? wh_ctl_of(H.in_use) : NULL;
	if (c && IsWindowVisible(H.in_use)) wh_focus_ctl(c);
}

// ---- The scheme and the file types, registered for the user ----

static bool reg_user_string(const wchar_t *key, const wchar_t *name, wchar_t **out) {
	*out = wh_reg_string(HKEY_CURRENT_USER, key, name && *name ? name : NULL);
	return *out != NULL;
}

static bool set_user_string(const wchar_t *key, const wchar_t *name, const wchar_t *value) {
	HKEY k;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS) return false;
	LSTATUS s = RegSetValueExW(k, name && *name ? name : NULL, 0, REG_SZ, (const BYTE *)value, (DWORD)((wcslen(value) + 1) * sizeof(wchar_t)));
	RegCloseKey(k);
	return s == ERROR_SUCCESS;
}

// ensure sets a value unless it's so (if_unset: unless it has one), and
// reports whether it changed it.
static bool ensure(const wchar_t *key, const wchar_t *name, const wchar_t *value, bool if_unset) {
	wchar_t *have;
	if (reg_user_string(key, name, &have)) {
		bool same = wcscmp(have, value) == 0 || if_unset;
		free(have);
		if (same) return false;
	}
	if (!set_user_string(key, name, value)) {
		wh_log("registering the app's links: %ls", key);
		return false;
	}
	return true;
}

static void register_scheme(const wchar_t *exe) {
	// A packaged app's manifest declares its scheme and file types:
	// what it would write is its package's own registry, which nothing
	// else reads.
	if (!H.config.scheme || not_itself() || wh_packaged()) return;
	wchar_t *scheme = wh_wide(H.config.scheme, (int)strlen(H.config.scheme));
	wchar_t key[512], sub[600], v[MAX_PATH * 4 + 32];
	swprintf(key, WH_LEN(key), L"Software\\Classes\\%ls", scheme);
	swprintf(v, WH_LEN(v), L"URL:%ls", H.app_namew);
	ensure(key, L"", v, false);
	ensure(key, L"URL Protocol", L"", false);
	swprintf(sub, WH_LEN(sub), L"%ls\\DefaultIcon", key);
	swprintf(v, WH_LEN(v), L"\"%ls\",0", exe);
	ensure(sub, L"", v, false);
	swprintf(sub, WH_LEN(sub), L"%ls\\shell\\open\\command", key);
	swprintf(v, WH_LEN(v), L"\"%ls\" \"%%1\"", exe);
	ensure(sub, L"", v, false);
	free(scheme);
}

static long base64url(const char *s, uint8_t *out) {
	uint32_t acc = 0;
	int bits = 0;
	long n = 0;
	for (; *s; s++) {
		int v;
		char c = *s;
		if (c >= 'A' && c <= 'Z') v = c - 'A';
		else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
		else if (c >= '0' && c <= '9') v = c - '0' + 52;
		else if (c == '-' || c == '+') v = 62;
		else if (c == '_' || c == '/') v = 63;
		else if (c == '=') break;
		else return -1;
		acc = acc << 6 | (uint32_t)v;
		bits += 6;
		if (bits >= 8) bits -= 8, out[n++] = (uint8_t)(acc >> bits);
	}
	return n;
}

// prog_id is a type's ProgID: the app's ID (letters, digits and dots)
// and its first extension, at most 39 characters (internal/filetype's).
static void prog_id(char *out, const char *ext) {
	char id[256] = {0};
	size_t k = 0;
	for (const char *p = H.app_id ? H.app_id : ""; *p && k < 200; p++)
		if (*p == '.' || isalnum((unsigned char)*p)) id[k++] = *p;
	char p[300];
	snprintf(p, sizeof p, "%s.%s", id, ext);
	char *s = p;
	while (*s == '.') s++;
	size_t n = strlen(s);
	while (n && s[n - 1] == '.') s[--n] = 0;
	if (n > 39) {
		char *t = s + n - 39;
		char *dot = strchr(t, '.');
		if (dot && (size_t)(dot - t) < 39 - 1 - strlen(ext)) t = dot + 1;  // whole parts
		s = t;
	}
	strcpy(out, s);
}

static void register_file_types(const wchar_t *exe) {
	if (!H.config.file_types || not_itself() || wh_packaged()) return;
	size_t n = strlen(H.config.file_types);
	uint8_t *raw = wh_alloc(n);
	long len = base64url(H.config.file_types, raw);
	dsk_json *types = len > 0 ? dsk_json_parse((const char *)raw, (size_t)len) : NULL;
	free(raw);
	if (!types || types->type != DSK_JARR) {
		dsk_json_free(types);
		return;
	}
	const wchar_t *base = wcsrchr(exe, '\\') ? wcsrchr(exe, '\\') + 1 : exe;
	bool changed = false;
	for (size_t i = 0; i < types->n; i++) {
		const dsk_json *t = &types->kids[i];
		const dsk_json *exts = dsk_json_get(t, "e");
		if (!exts || exts->type != DSK_JARR || !exts->n || exts->kids[0].type != DSK_JSTR) continue;
		char first[64];
		snprintf(first, sizeof first, "%s", exts->kids[0].str);
		for (char *c = first; *c; c++) *c = (char)tolower((unsigned char)*c);
		char prog8[64];
		prog_id(prog8, first);
		wchar_t *prog = wh_wide(prog8, (int)strlen(prog8));
		wchar_t *name = wh_wide(dsk_json_str(t, "n", ""), (int)strlen(dsk_json_str(t, "n", "")));
		wchar_t key[600], sub[700], v[MAX_PATH * 4 + 32];
		swprintf(key, WH_LEN(key), L"Software\\Classes\\%ls", prog);
		changed |= ensure(key, L"", name, false);
		int icon = (int)dsk_json_num(t, "i", 0);
		if (icon) swprintf(v, WH_LEN(v), L"\"%ls\",-%d", exe, icon);
		else swprintf(v, WH_LEN(v), L"\"%ls\",0", exe);
		swprintf(sub, WH_LEN(sub), L"%ls\\DefaultIcon", key);
		changed |= ensure(sub, L"", v, false);
		swprintf(sub, WH_LEN(sub), L"%ls\\shell\\open\\command", key);
		swprintf(v, WH_LEN(v), L"\"%ls\" \"%%1\"", exe);
		changed |= ensure(sub, L"", v, false);
		for (size_t e = 0; e < exts->n; e++) {
			if (exts->kids[e].type != DSK_JSTR) continue;
			const char *x = exts->kids[e].str;
			if (*x == '.') x++;
			char lower[64];
			snprintf(lower, sizeof lower, ".%s", x);
			for (char *c = lower; *c; c++) *c = (char)tolower((unsigned char)*c);
			wchar_t *ext = wh_wide(lower, (int)strlen(lower));
			swprintf(sub, WH_LEN(sub), L"Software\\Classes\\%ls\\OpenWithProgids", ext);
			changed |= ensure(sub, prog, L"", false);
			if (dsk_json_bool(t, "o", false)) {
				swprintf(sub, WH_LEN(sub), L"Software\\Classes\\%ls", ext);
				changed |= ensure(sub, L"", prog, true);
			}
			swprintf(sub, WH_LEN(sub), L"Software\\Classes\\Applications\\%ls\\SupportedTypes", base);
			changed |= ensure(sub, ext, L"", false);
			free(ext);
		}
		free(prog);
		free(name);
	}
	dsk_json_free(types);
	if (changed) SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

// wh_after_launch, once the root is mounted: registers the app's scheme
// and file types for the user, passes on the files and URLs the app was
// opened with, and starts the toasts' activator.
void wh_after_launch(int argc, wchar_t **argv) {
	wchar_t exe[MAX_PATH * 4];
	GetModuleFileNameW(NULL, exe, WH_LEN(exe));
	register_scheme(exe);
	register_file_types(exe);
	wchar_t dir[MAX_PATH * 4];
	GetCurrentDirectoryW(WH_LEN(dir), dir);
	if (!wh_dock_item_launch(argc, argv)) {
		size_t n;
		char *u = opens(argc, argv, dir, &n);
		if (n) wh_incoming_url(u, n);
		free(u);
	}
	wh_send_pending_urls();
	wh_start_toasts();
}

// ---- Quitting: the user's quit (closing the main window), quit(), the
// Quit command, Ctrl+C in its console and the session ending all ask the
// app's on quit handlers first (asuka_quit_request), once: a quit they
// allowed only finishes. ----

static bool quit_allowed;

bool wh_ask_quit(bool ending) {
	if (quit_allowed) return true;
	wh_core_begin();
	int canceled = asuka_quit_request(ending ? 1 : 0);
	wh_core_end();
	if (canceled && !ending) return false;
	quit_allowed = true;
	return true;
}

static void quit_now(void *ctx) {
	bool again = ctx != NULL;
	if (H.quitting || !IsWindow(H.window) || !wh_ask_quit(false)) return;
	relaunch = again;
	wh_lifecycle(AK_LIFECYCLE_BACKGROUND);
	wh_save_placement(H.main_ctl);
	H.quitting = true;
	DestroyWindow(H.window);
}

// wh_quit quits the app as the user's Quit does (a Quit item, the Quit
// command, Ctrl+C), if its handlers let it: the main window closes for
// good, also in an app that keeps running with no window; with relaunch,
// the app starts again once this process ends, with the same arguments.
void wh_quit(bool again) { wh_after_core(quit_now, again ? (void *)1 : NULL); }

// wh_quit_message handles the session ending (WM_QUERYENDSESSION, which
// the app doesn't refuse; WM_ENDSESSION, after which the process may end
// at any moment: what's pending is written then).
LRESULT wh_quit_message(UINT m, WPARAM wp, LPARAM lp, bool *handled) {
	(void)lp;
	*handled = true;
	if (m == WM_QUERYENDSESSION) {
		wh_ask_quit(true);
		return TRUE;
	}
	if (m == WM_ENDSESSION) {
		if (wp) {
			wh_ask_quit(true);
			wh_save_placement(H.main_ctl);
			wh_lifecycle(AK_LIFECYCLE_BACKGROUND);
			wh_flush_routes();
		}
		return 0;
	}
	*handled = false;
	return 0;
}

// wh_finish_quit starts the app again if its quit asked to: the same
// executable (an update put in place is at its path) with the same
// arguments, once this process has let go of its instance.
void wh_finish_quit(void) {
	if (!relaunch) return;
	wchar_t exe[MAX_PATH * 4];
	GetModuleFileNameW(NULL, exe, WH_LEN(exe));
	const wchar_t *cmd = GetCommandLineW();
	wchar_t *line = _wcsdup(cmd);
	STARTUPINFOW si = {sizeof si};
	PROCESS_INFORMATION pi;
	// The new process waits for this one's mutex, as a second launch
	// would wait for it to go: it hands itself over to no one, as this
	// one's message window is gone; it waits for this one to exit first
	// (update.c's wh_relaunch_wait).
	wchar_t pid[16];
	swprintf(pid, WH_LEN(pid), L"%lu", GetCurrentProcessId());
	SetEnvironmentVariableW(L"ASUKA_RELAUNCH_WAIT", pid);
	// Out of the job this one runs in, where the job lets it: a terminal's,
	// a launcher's or Task Scheduler's ends its processes when it does, and
	// the new one outlives this one.
	BOOL ok = CreateProcessW(exe, line, NULL, NULL, FALSE, CREATE_BREAKAWAY_FROM_JOB, NULL, NULL, &si, &pi);
	if (!ok && GetLastError() == ERROR_ACCESS_DENIED) ok = CreateProcessW(exe, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
	if (ok) {
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
	} else {
		wh_log("relaunching: error %lu", GetLastError());
	}
	free(line);
}
