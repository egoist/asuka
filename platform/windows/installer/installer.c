// installer.c: a Windows app's installer and its uninstaller, as asuka
// build makes them (cmd/asuka's windows_installer.go): one program, built
// twice. The installer has the app in it, a zip of its folder (the
// resource PAYLOAD), and installs it for the user who runs it, with no
// administrator's rights: into %LOCALAPPDATA%\Programs\<Name>, which the
// app can update itself in; with a Start menu shortcut (its
// AppUserModelID the app's, as its notifications want), and the app
// listed in Settings' Installed apps (HKCU's Uninstall key); then it
// opens the app. The uninstaller, built without the payload, is in the
// folder beside the app and takes it all away again (the app's data,
// elsewhere, stays). What both say of the app is the resource CONFIG:
// lines of key=value, UTF-8.
//
// The installer is one click, as electron-builder's (NSIS's oneClick):
// a progress dialog (a task dialog, the system's own), the app open
// when it's done. Run again, it updates the install in place: a running
// app is asked to close first, the folder's entries replaced (each put
// aside, then removed), and what the version before installed and this
// one lacks taken away (the folder's .asuka-files, the updater's list).
// Unpacked into a folder beside the install first, so a broken payload
// or a full disk leaves the install as it was.
//
//   Setup.exe [/S] [/D=dir]   /S: silently (no dialog, the app not opened);
//                             /D=: the folder (the last argument, NSIS's)
//   Uninstall.exe [/S]
//
// Exit codes: 0 done, 1 failed, 2 canceled.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <objbase.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "std/flate.h"

#define LEN(a) (sizeof(a) / sizeof((a)[0]))
#define RES_CONFIG 1
#define RES_PAYLOAD 2
#define FILE_LIST L".asuka-files"
#define UNINSTALL_KEY L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"

// The shell's System.AppUserModel.ID and .ToastActivatorCLSID.
static const PROPERTYKEY PKEY_AUMID = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 5};
static const PROPERTYKEY PKEY_ToastActivator = {{0x9F4C2855, 0x9F79, 0x4B39, {0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3}}, 26};

// ---- The allocator flate.c takes memory from ----

static void oom(void) {
	FatalAppExitW(0, L"Out of memory");
}

void *ak_alloc(size_t n) {
	void *p = calloc(1, n ? n : 1);
	if (!p) oom();
	return p;
}

void *ak_alloc_raw(size_t n) {
	void *p = malloc(n ? n : 1);
	if (!p) oom();
	return p;
}

void *ak_realloc(void *p, size_t n) {
	void *q = realloc(p, n ? n : 1);
	if (!q) oom();
	return q;
}

void ak_free(void *p) { free(p); }

// ---- Strings ----

static void *xalloc(size_t n) { return ak_alloc(n); }

static wchar_t *wdup(const wchar_t *s) {
	size_t n = wcslen(s) + 1;
	wchar_t *o = xalloc(n * sizeof *o);
	memcpy(o, s, n * sizeof *o);
	return o;
}

// wcat is a new string of the parts, up to a NULL.
static wchar_t *wcat(const wchar_t *first, ...) {
	va_list ap;
	size_t n = 1;
	va_start(ap, first);
	for (const wchar_t *s = first; s; s = va_arg(ap, const wchar_t *)) n += wcslen(s);
	va_end(ap);
	wchar_t *o = xalloc(n * sizeof *o);
	va_start(ap, first);
	for (const wchar_t *s = first; s; s = va_arg(ap, const wchar_t *)) wcscat(o, s);
	va_end(ap);
	return o;
}

static wchar_t *wjoin(const wchar_t *dir, const wchar_t *name) {
	size_t n = wcslen(dir);
	return wcat(dir, n && dir[n - 1] != L'\\' ? L"\\" : L"", name, (wchar_t *)NULL);
}

static wchar_t *wide(const char *s, int n) {
	if (n < 0) n = (int)strlen(s);
	int k = MultiByteToWideChar(CP_UTF8, 0, s, n, NULL, 0);
	wchar_t *o = xalloc(((size_t)k + 1) * sizeof *o);
	MultiByteToWideChar(CP_UTF8, 0, s, n, o, k);
	return o;
}

static wchar_t *wfmt(const wchar_t *f, ...) {
	va_list ap;
	va_start(ap, f);
	int n = _vscwprintf(f, ap);
	va_end(ap);
	wchar_t *o = xalloc(((size_t)n + 1) * sizeof *o);
	va_start(ap, f);
	_vsnwprintf(o, (size_t)n + 1, f, ap);
	va_end(ap);
	return o;
}

// ---- What the app is (CONFIG) ----

typedef struct {
	wchar_t *name;       // as people see it: the Start menu's, Installed apps'
	wchar_t *version;    // shown in Installed apps
	wchar_t *publisher;  // likewise ("" for none)
	wchar_t *id;         // the Uninstall key's name: the app's ID
	wchar_t *aumid;      // the app's AppUserModelID (the shortcut's)
	wchar_t *exe;        // the app's executable, in the folder
	wchar_t *folder;     // the folder's name, under Programs
	wchar_t *uninstaller;  // the uninstaller's name, in the folder
	wchar_t *homepage;
	wchar_t *scheme;       // the URL scheme the app registers ("": none)
	wchar_t *progids;      // the file types' ProgIDs it registers, ;-separated
	wchar_t *exts;         // their extensions, ;-separated (".txt")
	wchar_t *activator;    // its toasts' activator's CLSID ("{...}")
	bool desktop;          // a desktop shortcut too
} config;

static config C;

static wchar_t *field(const char *text, size_t n, const char *key) {
	size_t kl = strlen(key);
	for (size_t i = 0; i < n;) {
		size_t e = i;
		while (e < n && text[e] != '\n') e++;
		if (e - i > kl && !memcmp(text + i, key, kl) && text[i + kl] == '=') {
			size_t v = i + kl + 1, ve = e;
			if (ve > v && text[ve - 1] == '\r') ve--;
			return wide(text + v, (int)(ve - v));
		}
		i = e + 1;
	}
	return wdup(L"");
}

static const uint8_t *resource(int id, DWORD *size) {
	HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));  // RT_RCDATA
	if (!r) return NULL;
	HGLOBAL g = LoadResource(NULL, r);
	if (!g) return NULL;
	*size = SizeofResource(NULL, r);
	return LockResource(g);
}

static bool read_config(void) {
	DWORD n = 0;
	const char *t = (const char *)resource(RES_CONFIG, &n);
	if (!t) return false;
	C.name = field(t, n, "name");
	C.version = field(t, n, "version");
	C.publisher = field(t, n, "publisher");
	C.id = field(t, n, "id");
	C.aumid = field(t, n, "aumid");
	C.exe = field(t, n, "exe");
	C.folder = field(t, n, "folder");
	C.uninstaller = field(t, n, "uninstaller");
	C.homepage = field(t, n, "homepage");
	C.scheme = field(t, n, "scheme");
	C.progids = field(t, n, "progids");
	C.exts = field(t, n, "exts");
	C.activator = field(t, n, "activator");
	wchar_t *d = field(t, n, "desktop");
	C.desktop = !wcscmp(d, L"1");
	free(d);
	return *C.name && *C.id && *C.exe && *C.folder;
}

// ---- The run: silent or with a dialog, its log ----

static bool silent;
static wchar_t *log_path;
static wchar_t failure[1024];  // what went wrong, for the dialog
static volatile LONG canceled;
static HWND dialog;  // the progress dialog, while it's up

static void say(const wchar_t *f, ...) {
	if (!log_path) return;
	wchar_t line[2048];
	va_list ap;
	va_start(ap, f);
	_vsnwprintf(line, LEN(line) - 2, f, ap);
	va_end(ap);
	line[LEN(line) - 3] = 0;
	wcscat(line, L"\r\n");
	int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, NULL, 0, NULL, NULL);
	char *u = xalloc((size_t)n);
	WideCharToMultiByte(CP_UTF8, 0, line, -1, u, n, NULL, NULL);
	HANDLE h = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w;
		WriteFile(h, u, (DWORD)strlen(u), &w, NULL);
		CloseHandle(h);
	}
	free(u);
}

// fail says what went wrong (with the system's message for err, if any),
// once: the first failure is the one shown.
static bool fail(DWORD err, const wchar_t *f, ...) {
	wchar_t what[768];
	va_list ap;
	va_start(ap, f);
	_vsnwprintf(what, LEN(what) - 1, f, ap);
	va_end(ap);
	what[LEN(what) - 1] = 0;
	wchar_t sys[256] = L"";
	if (err) {
		FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, err, 0, sys, LEN(sys), NULL);
		size_t n = wcslen(sys);
		while (n && (sys[n - 1] == L'\n' || sys[n - 1] == L'\r' || sys[n - 1] == L'.')) sys[--n] = 0;
	}
	say(L"error: %ls%ls%ls", what, *sys ? L": " : L"", sys);
	if (!*failure) _snwprintf(failure, LEN(failure) - 1, L"%ls%ls%ls.", what, *sys ? L": " : L"", sys);
	return false;
}

static void progress(uint64_t done, uint64_t total) {
	if (!dialog || !total) return;
	SendMessageW(dialog, TDM_SET_PROGRESS_BAR_POS, (WPARAM)(done * 1000 / total), 0);
}

// ---- Files ----

static bool exists(const wchar_t *p) { return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }

static bool is_dir(const wchar_t *p) {
	DWORD a = GetFileAttributesW(p);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
}

static wchar_t *parent_of(const wchar_t *p) {
	wchar_t *d = wdup(p);
	wchar_t *s = wcsrchr(d, L'\\');
	if (s) *s = 0;
	return d;
}

static bool make_dirs(const wchar_t *dir) {
	if (is_dir(dir)) return true;
	int r = SHCreateDirectoryExW(NULL, dir, NULL);
	return r == ERROR_SUCCESS || r == ERROR_ALREADY_EXISTS || r == ERROR_FILE_EXISTS;
}

// remove_all removes a file, or a folder and what's in it (a link to a
// folder, not what it links to).
static bool remove_all(const wchar_t *p) {
	DWORD a = GetFileAttributesW(p);
	if (a == INVALID_FILE_ATTRIBUTES) return true;
	if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p, a & ~(DWORD)FILE_ATTRIBUTE_READONLY);
	if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return DeleteFileW(p);
	if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
		wchar_t *pat = wjoin(p, L"*");
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW(pat, &fd);
		free(pat);
		if (h != INVALID_HANDLE_VALUE) {
			do {
				if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
				wchar_t *c = wjoin(p, fd.cFileName);
				remove_all(c);
				free(c);
			} while (FindNextFileW(h, &fd));
			FindClose(h);
		}
	}
	return RemoveDirectoryW(p);
}

// A list of names: a folder's entries, the file list's.
typedef struct {
	wchar_t **v;
	size_t n, cap;
} names;

static void names_add(names *l, wchar_t *s) {
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 16;
		l->v = ak_realloc(l->v, l->cap * sizeof *l->v);
	}
	l->v[l->n++] = s;
}

static bool names_has(const names *l, const wchar_t *s) {
	for (size_t i = 0; i < l->n; i++)
		if (!_wcsicmp(l->v[i], s)) return true;
	return false;
}

static void names_free(names *l) {
	for (size_t i = 0; i < l->n; i++) free(l->v[i]);
	free(l->v);
	*l = (names){0};
}

// split is s's parts between semicolons.
static names split(const wchar_t *s) {
	names l = {0};
	while (*s) {
		const wchar_t *e = wcschr(s, L';');
		size_t n = e ? (size_t)(e - s) : wcslen(s);
		if (n) {
			wchar_t *p = xalloc((n + 1) * sizeof *p);
			memcpy(p, s, n * sizeof *p);
			names_add(&l, p);
		}
		if (!e) break;
		s = e + 1;
	}
	return l;
}

// read_list reads dir's .asuka-files: the folder's entries its version
// installed, one a line, with slashes (Windows' are top-level names).
static names read_list(const wchar_t *dir) {
	names l = {0};
	wchar_t *p = wjoin(dir, FILE_LIST);
	HANDLE h = CreateFileW(p, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	free(p);
	if (h == INVALID_HANDLE_VALUE) return l;
	LARGE_INTEGER size;
	if (!GetFileSizeEx(h, &size) || size.QuadPart > 16 << 20) {
		CloseHandle(h);
		return l;
	}
	char *b = xalloc((size_t)size.QuadPart + 1);
	DWORD got = 0;
	ReadFile(h, b, (DWORD)size.QuadPart, &got, NULL);
	CloseHandle(h);
	for (DWORD i = 0; i < got;) {
		DWORD e = i;
		while (e < got && b[e] != '\n') e++;
		DWORD ve = e;
		if (ve > i && b[ve - 1] == '\r') ve--;
		if (ve > i) {
			wchar_t *s = wide(b + i, (int)(ve - i));
			wchar_t *slash = wcschr(s, L'/');
			if (slash) *slash = 0;  // a unit is the first part
			if (*s && wcscmp(s, L".") && wcscmp(s, L"..") && !names_has(&l, s)) names_add(&l, s);
			else free(s);
		}
		i = e + 1;
	}
	free(b);
	return l;
}

static bool write_list(const wchar_t *dir, const names *l) {
	size_t cap = 1;
	for (size_t i = 0; i < l->n; i++) cap += wcslen(l->v[i]) * 3 + 1;
	char *b = xalloc(cap), *at = b;
	for (size_t i = 0; i < l->n; i++) {
		at += WideCharToMultiByte(CP_UTF8, 0, l->v[i], -1, at, (int)(cap - (size_t)(at - b)), NULL, NULL) - 1;
		*at++ = '\n';
	}
	wchar_t *p = wjoin(dir, FILE_LIST), *tmp = wcat(p, L".tmp", (wchar_t *)NULL);
	bool ok = false;
	HANDLE h = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h != INVALID_HANDLE_VALUE) {
		DWORD w;
		ok = WriteFile(h, b, (DWORD)(at - b), &w, NULL) && w == (DWORD)(at - b);
		CloseHandle(h);
		ok = ok && MoveFileExW(tmp, p, MOVEFILE_REPLACE_EXISTING);
		if (!ok) DeleteFileW(tmp);
	}
	free(b), free(p), free(tmp);
	return ok;
}

// ---- The payload: a zip of the app's folder ----

static uint32_t crc_table[256];

static uint32_t crc32_of(uint32_t crc, const uint8_t *p, size_t n) {
	if (!crc_table[1]) {
		for (uint32_t i = 0; i < 256; i++) {
			uint32_t c = i;
			for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
			crc_table[i] = c;
		}
	}
	crc = ~crc;
	for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
	return ~crc;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

typedef struct {
	wchar_t *path;  // relative, with backslashes
	bool dir;
	uint16_t method;  // 0 stored, 8 deflated
	uint32_t crc, csize, usize;
	const uint8_t *data;  // its compressed bytes, in the payload
} entry;

typedef struct {
	entry *v;
	size_t n;
	uint64_t total;  // the uncompressed bytes
} archive;

// safe_path reports whether a zip's name stays in the folder: relative,
// no part . or .., none a device's or with a drive's colon.
static bool safe_path(const wchar_t *p) {
	if (!*p || *p == L'\\' || wcschr(p, L':')) return false;
	const wchar_t *s = p;
	while (*s) {
		const wchar_t *e = wcschr(s, L'\\');
		size_t n = e ? (size_t)(e - s) : wcslen(s);
		if ((n == 1 && s[0] == L'.') || (n == 2 && s[0] == L'.' && s[1] == L'.')) return false;
		if (!e) break;
		s = e + 1;
	}
	return true;
}

static bool read_archive(const uint8_t *z, size_t n, archive *a) {
	*a = (archive){0};
	// The end of the central directory: the last 22 bytes and a comment.
	size_t eocd = SIZE_MAX;
	for (size_t i = n >= 22 ? n - 22 : 0, stop = n > 22 + 65535 ? n - 22 - 65535 : 0;; i--) {
		if (n >= 22 && le32(z + i) == 0x06054b50) {
			eocd = i;
			break;
		}
		if (i == stop) break;
	}
	if (eocd == SIZE_MAX) return fail(0, L"The installer is damaged (no archive in it)");
	uint16_t count = le16(z + eocd + 10);
	uint32_t cd = le32(z + eocd + 16);
	if (cd >= n) return fail(0, L"The installer is damaged (its archive's directory)");
	a->v = xalloc(sizeof(entry) * (count ? count : 1));
	size_t at = cd;
	for (uint16_t i = 0; i < count; i++) {
		if (at + 46 > n || le32(z + at) != 0x02014b50) return fail(0, L"The installer is damaged (an entry)");
		uint16_t method = le16(z + at + 10), nl = le16(z + at + 28), xl = le16(z + at + 30), cl = le16(z + at + 32);
		uint32_t crc = le32(z + at + 16), csize = le32(z + at + 20), usize = le32(z + at + 24), local = le32(z + at + 42);
		if (at + 46 + nl > n || local + 30 > n) return fail(0, L"The installer is damaged (an entry)");
		const char *name = (const char *)z + at + 46;
		size_t lnl = le16(z + local + 26), lxl = le16(z + local + 28);
		size_t data = local + 30 + lnl + lxl;
		if (data + csize > n || (method != 0 && method != 8)) return fail(0, L"The installer is damaged (an entry's data)");
		entry *e = &a->v[a->n];
		e->path = wide(name, nl);
		for (wchar_t *c = e->path; *c; c++)
			if (*c == L'/') *c = L'\\';
		size_t pl = wcslen(e->path);
		if (pl && e->path[pl - 1] == L'\\') e->dir = true, e->path[pl - 1] = 0;
		if (!safe_path(e->path)) return fail(0, L"The installer is damaged (a path outside the folder: %ls)", e->path);
		e->method = method, e->crc = crc, e->csize = csize, e->usize = usize, e->data = z + data;
		a->total += usize;
		a->n++;
		at += 46 + (size_t)nl + xl + cl;
	}
	return true;
}

static bool write_all(HANDLE h, const uint8_t *p, size_t n) {
	while (n) {
		DWORD k = n > (1u << 30) ? 1u << 30 : (DWORD)n, w = 0;
		if (!WriteFile(h, p, k, &w, NULL) || !w) return false;
		p += w, n -= w;
	}
	return true;
}

// extract writes the entry under dir, checking its CRC.
static bool extract(const entry *e, const wchar_t *dir, uint64_t *done, uint64_t total) {
	wchar_t *p = wjoin(dir, e->path);
	bool ok = false;
	if (e->dir) {
		ok = make_dirs(p) || fail(GetLastError(), L"Can't make the folder %ls", p);
		free(p);
		return ok;
	}
	wchar_t *d = parent_of(p);
	if (!make_dirs(d)) {
		fail(GetLastError(), L"Can't make the folder %ls", d);
		free(d), free(p);
		return false;
	}
	free(d);
	HANDLE h = CreateFileW(p, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		fail(GetLastError(), L"Can't write %ls", p);
		free(p);
		return false;
	}
	uint32_t crc = 0;
	if (e->method == 0) {
		ok = e->csize == e->usize && write_all(h, e->data, e->csize);
		crc = crc32_of(0, e->data, e->csize);
		*done += e->usize;
		progress(*done, total);
		if (!ok) fail(GetLastError(), L"Can't write %ls", p);
	} else {
		// Inflated a piece at a time: what's made written out, the last
		// 32 KB kept for the matches that look back into it.
		enum { HIST = 32 << 10, CHUNK = 1 << 20 };
		size_t cap = HIST + CHUNK + AK_FL_OUT_SLACK;
		uint8_t *out = ak_alloc_raw(cap);
		ak_inflater *inf = ak_inflater_new();
		ak_fl_io io = {.in = e->data, .in_len = e->csize, .eof = true, .out = out, .cap = cap};
		size_t written = 0;
		uint64_t made = 0;
		ok = true;
		for (;;) {
			int r = ak_inflater_run(inf, &io);
			if (r == AK_FL_CORRUPT || r == AK_FL_NEED_INPUT) {
				ok = fail(0, L"The installer is damaged (%ls)", e->path);
				break;
			}
			size_t n = io.pos - written;
			if (n && !write_all(h, out + written, n)) {
				ok = fail(GetLastError(), L"Can't write %ls", p);
				break;
			}
			crc = crc32_of(crc, out + written, n);
			made += n, *done += n;
			progress(*done, total);
			written = io.pos;
			if (r == AK_FL_DONE) break;
			if (canceled) {
				ok = false;
				break;
			}
			// NEED_OUTPUT: keep the window's last 32 KB at the front.
			size_t keep = io.pos - io.start < HIST ? io.pos - io.start : HIST;
			memmove(out, out + io.pos - keep, keep);
			io.start = 0, io.pos = keep, written = keep;
		}
		ak_inflater_free(inf);
		free(out);
		if (ok && made != e->usize) ok = fail(0, L"The installer is damaged (%ls's size)", e->path);
	}
	CloseHandle(h);
	if (ok && crc != e->crc) ok = fail(0, L"The installer is damaged (%ls's checksum)", e->path);
	if (!ok) DeleteFileW(p);
	free(p);
	return ok;
}

// ---- A running copy of the app ----

typedef struct {
	DWORD *pids;
	size_t n;
} pid_list;

static bool in_dir(const wchar_t *path, const wchar_t *dir) {
	size_t n = wcslen(dir);
	return !_wcsnicmp(path, dir, n) && path[n] == L'\\';
}

// running are the processes (but this one, and the uninstaller) whose
// executable is in dir.
static pid_list running(const wchar_t *dir) {
	pid_list l = {0};
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
	if (snap == INVALID_HANDLE_VALUE) return l;
	PROCESSENTRY32W pe = {sizeof pe};
	size_t cap = 0;
	for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
		if (pe.th32ProcessID == GetCurrentProcessId() || !pe.th32ProcessID) continue;
		HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
		if (!p) continue;
		wchar_t img[MAX_PATH * 4];
		DWORD n = LEN(img);
		const wchar_t *base = NULL;
		// The uninstaller in the folder isn't the app: it's the one that
		// started this copy of it, waiting for it.
		if (QueryFullProcessImageNameW(p, 0, img, &n) && in_dir(img, dir) && (base = wcsrchr(img, L'\\')) && _wcsicmp(base + 1, C.uninstaller)) {
			if (l.n == cap) {
				cap = cap ? cap * 2 : 4;
				l.pids = ak_realloc(l.pids, cap * sizeof *l.pids);
			}
			l.pids[l.n++] = pe.th32ProcessID;
		}
		CloseHandle(p);
	}
	CloseHandle(snap);
	return l;
}

static BOOL CALLBACK close_window(HWND w, LPARAM pid) {
	DWORD owner = 0;
	GetWindowThreadProcessId(w, &owner);
	if (owner == (DWORD)pid && IsWindowVisible(w) && !GetWindow(w, GW_OWNER)) PostMessageW(w, WM_CLOSE, 0, 0);
	return TRUE;
}



static int ask(const wchar_t *instruction, const wchar_t *content, const wchar_t *yes, TASKDIALOG_COMMON_BUTTON_FLAGS common);

// may_close asks the user whether the app, open from dir, may be closed
// (unless silent, or it isn't open).
static bool may_close(const wchar_t *dir) {
	pid_list l = running(dir);
	free(l.pids);
	if (silent || !l.n) return true;
	wchar_t *what = wfmt(L"%ls is open", C.name);
	wchar_t *why = wdup(L"Setup closes it to go on: what it hasn't saved may be lost.");
	int r = ask(what, why, L"Close it", TDCBF_CANCEL_BUTTON);
	free(what), free(why);
	if (r == IDOK) return true;
	InterlockedExchange(&canceled, 1);
	return false;
}

// close_app closes the copies of the app running from dir: asked to
// (their windows closed, as the user would), then ended if they're still
// there after ten seconds.
static bool close_app(const wchar_t *dir) {
	pid_list l = running(dir);
	if (!l.n) return true;
	say(L"closing %zu running copies of the app", l.n);
	HANDLE *hs = xalloc(l.n * sizeof *hs);
	for (size_t i = 0; i < l.n; i++) {
		hs[i] = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, l.pids[i]);
		EnumWindows(close_window, (LPARAM)l.pids[i]);
	}
	bool ok = true;
	for (size_t i = 0; i < l.n; i++) {
		if (!hs[i]) continue;
		if (WaitForSingleObject(hs[i], 10000) != WAIT_OBJECT_0) {
			TerminateProcess(hs[i], 1);
			if (WaitForSingleObject(hs[i], 5000) != WAIT_OBJECT_0) ok = fail(0, L"%ls didn't close", C.name);
		}
		CloseHandle(hs[i]);
	}
	free(hs), free(l.pids);
	return ok;
}

// ---- The registry, the shell ----

static void reg_str(HKEY k, const wchar_t *name, const wchar_t *v) {
	RegSetValueExW(k, name, 0, REG_SZ, (const BYTE *)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
}

static void reg_dword(HKEY k, const wchar_t *name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&v, sizeof v); }

static wchar_t *reg_get(HKEY root, const wchar_t *key, const wchar_t *name) {
	DWORD size = 0;
	if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, NULL, &size) != ERROR_SUCCESS) return NULL;
	wchar_t *v = xalloc(size + sizeof(wchar_t));
	if (RegGetValueW(root, key, name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, NULL, v, &size) != ERROR_SUCCESS) {
		free(v);
		return NULL;
	}
	return v;
}

static uint64_t dir_size(const wchar_t *dir) {
	uint64_t n = 0;
	wchar_t *pat = wjoin(dir, L"*");
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pat, &fd);
	free(pat);
	if (h == INVALID_HANDLE_VALUE) return 0;
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
			wchar_t *c = wjoin(dir, fd.cFileName);
			n += dir_size(c);
			free(c);
		} else n += (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow;
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	return n;
}

// register_app lists the app in Installed apps: the Uninstall key of the
// user's.
static bool register_app(const wchar_t *dir) {
	wchar_t *key = wcat(UNINSTALL_KEY, C.id, (wchar_t *)NULL);
	HKEY k;
	LSTATUS s = RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL);
	free(key);
	if (s != ERROR_SUCCESS) return fail((DWORD)s, L"Can't list the app in Installed apps");
	wchar_t *exe = wjoin(dir, C.exe), *un = wjoin(dir, C.uninstaller);
	wchar_t *icon = wfmt(L"\"%ls\",0", exe), *cmd = wfmt(L"\"%ls\"", un), *quiet = wfmt(L"\"%ls\" /S", un);
	SYSTEMTIME t;
	GetLocalTime(&t);
	wchar_t date[16];
	_snwprintf(date, LEN(date), L"%04u%02u%02u", t.wYear, t.wMonth, t.wDay);
	reg_str(k, L"DisplayName", C.name);
	reg_str(k, L"DisplayVersion", C.version);
	if (*C.publisher) reg_str(k, L"Publisher", C.publisher);
	reg_str(k, L"DisplayIcon", icon);
	reg_str(k, L"InstallLocation", dir);
	reg_str(k, L"UninstallString", cmd);
	reg_str(k, L"QuietUninstallString", quiet);
	reg_str(k, L"InstallDate", date);
	if (*C.homepage) reg_str(k, L"URLInfoAbout", C.homepage);
	reg_dword(k, L"NoModify", 1);
	reg_dword(k, L"NoRepair", 1);
	reg_dword(k, L"EstimatedSize", (DWORD)(dir_size(dir) / 1024));
	RegCloseKey(k);
	free(exe), free(un), free(icon), free(cmd), free(quiet);
	return true;
}

static wchar_t *known_folder(REFKNOWNFOLDERID id) {
	PWSTR p = NULL;
	if (FAILED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, NULL, &p))) return NULL;
	wchar_t *o = wdup(p);
	CoTaskMemFree(p);
	return o;
}

static wchar_t *shortcut_path(REFKNOWNFOLDERID where) {
	wchar_t *d = known_folder(where);
	if (!d) return NULL;
	wchar_t *name = wcat(C.name, L".lnk", (wchar_t *)NULL);
	for (wchar_t *c = name; *c; c++)
		if (wcschr(L"\\/:*?\"<>|", *c)) *c = L'_';
	wchar_t *p = wjoin(d, name);
	free(d), free(name);
	return p;
}

// make_shortcut makes a shortcut to the app at path, with its
// AppUserModelID (the Start menu's: what the app's notifications are
// shown as from).
static bool make_shortcut(const wchar_t *path, const wchar_t *dir) {
	IShellLinkW *link = NULL;
	if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return false;
	wchar_t *exe = wjoin(dir, C.exe);
	IShellLinkW_SetPath(link, exe);
	IShellLinkW_SetWorkingDirectory(link, dir);
	IShellLinkW_SetIconLocation(link, exe, 0);
	IPropertyStore *ps = NULL;
	if (*C.aumid && SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPropertyStore, (void **)&ps))) {
		PROPVARIANT v;
		PropVariantInit(&v);
		v.vt = VT_LPWSTR, v.pwszVal = C.aumid;
		IPropertyStore_SetValue(ps, &PKEY_AUMID, &v);
		CLSID clsid;
		if (*C.activator && SUCCEEDED(CLSIDFromString(C.activator, &clsid))) {
			// The toasts' activator's, as the app's own shortcut has it
			// (platform/notify), so the app doesn't remake it.
			v.vt = VT_CLSID, v.puuid = &clsid;
			IPropertyStore_SetValue(ps, &PKEY_ToastActivator, &v);
		}
		IPropertyStore_Commit(ps);
		IPropertyStore_Release(ps);
	}
	IPersistFile *pf = NULL;
	bool ok = SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf)) && SUCCEEDED(IPersistFile_Save(pf, path, TRUE));
	if (pf) IPersistFile_Release(pf);
	IShellLinkW_Release(link);
	free(exe);
	return ok;
}

// remove_shortcut removes a shortcut at path if it's to the app in dir.
static void remove_shortcut(const wchar_t *path, const wchar_t *dir) {
	if (!path || !exists(path)) return;
	IShellLinkW *link = NULL;
	if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link))) return;
	IPersistFile *pf = NULL;
	bool ours = false;
	if (SUCCEEDED(IShellLinkW_QueryInterface(link, &IID_IPersistFile, (void **)&pf)) && SUCCEEDED(IPersistFile_Load(pf, path, STGM_READ))) {
		wchar_t target[MAX_PATH * 4];
		ours = SUCCEEDED(IShellLinkW_GetPath(link, target, LEN(target), NULL, SLGP_RAWPATH)) && in_dir(target, dir);
	}
	if (pf) IPersistFile_Release(pf);
	IShellLinkW_Release(link);
	if (ours) DeleteFileW(path);
}

// ---- The dialog ----

// app_icon is the dialogs' icon: the app's (the group icon 1), else none.
static PCWSTR app_icon(void) {
	return FindResourceW(NULL, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(14)) ? MAKEINTRESOURCEW(1) : NULL;  // RT_GROUP_ICON
}

static int ask(const wchar_t *instruction, const wchar_t *content, const wchar_t *yes, TASKDIALOG_COMMON_BUTTON_FLAGS common) {
	TASKDIALOG_BUTTON b = {IDOK, yes};
	TASKDIALOGCONFIG tc = {sizeof tc};
	tc.hInstance = GetModuleHandleW(NULL);
	tc.dwFlags = TDF_POSITION_RELATIVE_TO_WINDOW | TDF_SIZE_TO_CONTENT;
	tc.pszWindowTitle = C.name;
	tc.pszMainIcon = app_icon();
	tc.pszMainInstruction = instruction;
	tc.pszContent = content;
	tc.cButtons = 1, tc.pButtons = &b;
	tc.dwCommonButtons = common;
	int r = IDCANCEL;
	TaskDialogIndirect(&tc, &r, NULL, NULL);
	return r;
}

static void show_error(const wchar_t *instruction) {
	if (silent) return;
	TASKDIALOGCONFIG tc = {sizeof tc};
	tc.hInstance = GetModuleHandleW(NULL);
	tc.dwFlags = TDF_SIZE_TO_CONTENT;
	tc.pszWindowTitle = C.name;
	tc.pszMainIcon = TD_ERROR_ICON;
	tc.pszMainInstruction = instruction;
	tc.pszContent = *failure ? failure : L"Something went wrong.";
	tc.dwCommonButtons = TDCBF_CLOSE_BUTTON;
	TaskDialogIndirect(&tc, NULL, NULL, NULL);
}

typedef bool (*job_fn)(void *);

typedef struct {
	job_fn fn;
	void *arg;
	bool ok;
	volatile LONG done;
	HANDLE thread;
} job;

static DWORD WINAPI job_thread(LPVOID p) {
	job *j = p;
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	j->ok = j->fn(j->arg);
	CoUninitialize();
	InterlockedExchange(&j->done, 1);
	if (dialog) PostMessageW(dialog, TDM_CLICK_BUTTON, IDCANCEL, 0);
	return 0;
}

static HRESULT CALLBACK job_dialog(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LONG_PTR ref) {
	(void)lp;
	job *j = (job *)ref;
	switch (msg) {
	case TDN_CREATED:
		dialog = hwnd;
		SendMessageW(hwnd, TDM_SET_PROGRESS_BAR_RANGE, 0, MAKELPARAM(0, 1000));
		j->thread = CreateThread(NULL, 0, job_thread, j, 0, NULL);
		if (!j->thread) {
			j->ok = fail(GetLastError(), L"Can't start");
			j->done = 1;
			return S_OK;
		}
		break;
	case TDN_BUTTON_CLICKED:
		if (j->done) return S_OK;  // the job's end (it clicks Cancel)
		if (wp == IDCANCEL) {
			// The user's: the job stops at its next step.
			InterlockedExchange(&canceled, 1);
			SendMessageW(hwnd, TDM_ENABLE_BUTTON, IDCANCEL, FALSE);
		}
		return S_FALSE;
	case TDN_DESTROYED:
		dialog = NULL;
		break;
	}
	return S_OK;
}

// run runs fn: with a progress dialog that says what, on a thread of
// its own; silently, here.
static bool run(job_fn fn, void *arg, const wchar_t *what, const wchar_t *detail) {
	if (silent) return fn(arg);
	job j = {fn, arg, false, 0, NULL};
	TASKDIALOGCONFIG tc = {sizeof tc};
	tc.hInstance = GetModuleHandleW(NULL);
	tc.dwFlags = TDF_SHOW_PROGRESS_BAR | TDF_SIZE_TO_CONTENT;
	tc.pszWindowTitle = C.name;
	tc.pszMainIcon = app_icon();
	tc.pszMainInstruction = what;
	tc.pszContent = detail;
	tc.dwCommonButtons = TDCBF_CANCEL_BUTTON;
	tc.pfCallback = job_dialog;
	tc.lpCallbackData = (LONG_PTR)&j;
	HRESULT hr = TaskDialogIndirect(&tc, NULL, NULL, NULL);
	if (j.thread) {
		WaitForSingleObject(j.thread, INFINITE);
		CloseHandle(j.thread);
	} else if (FAILED(hr)) {
		// No dialog (no common controls 6?): the job, here.
		return fn(arg);
	}
	return j.ok;
}

// ---- Installing ----

typedef struct {
	wchar_t *dir;  // where the app goes
	archive a;
	bool update;  // it was installed there before
} install;

// unique is a name beside dir's entries for an entry put aside, which
// the app removes as it starts (wh_update_clean's .*.old-*).
static wchar_t *aside_name(const wchar_t *dir, const wchar_t *unit) {
	return wfmt(L"%ls\\.%ls.old-%lu", dir, unit, GetCurrentProcessId());
}

static bool do_install(void *p) {
	install *in = p;
	say(L"installing %ls %ls into %ls", C.name, C.version, in->dir);
	if (!close_app(in->dir)) return false;
	if (canceled) return false;
	wchar_t *parent = parent_of(in->dir);
	if (!make_dirs(parent)) {
		fail(GetLastError(), L"Can't make the folder %ls", parent);
		free(parent);
		return false;
	}
	// Unpacked beside the install, on its drive: then moved in.
	wchar_t *stage = wfmt(L"%ls\\.%ls.asuka-install-%lu", parent, C.folder, GetCurrentProcessId());
	free(parent);
	remove_all(stage);
	bool ok = make_dirs(stage) || fail(GetLastError(), L"Can't make the folder %ls", stage);
	uint64_t done = 0;
	for (size_t i = 0; ok && i < in->a.n; i++) {
		if (canceled) ok = false;
		else ok = extract(&in->a.v[i], stage, &done, in->a.total);
	}
	if (!ok) {
		remove_all(stage);
		free(stage);
		return false;
	}
	// The payload's units: its top-level entries.
	names units = {0};
	for (size_t i = 0; i < in->a.n; i++) {
		wchar_t *u = wdup(in->a.v[i].path);
		wchar_t *s = wcschr(u, L'\\');
		if (s) *s = 0;
		if (!names_has(&units, u)) names_add(&units, u);
		else free(u);
	}
	// From here the install changes: what the version before installed
	// and this one lacks, put aside; each unit's old self put aside,
	// the new moved in. The asides go when the app starts, or now.
	ok = make_dirs(in->dir) || fail(GetLastError(), L"Can't make the folder %ls", in->dir);
	names old = read_list(in->dir), asides = {0};
	for (size_t i = 0; ok && i < old.n; i++) {
		if (names_has(&units, old.v[i])) continue;
		wchar_t *from = wjoin(in->dir, old.v[i]);
		if (exists(from)) {
			wchar_t *to = aside_name(in->dir, old.v[i]);
			remove_all(to);
			if (MoveFileExW(from, to, 0)) names_add(&asides, to);
			else free(to);
		}
		free(from);
	}
	for (size_t i = 0; ok && i < units.n; i++) {
		wchar_t *from = wjoin(stage, units.v[i]), *to = wjoin(in->dir, units.v[i]);
		if (exists(to)) {
			wchar_t *aside = aside_name(in->dir, units.v[i]);
			remove_all(aside);
			if (MoveFileExW(to, aside, 0)) names_add(&asides, aside);
			else {
				ok = fail(GetLastError(), L"Can't replace %ls", to);
				free(aside);
			}
		}
		if (ok && !MoveFileExW(from, to, MOVEFILE_COPY_ALLOWED)) ok = fail(GetLastError(), L"Can't move %ls into place", units.v[i]);
		free(from), free(to);
	}
	for (size_t i = 0; i < asides.n; i++) remove_all(asides.v[i]);
	names_free(&asides);
	remove_all(stage);
	free(stage);
	if (ok) {
		// The list the updater keeps: what this version installed, the
		// uninstaller not in it (an update would take it away).
		names list = {0};
		for (size_t i = 0; i < units.n; i++)
			if (_wcsicmp(units.v[i], FILE_LIST) && _wcsicmp(units.v[i], C.uninstaller)) names_add(&list, wdup(units.v[i]));
		if (!write_list(in->dir, &list)) ok = fail(GetLastError(), L"Can't write the app's file list");
		names_free(&list);
	}
	names_free(&units);
	names_free(&old);
	if (!ok) return false;
	if (!register_app(in->dir)) return false;
	wchar_t *start = shortcut_path(&FOLDERID_Programs);
	if (!start || !make_shortcut(start, in->dir)) say(L"no Start menu shortcut");
	free(start);
	if (C.desktop && !in->update) {
		wchar_t *desk = shortcut_path(&FOLDERID_Desktop);
		if (desk) make_shortcut(desk, in->dir);
		free(desk);
	}
	SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW, in->dir, NULL);
	say(L"installed");
	return true;
}

static void open_app(const wchar_t *dir) {
	wchar_t *exe = wjoin(dir, C.exe), *cmd = wfmt(L"\"%ls\"", exe);
	STARTUPINFOW si = {sizeof si};
	PROCESS_INFORMATION pi;
	if (CreateProcessW(exe, cmd, NULL, NULL, FALSE, 0, NULL, dir, &si, &pi)) {
		AllowSetForegroundWindow(pi.dwProcessId);
		CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
	}
	free(exe), free(cmd);
}

// default_dir is where the app goes: where it was installed before (the
// Uninstall key's InstallLocation), else the user's Programs folder
// (%LOCALAPPDATA%\Programs).
static wchar_t *default_dir(bool *update) {
	wchar_t *key = wcat(UNINSTALL_KEY, C.id, (wchar_t *)NULL);
	wchar_t *was = reg_get(HKEY_CURRENT_USER, key, L"InstallLocation");
	free(key);
	if (was && *was && is_dir(was)) {
		*update = true;
		return was;
	}
	free(was);
	wchar_t *programs = known_folder(&FOLDERID_UserProgramFiles);
	if (!programs) {
		wchar_t local[MAX_PATH * 2];
		if (!GetEnvironmentVariableW(L"LOCALAPPDATA", local, LEN(local))) return NULL;
		programs = wjoin(local, L"Programs");
	}
	wchar_t *d = wjoin(programs, C.folder);
	free(programs);
	*update = exists(d);
	return d;
}

static wchar_t *full_path(const wchar_t *p) {
	DWORD n = GetFullPathNameW(p, 0, NULL, NULL);
	if (!n) return wdup(p);
	wchar_t *o = xalloc(((size_t)n + 1) * sizeof *o);
	GetFullPathNameW(p, n + 1, o, NULL);
	size_t l = wcslen(o);
	while (l > 3 && o[l - 1] == L'\\') o[--l] = 0;
	return o;
}

static int installer(const uint8_t *z, DWORD n, const wchar_t *dir) {
	install in = {0};
	if (!read_archive(z, n, &in.a)) {
		show_error(L"The installer can't be read");
		return 1;
	}
	in.dir = dir ? full_path(dir) : default_dir(&in.update);
	if (!in.dir) {
		fail(0, L"No folder to install into");
		show_error(L"The app couldn't be installed");
		return 1;
	}
	if (dir) in.update = exists(in.dir);
	if (!may_close(in.dir)) return 2;
	wchar_t *what = wfmt(L"Installing %ls", C.name);
	wchar_t *detail = wfmt(L"Version %ls, into %ls", C.version, in.dir);
	bool ok = run(do_install, &in, what, detail);
	free(what), free(detail);
	if (!ok) {
		if (canceled) {
			say(L"canceled");
			return 2;
		}
		show_error(L"The app couldn't be installed");
		return 1;
	}
	if (!silent) open_app(in.dir);
	return 0;
}

// ---- Uninstalling ----

// unregister removes what the app registered for the user, as it runs
// (instance.c's URL scheme and file types, notify.c's toasts), where it's
// the app in dir's, and its Installed apps' entry.
static void unregister(const wchar_t *dir) {
	wchar_t *key;
	if (*C.scheme) {
		key = wfmt(L"Software\\Classes\\%ls\\shell\\open\\command", C.scheme);
		wchar_t *cmd = reg_get(HKEY_CURRENT_USER, key, NULL);
		free(key);
		wchar_t *exe = wjoin(dir, C.exe);
		if (cmd && wcsstr(cmd, exe)) {
			key = wfmt(L"Software\\Classes\\%ls", C.scheme);
			RegDeleteTreeW(HKEY_CURRENT_USER, key);
			free(key);
		}
		free(cmd), free(exe);
	}
	// The file types' ProgIDs, and the extensions' mentions of them.
	names ids = split(C.progids), exts = split(C.exts);
	for (size_t i = 0; i < ids.n; i++) {
		key = wfmt(L"Software\\Classes\\%ls", ids.v[i]);
		RegDeleteTreeW(HKEY_CURRENT_USER, key);
		free(key);
		for (size_t j = 0; j < exts.n; j++) {
			key = wfmt(L"Software\\Classes\\%ls\\OpenWithProgids", exts.v[j]);
			RegDeleteKeyValueW(HKEY_CURRENT_USER, key, ids.v[i]);
			free(key);
			key = wfmt(L"Software\\Classes\\%ls", exts.v[j]);
			wchar_t *def = reg_get(HKEY_CURRENT_USER, key, NULL);
			if (def && !_wcsicmp(def, ids.v[i])) RegDeleteKeyValueW(HKEY_CURRENT_USER, key, NULL);
			free(def), free(key);
		}
	}
	names_free(&ids), names_free(&exts);
	key = wfmt(L"Software\\Classes\\Applications\\%ls", C.exe);
	RegDeleteTreeW(HKEY_CURRENT_USER, key);
	free(key);
	if (*C.aumid) {
		key = wfmt(L"Software\\Classes\\AppUserModelId\\%ls", C.aumid);
		RegDeleteTreeW(HKEY_CURRENT_USER, key);
		free(key);
	}
	if (*C.activator) {
		key = wfmt(L"Software\\Classes\\CLSID\\%ls\\LocalServer32", C.activator);
		wchar_t *cmd = reg_get(HKEY_CURRENT_USER, key, NULL);
		free(key);
		wchar_t *exe = wjoin(dir, C.exe);
		if (cmd && wcsstr(cmd, exe)) {
			key = wfmt(L"Software\\Classes\\CLSID\\%ls", C.activator);
			RegDeleteTreeW(HKEY_CURRENT_USER, key);
			free(key);
		}
		free(cmd), free(exe);
	}
	key = wcat(UNINSTALL_KEY, C.id, (wchar_t *)NULL);
	RegDeleteTreeW(HKEY_CURRENT_USER, key);
	free(key);
	SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, NULL, NULL);
}

typedef struct {
	wchar_t *dir;
	wchar_t *self;  // the uninstaller in the folder, to remove too
} uninstall;

static bool do_uninstall(void *p) {
	uninstall *u = p;
	say(L"uninstalling %ls from %ls", C.name, u->dir);
	if (!close_app(u->dir)) return false;
	names list = read_list(u->dir);
	if (!list.n) names_add(&list, wdup(C.exe));
	uint64_t done = 0;
	bool ok = true;
	for (size_t i = 0; i < list.n; i++) {
		wchar_t *p = wjoin(u->dir, list.v[i]);
		if (!remove_all(p) && exists(p)) ok = fail(GetLastError(), L"Can't remove %ls", p);
		free(p);
		progress(++done, list.n + 1);
	}
	names_free(&list);
	// What the updater and the installer left: the list, entries put
	// aside, an update's work folders.
	const wchar_t *pats[] = {FILE_LIST, L".*.old-*", L".asuka-update-*", FILE_LIST L".tmp"};
	for (size_t i = 0; i < LEN(pats); i++) {
		wchar_t *pat = wjoin(u->dir, pats[i]);
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW(pat, &fd);
		free(pat);
		if (h == INVALID_HANDLE_VALUE) continue;
		do {
			wchar_t *c = wjoin(u->dir, fd.cFileName);
			remove_all(c);
			free(c);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	if (u->self) DeleteFileW(u->self);
	RemoveDirectoryW(u->dir);  // unless something of the user's is left in it
	wchar_t *start = shortcut_path(&FOLDERID_Programs), *desk = shortcut_path(&FOLDERID_Desktop);
	remove_shortcut(start, u->dir);
	remove_shortcut(desk, u->dir);
	free(start), free(desk);
	unregister(u->dir);
	progress(1, 1);
	say(ok ? L"uninstalled" : L"uninstalled, but for some files");
	return ok;
}

// The uninstaller can't remove itself while it runs: run from the folder,
// it starts a copy of itself in the temporary folder, which does it, and
// waits for that copy to say it's done (so a caller that waits for the
// uninstaller waits for the uninstall), then ends; the copy removes the
// one in the folder once it has, and itself after it ends (cmd, a moment
// later).
static int relaunch_from_temp(const wchar_t *self, const wchar_t *dir) {
	wchar_t tmp[MAX_PATH + 1];
	if (!GetTempPathW(LEN(tmp), tmp)) return -1;
	wchar_t *copy = wfmt(L"%lsasuka-uninstall-%lu.exe", tmp, GetCurrentProcessId());
	if (!CopyFileW(self, copy, FALSE)) {
		free(copy);
		return -1;
	}
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	HANDLE done = CreateEventW(&sa, TRUE, FALSE, NULL);
	HANDLE me = NULL;
	DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(), GetCurrentProcess(), &me, SYNCHRONIZE, TRUE, 0);
	wchar_t *cmd = wfmt(L"\"%ls\" %ls--from=\"%ls\" --done=%llu --parent=%llu%ls%ls%ls", copy, silent ? L"/S " : L"", dir,
		(unsigned long long)(uintptr_t)done, (unsigned long long)(uintptr_t)me, log_path ? L" \"/LOG=" : L"", log_path ? log_path : L"",
		log_path ? L"\"" : L"");
	STARTUPINFOW si = {sizeof si};
	PROCESS_INFORMATION pi;
	BOOL started = CreateProcessW(copy, cmd, NULL, NULL, TRUE, 0, NULL, tmp, &si, &pi);
	free(cmd);
	if (!started) {
		DeleteFileW(copy);
		free(copy);
		CloseHandle(done);
		if (me) CloseHandle(me);
		return -1;
	}
	free(copy);
	// Until the copy says it's done (it uninstalled the app), or ends
	// without saying so (it didn't: its exit code says why).
	HANDLE both[2] = {done, pi.hProcess};
	DWORD w = WaitForMultipleObjects(2, both, FALSE, INFINITE);
	DWORD code = 0;
	if (w != WAIT_OBJECT_0 && !GetExitCodeProcess(pi.hProcess, &code)) code = 1;
	CloseHandle(pi.hThread), CloseHandle(pi.hProcess), CloseHandle(done);
	if (me) CloseHandle(me);
	return (int)code;
}

static void remove_later(const wchar_t *self) {
	// cmd's del, once this process has ended (ping: a moment's wait).
	wchar_t sys[MAX_PATH];
	GetSystemDirectoryW(sys, LEN(sys));
	wchar_t *app = wfmt(L"%ls\\cmd.exe", sys);
	wchar_t *cmd = wfmt(L"\"%ls\" /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"%ls\"", app, self);
	STARTUPINFOW si = {sizeof si};
	PROCESS_INFORMATION pi;
	if (CreateProcessW(app, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, NULL, NULL, &si, &pi))
		CloseHandle(pi.hThread), CloseHandle(pi.hProcess);
	free(app), free(cmd);
}

static int uninstaller(const wchar_t *self, const wchar_t *from, HANDLE done, HANDLE parent) {
	uninstall u = {0};
	if (!from) {
		// Run from the folder: confirm, then hand over to a copy.
		wchar_t *dir = parent_of(self);
		if (!silent) {
			wchar_t *what = wfmt(L"Uninstall %ls?", C.name);
			wchar_t *why = wfmt(L"%ls and its files in %ls are removed. What you made with it, and its settings, stay.", C.name, dir);
			int r = ask(what, why, L"Uninstall", TDCBF_CANCEL_BUTTON);
			free(what), free(why);
			if (r != IDOK) return 2;
		}
		int code = relaunch_from_temp(self, dir);
		if (code >= 0) return code;
		// No copy: here, leaving the uninstaller.
		u.dir = dir;
	} else {
		u.dir = full_path(from);
		u.self = wjoin(u.dir, C.uninstaller);
	}
	wchar_t *what = wfmt(L"Uninstalling %ls", C.name);
	bool ok = run(do_uninstall, &u, what, u.dir);
	free(what);
	if (ok && done) {
		// The one in the folder ends now: then it's removed.
		SetEvent(done);
		if (parent && WaitForSingleObject(parent, 10000) == WAIT_OBJECT_0 && u.self) {
			// Its image lets go of the file a moment after it ends.
			for (int i = 0; i < 50 && exists(u.self) && !DeleteFileW(u.self); i++) Sleep(100);
			RemoveDirectoryW(u.dir);
		}
	}
	if (from) remove_later(self);
	if (!ok && !canceled) show_error(L"The app couldn't be uninstalled");
	return ok ? 0 : canceled ? 2 : 1;
}

// ---- Arguments ----

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdline, int show) {
	(void)inst, (void)prev, (void)cmdline, (void)show;
	SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_PROGRESS_CLASS};
	InitCommonControlsEx(&icc);
	if (!read_config()) {
		MessageBoxW(NULL, L"The installer is damaged (it says nothing of the app).", L"Setup", MB_ICONERROR);
		return 1;
	}
	int argc = 0;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	const wchar_t *dir = NULL, *from = NULL;
	HANDLE done = NULL, parent = NULL;
	for (int i = 1; argv && i < argc; i++) {
		const wchar_t *a = argv[i];
		if (!_wcsicmp(a, L"/S") || !_wcsicmp(a, L"--silent")) silent = true;
		else if (!_wcsnicmp(a, L"/D=", 3)) dir = a + 3;
		else if (!_wcsnicmp(a, L"/LOG=", 5)) log_path = wdup(a + 5);
		else if (!wcsncmp(a, L"--from=", 7)) from = a + 7;
		else if (!wcsncmp(a, L"--done=", 7)) done = (HANDLE)(uintptr_t)_wcstoui64(a + 7, NULL, 10);
		else if (!wcsncmp(a, L"--parent=", 9)) parent = (HANDLE)(uintptr_t)_wcstoui64(a + 9, NULL, 10);
	}
	if (!dir) {
		// /D= is the last argument and may have spaces, unquoted (NSIS's).
		const wchar_t *d = wcsstr(GetCommandLineW(), L" /D=");
		if (d) dir = d + 4;
	}
	wchar_t self[MAX_PATH * 4];
	GetModuleFileNameW(NULL, self, LEN(self));
	DWORD n = 0;
	const uint8_t *z = resource(RES_PAYLOAD, &n);
	int code = z ? installer(z, n, dir) : uninstaller(self, from, done, parent);
	CoUninitialize();
	return code;
}
