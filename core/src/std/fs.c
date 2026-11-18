// fs.c: std/fs's builtins: paths, the directories files go in, and the
// file system's calls (POSIX, Win32 in UTF-16, WASI's where the web gives
// it a directory). The calls block, so apps make them in spawn { }; each
// is safe on any thread.
//
// A call that fails returns its failure value (none, false) and leaves
// why in this thread's last error, which fs.failureKind() gives (a
// FAIL_*) and fs.failureReason() says; the Asuka side throws an FsError
// of it (std/fs/fs.asuka).
//
//	fs.appDir() / home() / tempDir() -> string
//	fs.join(parts) / clean(p) / dir(p) / base(p) / ext(p) -> string
//	fs.absolute(p) -> string?        against the working directory, cleaned
//	fs.listNames(p) -> [string]?     names in a directory, sorted
//	fs.statInfo(p) -> [int]?         [isDir, size, modified (ns), link, mode]
//	fs.readFile(p) -> bytes?
//	fs.writeFile(p, data, append) -> bool
//	fs.copyFile(from, to) / moveFile(from, to) -> bool
//	fs.makeDirs(p) / removeOne(p) / removeTree(p) -> bool
//	fs.exists(p) -> bool
//	fs.walkInfo(p) -> [int]?         statInfo's
//	fs.matches(pattern, p) -> bool   a glob's pattern
//	fs.chmodPath(p, mode) / symlinkPath(to, at) -> bool, readLinkPath(p) -> string?
//	fs.file*: open files (below)
//	fs.failureKind() -> int / failureReason() -> string
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE  // st_mtimespec, getprogname
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE  // DT_DIR
#endif
#include "std/std.h"
#include "base/thread.h"

#include <stdio.h>
#include <stdlib.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__ANDROID__) || defined(__wasm__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define AK_FS_SANDBOX 1  // a phone's app or the web: no user's home to see
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#if !defined(AK_FS_SANDBOX)
#include <pwd.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STR(i) ((ak_str *)args[i].p)
#define BYTES(i) ((ak_bytes *)args[i].p)

#if defined(_WIN32)
#define SEP '\\'
static bool is_sep(char c) { return c == '/' || c == '\\'; }
#else
#define SEP '/'
static bool is_sep(char c) { return c == '/'; }
#endif

// ---- Failures ----

enum { FAIL_OTHER = 1, FAIL_NOT_FOUND, FAIL_EXISTS, FAIL_DENIED, FAIL_NOT_DIR, FAIL_IS_DIR, FAIL_NOT_EMPTY };

static AK_TLS int fail_kind;
static AK_TLS char fail_reason[200];

static void failed(int kind, const char *reason) {
	fail_kind = kind;
	snprintf(fail_reason, sizeof fail_reason, "%s", reason);
}

#if defined(_WIN32)
static void failed_os(void) {
	DWORD e = GetLastError();
	int kind;
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
		kind = FAIL_NOT_FOUND;
		break;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		kind = FAIL_EXISTS;
		break;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
		kind = FAIL_DENIED;
		break;
	case ERROR_DIRECTORY:
		kind = FAIL_NOT_DIR;
		break;
	case ERROR_DIR_NOT_EMPTY:
		kind = FAIL_NOT_EMPTY;
		break;
	default:
		kind = FAIL_OTHER;
	}
	char msg[160] = "";
	DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof msg, NULL);
	while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == '.')) msg[--n] = 0;
	if (n == 0) snprintf(msg, sizeof msg, "error %lu", (unsigned long)e);
	failed(kind, msg);
}
#else
static void failed_os(void) {
	int e = errno;
	int kind;
	switch (e) {
	case ENOENT: kind = FAIL_NOT_FOUND; break;
	case EEXIST: kind = FAIL_EXISTS; break;
	case EACCES:
	case EPERM: kind = FAIL_DENIED; break;
	case ENOTDIR: kind = FAIL_NOT_DIR; break;
	case EISDIR: kind = FAIL_IS_DIR; break;
	case ENOTEMPTY: kind = FAIL_NOT_EMPTY; break;
	default: kind = FAIL_OTHER;
	}
	failed(kind, strerror(e));
}
#endif

static ak_val failure(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.i = fail_kind ? fail_kind : FAIL_OTHER};
}

static ak_val failure_reason(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(fail_reason)};
}

// ---- Paths ----

// clean is p's shortest equivalent (Go's filepath.Clean): one separator
// between names, no "." names, ".." names folded into the name before
// them, "." for an empty path.
static ak_str *clean(const char *p, size_t n) {
	ak_buf b = {0};
	size_t i = 0;
	size_t root = 0;  // what ".." never goes above
#if defined(_WIN32)
	if (n >= 2 && p[1] == ':') {  // a drive
		ak_buf_put(&b, p, 2);
		i = 2;
	} else if (n >= 2 && is_sep(p[0]) && is_sep(p[1])) {  // \\server\share
		ak_buf_put(&b, "\\\\", 2);
		i = 2;
		for (int part = 0; part < 2; part++) {
			size_t j = i;
			while (j < n && !is_sep(p[j])) j++;
			ak_buf_put(&b, p + i, j - i);
			i = j;
			if (part == 0 && i < n) ak_buf_u8(&b, SEP), i++;
		}
	}
#endif
	size_t vol = b.len;
	bool abs = i < n && is_sep(p[i]);
	if (abs) {
		ak_buf_u8(&b, SEP);
		i++;
	}
	root = b.len;
	size_t names = 0;  // names written after root that ".." can fold
	while (i < n) {
		if (is_sep(p[i])) {
			i++;
			continue;
		}
		size_t j = i;
		while (j < n && !is_sep(p[j])) j++;
		size_t len = j - i;
		if (len == 1 && p[i] == '.') {
			// nothing
		} else if (len == 2 && p[i] == '.' && p[i + 1] == '.') {
			if (names > 0) {
				while (b.len > root && b.data[b.len - 1] != SEP) b.len--;
				if (b.len > root) b.len--;
				names--;
			} else if (!abs) {
				if (b.len > vol && b.data[b.len - 1] != SEP) ak_buf_u8(&b, SEP);
				ak_buf_put(&b, "..", 2);
				root = b.len;  // ".." can't fold a ".."
			}
		} else {
			if (b.len > vol && b.data[b.len - 1] != SEP) ak_buf_u8(&b, SEP);
			ak_buf_put(&b, p + i, len);
			names++;
		}
		i = j;
	}
	if (b.len == 0) ak_buf_u8(&b, '.');
	ak_str *s = ak_str_new((const char *)b.data, b.len);
	ak_buf_free(&b);
	return s;
}

static ak_val clean_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.p = clean(STR(0)->data, STR(0)->len)};
}

static ak_val join_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *parts = args[0].p;
	ak_buf b = {0};
	for (uint32_t k = 0; k < parts->len; k++) {
		ak_str *s = parts->data[k].p;
		if (!s || s->len == 0) continue;
		if (b.len) ak_buf_u8(&b, SEP);
		ak_buf_put(&b, s->data, s->len);
	}
	ak_str *r = b.len ? clean((const char *)b.data, b.len) : ak_str_new("", 0);
	ak_buf_free(&b);
	return (ak_val){.p = r};
}

// volume is the length of p's drive or share (Windows), 0 elsewhere.
static size_t volume(const char *p, size_t n) {
#if defined(_WIN32)
	if (n >= 2 && p[1] == ':') return 2;
	if (n >= 2 && is_sep(p[0]) && is_sep(p[1])) {
		size_t i = 2;
		for (int part = 0; part < 2; part++) {
			while (i < n && !is_sep(p[i])) i++;
			if (part == 0 && i < n) i++;
		}
		return i;
	}
#else
	AK_UNUSED(p), AK_UNUSED(n);
#endif
	return 0;
}

// dir: all but p's last name, cleaned ("." when there's none).
static ak_val dir_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *s = STR(0);
	size_t v = volume(s->data, s->len), i = s->len;
	while (i > v && !is_sep(s->data[i - 1])) i--;
	ak_str *d = clean(s->data + v, i - v);
	if (v) {
		ak_str *r = ak_str_new(NULL, v + d->len);
		memcpy(r->data, s->data, v);
		memcpy(r->data + v, d->data, d->len);
		ak_release(d);
		d = r;
	}
	return (ak_val){.p = d};
}

// base: p's last name ("." for an empty path, the separator for a root).
static ak_val base_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *s = STR(0);
	size_t v = volume(s->data, s->len), end = s->len;
	while (end > v && is_sep(s->data[end - 1])) end--;
	if (end == v) {
		if (s->len > v) return (ak_val){.p = ak_str_new((char[]){SEP}, 1)};
		return (ak_val){.p = ak_str_new(".", 1)};
	}
	size_t i = end;
	while (i > v && !is_sep(s->data[i - 1])) i--;
	return (ak_val){.p = ak_str_new(s->data + i, end - i)};
}

// ext: the last name's extension from its last '.', "" when none.
static ak_val ext_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *s = STR(0);
	for (size_t i = s->len; i > 0; i--) {
		char c = s->data[i - 1];
		if (is_sep(c)) break;
		if (c == '.') return (ak_val){.p = ak_str_new(s->data + i - 1, s->len - i + 1)};
	}
	return (ak_val){.p = ak_str_new("", 0)};
}

// ---- The app's directory ----

static char *app_dir;

void ak_fs_set_app_dir(const char *path) {
	char *old = app_dir;
	app_dir = path && *path ? strcpy(ak_alloc(strlen(path) + 1), path) : NULL;
	ak_free(old);
}

static char *dup_path(const char *s) {
	size_t n = strlen(s);
	char *p = ak_alloc(n + 1);
	memcpy(p, s, n + 1);
	return p;
}

char *ak_fs_app_dir(void) {
	if (app_dir) return dup_path(app_dir);
	const char *env = getenv("ASUKA_DATA");
	if (env && *env) return dup_path(env);
#if !defined(__wasm__)
	char buf[1024];
#endif
#if defined(_WIN32)
	const char *base = getenv("LOCALAPPDATA");
	if (base && *base) {
		char exe[MAX_PATH];
		DWORD k = GetModuleFileNameA(NULL, exe, sizeof exe);
		const char *name = exe + k;
		while (name > exe && !is_sep(name[-1])) name--;
		size_t len = strlen(name);
		if (len > 4 && _stricmp(name + len - 4, ".exe") == 0) len -= 4;
		snprintf(buf, sizeof buf, "%s\\%.*s", base, (int)len, name);
		return dup_path(buf);
	}
#elif defined(__APPLE__)
	// On iOS, HOME is the app's container.
	const char *home = getenv("HOME");
	if (home && *home) {
		snprintf(buf, sizeof buf, "%s/Library/Application Support/%s", home, getprogname());
		return dup_path(buf);
	}
#elif defined(__wasm__)
	return dup_path("/data");
#else
	const char *xdg = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	char name[256] = "asuka";
	FILE *f = fopen("/proc/self/comm", "r");
	if (f) {
		if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = 0;
		fclose(f);
	}
	if (xdg && *xdg) {
		snprintf(buf, sizeof buf, "%s/%s", xdg, name);
		return dup_path(buf);
	}
	if (home && *home) {
		snprintf(buf, sizeof buf, "%s/.local/share/%s", home, name);
		return dup_path(buf);
	}
#endif
	return dup_path(".");
}

static ak_val app_dir_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	char *d = ak_fs_app_dir();
	ak_str *s = ak_str_c(d);
	ak_free(d);
	return (ak_val){.p = s};
}

// ---- The file system ----

#if defined(_WIN32)

// wide is p in UTF-16 (ak_free it); NULL when p has a NUL.
static wchar_t *wide(const ak_str *p) {
	if (memchr(p->data, 0, p->len)) {
		failed(FAIL_NOT_FOUND, "the path has a NUL");
		return NULL;
	}
	int n = MultiByteToWideChar(CP_UTF8, 0, p->data, (int)p->len, NULL, 0);
	wchar_t *w = ak_alloc(sizeof(wchar_t) * ((size_t)n + 1));
	MultiByteToWideChar(CP_UTF8, 0, p->data, (int)p->len, w, n);
	w[n] = 0;
	return w;
}

static char *narrow(const wchar_t *w, int *len) {
	int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
	char *s = ak_alloc((size_t)n + 1);
	WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL);
	*len = n > 0 ? n - 1 : 0;
	return s;
}

static int64_t filetime_ns(FILETIME ft) {
	uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
	return (int64_t)(t - 116444736000000000ull) * 100;
}

#else

// path is p as a C string (p's own data: strings end in a NUL); NULL
// when p has a NUL inside.
static const char *path(const ak_str *p) {
	if (memchr(p->data, 0, p->len)) {
		failed(FAIL_NOT_FOUND, "the path has a NUL");
		return NULL;
	}
	return p->len ? p->data : "";
}

#endif

static int cmp_str(const void *a, const void *b) { return ak_str_cmp(*(ak_str *const *)a, *(ak_str *const *)b); }

static ak_val list_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_arr *out = ak_str_arr(0);
#if defined(_WIN32)
	ak_str *pat = ak_str_new(NULL, STR(0)->len + 2);
	memcpy(pat->data, STR(0)->data, STR(0)->len);
	memcpy(pat->data + STR(0)->len, "\\*", 2);
	wchar_t *w = wide(pat);
	ak_release(pat);
	if (!w) goto fail;
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(w, &fd);
	ak_free(w);
	if (h == INVALID_HANDLE_VALUE) {
		if (GetLastError() == ERROR_FILE_NOT_FOUND) goto done;  // empty
		failed_os();
		goto fail;
	}
	do {
		if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
		int len;
		char *s = narrow(fd.cFileName, &len);
		ak_str_arr_push(&out, s, (size_t)len);
		ak_free(s);
	} while (FindNextFileW(h, &fd));
	FindClose(h);
done:;
#else
	const char *p = path(STR(0));
	if (!p) goto fail;
	DIR *d = opendir(p);
	if (!d) {
		failed_os();
		goto fail;
	}
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
		ak_str_arr_push(&out, e->d_name, strlen(e->d_name));
	}
	closedir(d);
#endif
	if (out->len > 1) qsort(out->data, out->len, sizeof(ak_val), cmp_str);
	return (ak_val){.p = out};
fail:
	ak_release(out);
	return (ak_val){.p = NULL};
}

static ak_val walk_info_fn(ak_val *args, uint32_t n);

// stat_fn is walkInfo's: [isDir, size, modified (ns), link, mode].
static ak_val stat_fn(ak_val *args, uint32_t n) { return walk_info_fn(args, n); }

static ak_val exists_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
#if defined(_WIN32)
	wchar_t *w = wide(STR(0));
	if (!w) return (ak_val){.i = 0};
	DWORD a = GetFileAttributesW(w);
	ak_free(w);
	return (ak_val){.i = a != INVALID_FILE_ATTRIBUTES};
#else
	const char *p = path(STR(0));
	struct stat st;
	return (ak_val){.i = p && stat(p, &st) == 0};
#endif
}

static ak_val read_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_buf b = {0};
#if defined(_WIN32)
	wchar_t *w = wide(STR(0));
	if (!w) return (ak_val){.p = NULL};
	HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	ak_free(w);
	if (h == INVALID_HANDLE_VALUE) {
		failed_os();
		return (ak_val){.p = NULL};
	}
	for (;;) {
		ak_buf_reserve(&b, 65536);
		DWORD got;
		if (!ReadFile(h, b.data + b.len, 65536, &got, NULL)) {
			failed_os();
			CloseHandle(h);
			ak_buf_free(&b);
			return (ak_val){.p = NULL};
		}
		if (got == 0) break;
		b.len += got;
	}
	CloseHandle(h);
#else
	const char *p = path(STR(0));
	if (!p) return (ak_val){.p = NULL};
	int fd = open(p, O_RDONLY);
	if (fd < 0) {
		failed_os();
		return (ak_val){.p = NULL};
	}
	struct stat st;
	if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		close(fd);
		failed(FAIL_IS_DIR, "is a directory");
		return (ak_val){.p = NULL};
	}
	for (;;) {
		ak_buf_reserve(&b, 65536);
		ssize_t got = read(fd, b.data + b.len, 65536);
		if (got < 0) {
			if (errno == EINTR) continue;
			failed_os();
			close(fd);
			ak_buf_free(&b);
			return (ak_val){.p = NULL};
		}
		if (got == 0) break;
		b.len += (size_t)got;
	}
	close(fd);
#endif
	if (b.len > UINT32_MAX) {
		ak_buf_free(&b);
		failed(FAIL_OTHER, "the file is larger than 4 GB");
		return (ak_val){.p = NULL};
	}
	ak_bytes *out = ak_bytes_new(b.data, b.len);
	ak_buf_free(&b);
	return (ak_val){.p = out};
}

// write_fn(p, data, append): the file made (or emptied, unless append)
// with data.
static ak_val write_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_bytes *data = BYTES(1);
	size_t len = data ? data->len : 0;
	bool append = args[2].i != 0;
#if defined(_WIN32)
	wchar_t *w = wide(STR(0));
	if (!w) return (ak_val){.i = 0};
	HANDLE h = CreateFileW(w, append ? FILE_APPEND_DATA : GENERIC_WRITE, FILE_SHARE_READ, NULL, append ? OPEN_ALWAYS : CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	ak_free(w);
	if (h == INVALID_HANDLE_VALUE) {
		failed_os();
		return (ak_val){.i = 0};
	}
	size_t off = 0;
	while (off < len) {
		DWORD put;
		DWORD chunk = len - off > (1u << 30) ? (1u << 30) : (DWORD)(len - off);
		if (!WriteFile(h, data->data + off, chunk, &put, NULL)) {
			failed_os();
			CloseHandle(h);
			return (ak_val){.i = 0};
		}
		off += put;
	}
	CloseHandle(h);
#else
	const char *p = path(STR(0));
	if (!p) return (ak_val){.i = 0};
	int fd = open(p, O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC), 0644);
	if (fd < 0) {
		failed_os();
		return (ak_val){.i = 0};
	}
	size_t off = 0;
	while (off < len) {
		ssize_t put = write(fd, data->data + off, len - off);
		if (put < 0) {
			if (errno == EINTR) continue;
			failed_os();
			close(fd);
			return (ak_val){.i = 0};
		}
		off += (size_t)put;
	}
	if (close(fd) != 0) {
		failed_os();
		return (ak_val){.i = 0};
	}
#endif
	return (ak_val){.i = 1};
}

static ak_val copy_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
#if defined(_WIN32)
	wchar_t *from = wide(STR(0)), *to = from ? wide(STR(1)) : NULL;
	BOOL ok = from && to && CopyFileW(from, to, FALSE);
	if (from && to && !ok) failed_os();
	ak_free(from);
	ak_free(to);
	return (ak_val){.i = ok};
#else
	const char *from = path(STR(0)), *to = from ? path(STR(1)) : NULL;
	if (!from || !to) return (ak_val){.i = 0};
	int in = open(from, O_RDONLY);
	if (in < 0) {
		failed_os();
		return (ak_val){.i = 0};
	}
	struct stat st;
	if (fstat(in, &st) == 0 && S_ISDIR(st.st_mode)) {
		close(in);
		failed(FAIL_IS_DIR, "is a directory");
		return (ak_val){.i = 0};
	}
	int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (out < 0) {
		failed_os();
		close(in);
		return (ak_val){.i = 0};
	}
	char buf[65536];
	bool ok = true;
	for (;;) {
		ssize_t got = read(in, buf, sizeof buf);
		if (got < 0 && errno == EINTR) continue;
		if (got <= 0) {
			if (got < 0) failed_os(), ok = false;
			break;
		}
		for (ssize_t off = 0; off < got;) {
			ssize_t put = write(out, buf + off, (size_t)(got - off));
			if (put < 0 && errno == EINTR) continue;
			if (put < 0) {
				failed_os();
				ok = false;
				break;
			}
			off += put;
		}
		if (!ok) break;
	}
	close(in);
	if (close(out) != 0 && ok) failed_os(), ok = false;
	return (ak_val){.i = ok};
#endif
}

static ak_val rename_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
#if defined(_WIN32)
	wchar_t *from = wide(STR(0)), *to = from ? wide(STR(1)) : NULL;
	BOOL ok = from && to && MoveFileExW(from, to, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
	if (from && to && !ok) failed_os();
	ak_free(from);
	ak_free(to);
	return (ak_val){.i = ok};
#else
	const char *from = path(STR(0)), *to = from ? path(STR(1)) : NULL;
	if (!from || !to) return (ak_val){.i = 0};
	if (rename(from, to) != 0) {
		failed_os();
		return (ak_val){.i = 0};
	}
	return (ak_val){.i = 1};
#endif
}

// make_dir makes one directory; true when it's there after (made, or
// one already).
static bool make_dir(const ak_str *p) {
#if defined(_WIN32)
	wchar_t *w = wide(p);
	if (!w) return false;
	bool ok = CreateDirectoryW(w, NULL) != 0;
	DWORD e = ok ? 0 : GetLastError();
	if (!ok) {
		// One there already: some shares say so as access denied.
		DWORD a = GetFileAttributesW(w);
		if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ok = true;
		else if (e == ERROR_ALREADY_EXISTS) failed(FAIL_EXISTS, "a file is there");
		else {
			SetLastError(e);
			failed_os();
		}
	}
	ak_free(w);
	return ok;
#else
	const char *c = path(p);
	if (!c) return false;
	if (mkdir(c, 0755) == 0) return true;
	int e = errno;
	struct stat st;
	if (e == EEXIST) {
		if (stat(c, &st) == 0 && S_ISDIR(st.st_mode)) return true;
		failed(FAIL_EXISTS, "a file is there");
		return false;
	}
	errno = e;
	failed_os();
	return false;
#endif
}

static ak_val mkdir_all_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *p = STR(0);
	size_t v = volume(p->data, p->len);
	// Each prefix that ends a name, in order.
	for (size_t i = v; i <= p->len; i++) {
		if (i < p->len && !is_sep(p->data[i])) continue;
		if (i == v || is_sep(p->data[i - 1])) continue;  // a root, or "//"
		ak_str *prefix = ak_str_new(p->data, i);
		bool ok = make_dir(prefix);
		ak_release(prefix);
		if (!ok) return (ak_val){.i = 0};
	}
	return (ak_val){.i = 1};
}

bool ak_fs_make_dirs(const char *path) {
	ak_str *s = ak_str_c(path);
	ak_val a = {.p = s};
	ak_val r = mkdir_all_fn(&a, 1);
	ak_release(s);
	return r.i != 0;
}

static bool remove_one(const ak_str *p) {
#if defined(_WIN32)
	wchar_t *w = wide(p);
	if (!w) return false;
	DWORD a = GetFileAttributesW(w);
	bool ok;
	if (a == INVALID_FILE_ATTRIBUTES) ok = false;
	else if (a & FILE_ATTRIBUTE_DIRECTORY) ok = RemoveDirectoryW(w) != 0;
	else ok = DeleteFileW(w) != 0;
	if (!ok) failed_os();
	ak_free(w);
	return ok;
#else
	const char *c = path(p);
	if (!c) return false;
	struct stat st;
	if (lstat(c, &st) != 0) {
		failed_os();
		return false;
	}
	if ((S_ISDIR(st.st_mode) ? rmdir(c) : unlink(c)) != 0) {
		failed_os();
		return false;
	}
	return true;
#endif
}

static ak_val remove_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.i = remove_one(STR(0))};
}

// remove_all removes p and what's in it; one that isn't there is
// removed already.
static bool remove_all(const ak_str *p, int depth) {
	if (depth > 256) {
		failed(FAIL_OTHER, "directories nested too deep");
		return false;
	}
	bool dir;
#if defined(_WIN32)
	wchar_t *w = wide(p);
	if (!w) return false;
	DWORD a = GetFileAttributesW(w);
	ak_free(w);
	if (a == INVALID_FILE_ATTRIBUTES) {
		DWORD e = GetLastError();
		if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
		failed_os();
		return false;
	}
	dir = (a & FILE_ATTRIBUTE_DIRECTORY) && !(a & FILE_ATTRIBUTE_REPARSE_POINT);
#else
	const char *c = path(p);
	if (!c) return false;
	struct stat st;
	if (lstat(c, &st) != 0) {
		if (errno == ENOENT) return true;
		failed_os();
		return false;
	}
	dir = S_ISDIR(st.st_mode);
#endif
	if (dir) {
		ak_val arg = {.p = (void *)p};
		ak_val names = list_fn(&arg, 1);
		ak_arr *a = names.p;
		if (!a) return false;
		for (uint32_t k = 0; k < a->len; k++) {
			ak_str *name = a->data[k].p;
			ak_str *child = ak_str_new(NULL, p->len + 1 + name->len);
			memcpy(child->data, p->data, p->len);
			child->data[p->len] = SEP;
			memcpy(child->data + p->len + 1, name->data, name->len);
			bool ok = remove_all(child, depth + 1);
			ak_release(child);
			if (!ok) {
				ak_release(a);
				return false;
			}
		}
		ak_release(a);
	}
	return remove_one(p);
}

static ak_val remove_all_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.i = remove_all(STR(0), 0)};
}

// ---- The user's directories ----

// cleaned is p cleaned (a directory's trailing separator gone), and p
// freed.
static ak_val cleaned(char *p) {
	ak_str *s = clean(p, strlen(p));
	ak_free(p);
	return (ak_val){.p = s};
}

static ak_val home_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
#if defined(AK_FS_SANDBOX)
	return cleaned(ak_fs_app_dir());
#elif defined(_WIN32)
	// The environment's, as Go's UserHomeDir: USERPROFILE, which every
	// session sets, else HOMEDRIVE and HOMEPATH (the core links nothing
	// but kernel32 and ws2_32, so not the shell's SHGetKnownFolderPath).
	char *v = ak_os_getenv("USERPROFILE", true);
	if (v && *v) return cleaned(v);
	ak_free(v);
	char *drive = ak_os_getenv("HOMEDRIVE", true), *dir = ak_os_getenv("HOMEPATH", true);
	if (drive && dir && *drive && *dir) {
		size_t a = strlen(drive), b = strlen(dir);
		char *both = ak_alloc(a + b + 1);
		memcpy(both, drive, a);
		memcpy(both + a, dir, b + 1);
		ak_free(drive);
		ak_free(dir);
		return cleaned(both);
	}
	ak_free(drive);
	ak_free(dir);
	return cleaned(dup_path("C:\\"));
#else
	char *v = ak_os_getenv("HOME", true);
	if (v && *v) return cleaned(v);
	ak_free(v);
	struct passwd pw, *found = NULL;
	char buf[4096];
	if (getpwuid_r(getuid(), &pw, buf, sizeof buf, &found) == 0 && found && found->pw_dir && *found->pw_dir) return cleaned(dup_path(found->pw_dir));
	return cleaned(dup_path("/"));
#endif
}

static ak_val temp_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
#if defined(_WIN32)
	wchar_t w[MAX_PATH + 2];
	DWORD k = GetTempPathW(MAX_PATH + 2, w);
	if (k == 0 || k > MAX_PATH + 1) return cleaned(dup_path("C:\\Windows\\Temp"));
	int len;
	return cleaned(narrow(w, &len));
#elif defined(__wasm__)
	return cleaned(dup_path("/tmp"));
#else
	char *v = ak_os_getenv("TMPDIR", true);
	if (v && *v) return cleaned(v);
	ak_free(v);
#if defined(__ANDROID__)
	// No TMPDIR for an app: its cache directory, beside its files
	// (Context.getCacheDir, next to getFilesDir: the app's directory).
	char *app = ak_fs_app_dir();
	ak_str *d = clean(app, strlen(app));
	ak_free(app);
	size_t i = d->len;
	while (i > 0 && d->data[i - 1] != '/') i--;
	char *cache = ak_alloc(i + 6);
	memcpy(cache, d->data, i);
	memcpy(cache + i, "cache", 6);
	ak_release(d);
	return cleaned(cache);
#else
	return cleaned(dup_path("/tmp"));
#endif
#endif
}

// absolute: p against the working directory, cleaned; none when the
// working directory can't be had (it was removed).
static ak_val abs_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *p = STR(0);
#if defined(_WIN32)
	ak_str *dot = p->len ? NULL : ak_str_new(".", 1);
	wchar_t *w = wide(dot ? dot : p);
	ak_release(dot);
	if (!w) return (ak_val){.p = NULL};
	DWORD k = GetFullPathNameW(w, 0, NULL, NULL);
	wchar_t *full = k ? ak_alloc(sizeof(wchar_t) * k) : NULL;
	DWORD got = full ? GetFullPathNameW(w, k, full, NULL) : 0;
	ak_free(w);
	if (got == 0 || got >= k) {
		failed_os();
		ak_free(full);
		return (ak_val){.p = NULL};
	}
	int len;
	char *s = narrow(full, &len);
	ak_free(full);
	return cleaned(s);
#else
	if (p->len && p->data[0] == '/') return (ak_val){.p = clean(p->data, p->len)};
#if defined(__wasm__)
	char *cwd = dup_path("/");
#else
	size_t cap = 1024;
	char *cwd = ak_alloc(cap);
	while (!getcwd(cwd, cap)) {
		if (errno != ERANGE || cap > (1u << 20)) {
			failed_os();
			ak_free(cwd);
			return (ak_val){.p = NULL};
		}
		ak_free(cwd);
		cap *= 2;
		cwd = ak_alloc(cap);
	}
#endif
	size_t c = strlen(cwd);
	char *both = ak_alloc(c + 1 + p->len + 1);
	memcpy(both, cwd, c);
	both[c] = '/';
	memcpy(both + c + 1, p->data, p->len);
	both[c + 1 + p->len] = 0;
	ak_free(cwd);
	return cleaned(both);
#endif
}

// walkInfo: what's at p as statInfo says it (what a link links to; a
// link that's broken: the link), and 1 when p is a link (walk doesn't
// enter one), 0 when it isn't.
static ak_val walk_info_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t is_dir, size, mtime, link, mode;
#if defined(_WIN32)
	wchar_t *w = wide(STR(0));
	if (!w) return (ak_val){.p = NULL};
	WIN32_FILE_ATTRIBUTE_DATA a;
	if (!GetFileAttributesExW(w, GetFileExInfoStandard, &a)) {
		failed_os();
		ak_free(w);
		return (ak_val){.p = NULL};
	}
	link = (a.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
	is_dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	// Windows keeps whether it may be written: rw-, or r--, for everyone.
	mode = (a.dwFileAttributes & FILE_ATTRIBUTE_READONLY) ? 0444 : 0666;
	if (is_dir) mode |= 0111;
	size = (int64_t)((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow);
	mtime = filetime_ns(a.ftLastWriteTime);
	if (link) {
		// What it links to, when it's there.
		HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
		BY_HANDLE_FILE_INFORMATION fi;
		if (h != INVALID_HANDLE_VALUE) {
			if (GetFileInformationByHandle(h, &fi)) {
				is_dir = (fi.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				size = (int64_t)((uint64_t)fi.nFileSizeHigh << 32 | fi.nFileSizeLow);
				mtime = filetime_ns(fi.ftLastWriteTime);
			}
			CloseHandle(h);
		}
	}
	ak_free(w);
#else
	const char *p = path(STR(0));
	struct stat st;
	if (!p) return (ak_val){.p = NULL};
	if (lstat(p, &st) != 0) {
		failed_os();
		return (ak_val){.p = NULL};
	}
	link = S_ISLNK(st.st_mode);
	struct stat to;
	if (link && stat(p, &to) == 0) st = to;
	is_dir = S_ISDIR(st.st_mode);
	mode = (int64_t)(st.st_mode & 07777);
	size = (int64_t)st.st_size;
#if defined(__APPLE__)
	mtime = (int64_t)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
#else
	mtime = (int64_t)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
#endif
	ak_arr *a5 = ak_arr_new(AK_K_INT, NULL, 5);
	a5->data[0].i = is_dir;
	a5->data[1].i = is_dir ? 0 : size;
	a5->data[2].i = mtime;
	a5->data[3].i = link;
	a5->data[4].i = mode;
	a5->len = 5;
	return (ak_val){.p = a5};
}

char *ak_fs_temp_dir(void) {
#if defined(_WIN32)
	char base[MAX_PATH + 1];
	DWORD n = GetTempPathA(sizeof base, base);
	if (n == 0 || n > MAX_PATH) return NULL;
	for (unsigned k = 0; k < 100; k++) {
		char *p = ak_alloc(n + 48);
		snprintf(p, n + 48, "%sasuka-test-%lu-%u", base, (unsigned long)GetCurrentProcessId(), (unsigned)(GetTickCount() + k));
		if (CreateDirectoryA(p, NULL)) return p;
		ak_free(p);
	}
	return NULL;
#elif defined(__wasm__)
	return NULL;
#else
	const char *base = getenv("TMPDIR");
	if (!base || !*base) base = "/tmp";
	size_t n = strlen(base) + 32;
	char *p = ak_alloc(n);
	snprintf(p, n, "%s/asuka-test-XXXXXX", base);
	if (!mkdtemp(p)) {
		ak_free(p);
		return NULL;
	}
	return p;
#endif
}

bool ak_fs_remove_tree(const char *path) {
	ak_str *p = ak_str_c(path);
	bool ok = remove_all(p, 0);
	ak_release(p);
	return ok;
}

// ---- Open files ----
//
// A file std/fs's open gives is a number into this table: what crosses to
// spawn's threads is a value, and each call on a file takes its lock, so
// the calls on one file happen one at a time, in the order they're made
// (std/fs awaits each). Reading and writing go through a buffer: a call
// that the buffer answers (a line already read) touches nothing, and the
// file is read 64 KB at a time.
//
//	fs.fileOpen(p, mode) -> int         0: failed (mode: read, write, append, readWrite)
//	fs.fileLineReady(id) -> bool        readLine wouldn't touch the file
//	fs.fileReadLine(id) -> string?      a line without its \n (\r\n); none at the end
//	fs.fileReadReady(id) -> bool        read wouldn't
//	fs.fileRead(id, n) -> bytes?        up to n bytes; empty at the end
//	fs.fileReadAll(id) -> bytes?        the rest
//	fs.fileBuffer(id, data) -> bool     data kept to write; whether it's time to flush
//	fs.fileFlush(id) -> bool
//	fs.fileSeek(id, offset, whence) -> int   the position after; -1: failed
//	fs.fileSize(id) -> int              -1: failed
//	fs.fileClose(id) -> bool
//	fs.fileFailed(id) -> bool           the last call on it failed (why: failureKind)

#define FILE_CHUNK 65536

typedef struct afile {
	int64_t id;
	ak_mutex mu;
#if defined(_WIN32)
	HANDLE h;
#else
	int fd;
#endif
	ak_buf in;       // read, not yet taken: in.data[roff, in.len)
	size_t roff;
	bool eof;        // the file has no more after in
	ak_buf out;      // written, not yet in the file
	bool failed;     // the last call failed
	bool closed;
	int users;       // calls in flight (fmu)
	struct afile *next;
} afile;

static ak_mutex fmu = AK_MUTEX_INIT;
static afile *files;
static int64_t next_file = 1;

// take finds the open file id and holds it (its lock taken); NULL, with
// the failure, when there's none.
static afile *take(int64_t id) {
	ak_mutex_lock(&fmu);
	afile *f = files;
	while (f && f->id != id) f = f->next;
	if (f) f->users++;
	ak_mutex_unlock(&fmu);
	if (!f) {
		failed(FAIL_OTHER, "the file is closed");
		return NULL;
	}
	ak_mutex_lock(&f->mu);
	if (f->closed) {
		ak_mutex_unlock(&f->mu);
		ak_mutex_lock(&fmu);
		f->users--;
		ak_mutex_unlock(&fmu);
		failed(FAIL_OTHER, "the file is closed");
		return NULL;
	}
	f->failed = false;
	return f;
}

// give lets f go: freed once it's closed and nothing else holds it.
static void give(afile *f, bool ok) {
	if (!ok) f->failed = true;
	bool closed = f->closed;
	ak_mutex_unlock(&f->mu);
	ak_mutex_lock(&fmu);
	bool last = --f->users == 0 && closed;
	if (last) {
		for (afile **p = &files; *p; p = &(*p)->next) {
			if (*p == f) {
				*p = f->next;
				break;
			}
		}
	}
	ak_mutex_unlock(&fmu);
	if (last) {
		ak_buf_free(&f->in);
		ak_buf_free(&f->out);
		ak_free(f);
	}
}

static bool raw_write(afile *f, const uint8_t *p, size_t n) {
	size_t off = 0;
	while (off < n) {
#if defined(_WIN32)
		DWORD put;
		DWORD chunk = n - off > (1u << 30) ? (1u << 30) : (DWORD)(n - off);
		if (!WriteFile(f->h, p + off, chunk, &put, NULL)) {
			failed_os();
			return false;
		}
#else
		ssize_t put = write(f->fd, p + off, n - off);
		if (put < 0) {
			if (errno == EINTR) continue;
			failed_os();
			return false;
		}
#endif
		off += (size_t)put;
	}
	return true;
}

// raw_read reads up to n bytes into p: how many, 0 at the end, -1 failed.
static int64_t raw_read(afile *f, uint8_t *p, size_t n) {
	for (;;) {
#if defined(_WIN32)
		DWORD got;
		if (!ReadFile(f->h, p, (DWORD)n, &got, NULL)) {
			if (GetLastError() == ERROR_BROKEN_PIPE || GetLastError() == ERROR_HANDLE_EOF) return 0;
			failed_os();
			return -1;
		}
		return got;
#else
		ssize_t got = read(f->fd, p, n);
		if (got < 0 && errno == EINTR) continue;
		if (got < 0) failed_os();
		return got;
#endif
	}
}

static bool raw_seek(afile *f, int64_t off, int whence, int64_t *at) {
#if defined(_WIN32)
	LARGE_INTEGER d, now;
	d.QuadPart = off;
	if (!SetFilePointerEx(f->h, d, &now, whence == 0 ? FILE_BEGIN : whence == 1 ? FILE_CURRENT : FILE_END)) {
		failed_os();
		return false;
	}
	*at = now.QuadPart;
#else
	off_t r = lseek(f->fd, (off_t)off, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
	if (r < 0) {
		failed_os();
		return false;
	}
	*at = (int64_t)r;
#endif
	return true;
}

static bool flush_out(afile *f) {
	if (!f->out.len) return true;
	bool ok = raw_write(f, f->out.data, f->out.len);
	f->out.len = 0;
	return ok;
}

// unread gives back what's read ahead: the file's position is where the
// program has read to, before it writes or seeks.
static bool unread(afile *f) {
	size_t ahead = f->in.len - f->roff;
	f->in.len = f->roff = 0;
	f->eof = false;
	if (!ahead) return true;
	int64_t at;
	return raw_seek(f, -(int64_t)ahead, 1, &at);
}

// fill reads the next chunk into in; false when it failed.
static bool fill(afile *f) {
	if (!flush_out(f)) return false;
	if (f->roff && f->roff == f->in.len) f->in.len = f->roff = 0;
	ak_buf_reserve(&f->in, FILE_CHUNK);
	int64_t got = raw_read(f, f->in.data + f->in.len, FILE_CHUNK);
	if (got < 0) return false;
	if (got == 0) f->eof = true;
	f->in.len += (size_t)got;
	return true;
}

static const uint8_t *line_end(afile *f) {
	if (f->roff == f->in.len) return NULL;  // nothing read: in.data may be NULL
	return memchr(f->in.data + f->roff, '\n', f->in.len - f->roff);
}

// taken is the next k bytes read, as bytes (in.data is NULL until the
// first read).
static ak_bytes *taken(afile *f, size_t k) {
	ak_bytes *b = ak_bytes_new(k ? f->in.data + f->roff : NULL, k);
	f->roff += k;
	return b;
}

static ak_val file_open_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int mode = (int)args[1].i;
	afile *f = ak_alloc(sizeof *f);
	memset(f, 0, sizeof *f);
#if defined(_WIN32)
	wchar_t *w = wide(STR(0));
	if (!w) {
		ak_free(f);
		return (ak_val){.i = 0};
	}
	static const DWORD access[] = {GENERIC_READ, GENERIC_WRITE, FILE_APPEND_DATA, GENERIC_READ | GENERIC_WRITE};
	static const DWORD disp[] = {OPEN_EXISTING, CREATE_ALWAYS, OPEN_ALWAYS, OPEN_ALWAYS};
	f->h = CreateFileW(w, access[mode & 3], FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp[mode & 3], FILE_ATTRIBUTE_NORMAL, NULL);
	ak_free(w);
	if (f->h == INVALID_HANDLE_VALUE) {
		failed_os();
		ak_free(f);
		return (ak_val){.i = 0};
	}
#else
	const char *p = path(STR(0));
	if (!p) {
		ak_free(f);
		return (ak_val){.i = 0};
	}
	static const int flags[] = {O_RDONLY, O_WRONLY | O_CREAT | O_TRUNC, O_WRONLY | O_CREAT | O_APPEND, O_RDWR | O_CREAT};
	f->fd = open(p, flags[mode & 3] | O_CLOEXEC, 0644);
	if (f->fd < 0) {
		failed_os();
		ak_free(f);
		return (ak_val){.i = 0};
	}
	struct stat st;
	if (fstat(f->fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		close(f->fd);
		ak_free(f);
		failed(FAIL_IS_DIR, "is a directory");
		return (ak_val){.i = 0};
	}
#endif
	ak_mutex_init(&f->mu);
	ak_mutex_lock(&fmu);
	f->id = next_file++;
	f->next = files;
	files = f;
	ak_mutex_unlock(&fmu);
	return (ak_val){.i = f->id};
}

static ak_val file_line_ready_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.i = 1};  // the call says why
	bool ready = f->eof || line_end(f);
	give(f, true);
	return (ak_val){.i = ready};
}

static ak_val file_read_line_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.p = NULL};
	const uint8_t *nl;
	while (!(nl = line_end(f)) && !f->eof) {
		if (!fill(f)) {
			give(f, false);
			return (ak_val){.p = NULL};
		}
	}
	size_t start = f->roff, end;
	if (nl) {
		end = (size_t)(nl - f->in.data);
		f->roff = end + 1;
	} else if (f->roff < f->in.len) {
		end = f->in.len;  // the last line, without a \n
		f->roff = end;
	} else {
		give(f, true);
		return (ak_val){.p = NULL};  // the end
	}
	size_t len = end - start;
	if (len && f->in.data[start + len - 1] == '\r') len--;
	ak_str *s = ak_str_new((const char *)f->in.data + start, len);
	give(f, true);
	return (ak_val){.p = s};
}

static ak_val file_read_ready_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.i = 1};
	bool ready = f->eof || f->roff < f->in.len;
	give(f, true);
	return (ak_val){.i = ready};
}

static ak_val file_read_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.p = NULL};
	int64_t want = args[1].i < 1 ? 1 : args[1].i > (1 << 30) ? (1 << 30) : args[1].i;
	if (f->roff == f->in.len && !f->eof && !fill(f)) {
		give(f, false);
		return (ak_val){.p = NULL};
	}
	size_t have = f->in.len - f->roff, k = have < (size_t)want ? have : (size_t)want;
	ak_bytes *b = taken(f, k);
	give(f, true);
	return (ak_val){.p = b};
}

static ak_val file_read_all_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.p = NULL};
	while (!f->eof) {
		if (f->in.len - f->roff > UINT32_MAX) {
			failed(FAIL_OTHER, "the rest of the file is larger than 4 GB");
			give(f, false);
			return (ak_val){.p = NULL};
		}
		if (!fill(f)) {
			give(f, false);
			return (ak_val){.p = NULL};
		}
	}
	ak_bytes *b = taken(f, f->in.len - f->roff);
	give(f, true);
	return (ak_val){.p = b};
}

static ak_val file_buffer_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.i = 1};  // the flush says why
	bool ok = unread(f);
	const ak_bytes *d = BYTES(1);
	if (ok && d && d->len) {
		ak_buf_reserve(&f->out, d->len);
		memcpy(f->out.data + f->out.len, d->data, d->len);
		f->out.len += d->len;
	}
	bool due = !ok || f->out.len >= FILE_CHUNK;
	give(f, ok);
	return (ak_val){.i = due};
}

static ak_val file_flush_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.i = 0};
	if (f->failed) {
		give(f, false);
		return (ak_val){.i = 0};
	}
	bool ok = flush_out(f);
	give(f, ok);
	return (ak_val){.i = ok};
}

static ak_val file_seek_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.i = -1};
	int64_t at = -1;
	int whence = (int)args[2].i;
	int64_t off = args[1].i;
	if (whence == 1) off -= (int64_t)(f->in.len - f->roff);  // from where the program has read to
	bool ok = flush_out(f);
	f->in.len = f->roff = 0;
	f->eof = false;
	ok = ok && raw_seek(f, off, whence, &at);
	give(f, ok);
	return (ak_val){.i = ok ? at : -1};
}

static ak_val file_size_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.i = -1};
	int64_t size = -1;
	bool ok = flush_out(f);
#if defined(_WIN32)
	LARGE_INTEGER sz;
	if (ok && GetFileSizeEx(f->h, &sz)) size = sz.QuadPart;
	else if (ok) failed_os(), ok = false;
#else
	struct stat st;
	if (ok && fstat(f->fd, &st) == 0) size = (int64_t)st.st_size;
	else if (ok) failed_os(), ok = false;
#endif
	give(f, ok);
	return (ak_val){.i = size};
}

static ak_val file_close_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	afile *f = take(args[0].i);
	if (!f) return (ak_val){.i = 1};  // closed already: nothing to do
	bool ok = flush_out(f);
#if defined(_WIN32)
	if (!CloseHandle(f->h) && ok) failed_os(), ok = false;
#else
	if (close(f->fd) != 0 && ok) failed_os(), ok = false;
#endif
	f->closed = true;
	give(f, ok);
	return (ak_val){.i = ok};
}

static ak_val file_failed_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_mutex_lock(&fmu);
	afile *f = files;
	while (f && f->id != args[0].i) f = f->next;
	bool failed_ = !f || f->failed;  // closed: its calls fail
	ak_mutex_unlock(&fmu);
	return (ak_val){.i = failed_};
}

// ---- Patterns ----
//
// fs.matches(pattern, path): * any run of characters but a separator,
// ? one, [abc] [a-z] [!a] one of a set (or not), {a,b} either, ** any
// run of directories (a whole name of the pattern's). A name that starts
// with a dot is matched only by a pattern's that starts with one too, as
// shells match. \ quotes the next character (on Windows a separator, as
// /: write [*] for a literal *).

static bool sep_at(const char *p) {
#if defined(_WIN32)
	return *p == '/' || *p == '\\';
#else
	return *p == '/';
#endif
}

static bool match_here(const char *p, const char *pe, const char *s, const char *se, int depth);

// class matches s's character against [...] at p; *after is past it.
static bool class_match(const char *p, const char *pe, uint32_t c, const char **after) {
	bool neg = p < pe && (*p == '!' || *p == '^');
	if (neg) p++;
	bool hit = false, first = true;
	while (p < pe && (*p != ']' || first)) {
		first = false;
		size_t i = 0;
		uint32_t lo = ak_utf8_decode(p, (size_t)(pe - p), &i);
		p += i;
		uint32_t hi = lo;
		if (p + 1 < pe && *p == '-' && p[1] != ']') {
			i = 0;
			hi = ak_utf8_decode(p + 1, (size_t)(pe - p - 1), &i);
			p += 1 + i;
		}
		if (c >= lo && c <= hi) hit = true;
	}
	if (p >= pe) return false;  // no ]: not a class
	*after = p + 1;
	return hit != neg;
}

static bool match_here(const char *p, const char *pe, const char *s, const char *se, int depth) {
	if (depth > 64) return false;
	// The last * met, to go back to: it takes one more character and the
	// rest is tried again (the classic wildcard matcher, O(pattern ×
	// path): only the last * needs trying again, since one before it can
	// only take what this one could).
	const char *star_p = NULL, *star_s = NULL;
	bool name_start = true;  // at a name's first character, where a dot is the pattern's to match
	for (;;) {
		if (p == pe) {
			if (s == se) return true;
			goto mismatch;
		}
		if (*p == '*' && p + 1 < pe && p[1] == '*' && (p + 2 == pe || sep_at(p + 2)) && name_start) {
			// **: any directories, none included; the rest is tried after
			// each separator (or at the start).
			const char *rest = p + 2 == pe ? pe : p + 3;
			if (rest == pe) {
				if (s == se || *s != '.') return true;
				goto mismatch;
			}
			for (const char *t = s;; t++) {
				if ((t == s || sep_at(t - 1)) && (t == se || *t != '.' || *rest == '.') && match_here(rest, pe, t, se, depth + 1)) return true;
				if (t == se || (t > s && t[-1] == '.' && (t - 1 == s || sep_at(t - 2)))) break;
			}
			goto mismatch;
		}
		switch (*p) {
		case '*':
			if (name_start && s < se && *s == '.') goto mismatch;
			star_p = ++p, star_s = s;
			name_start = false;
			continue;
		case '?': {
			if (s == se || sep_at(s) || (name_start && *s == '.')) goto mismatch;
			size_t i = 0;
			ak_utf8_decode(s, (size_t)(se - s), &i);
			s += i, p++;
			name_start = false;
			continue;
		}
		case '[': {
			if (s == se || sep_at(s) || (name_start && *s == '.')) goto mismatch;
			size_t i = 0;
			uint32_t c = ak_utf8_decode(s, (size_t)(se - s), &i);
			const char *after = NULL;
			if (class_match(p + 1, pe, c, &after)) {
				s += i, p = after;
				name_start = false;
				continue;
			}
			if (after == NULL) goto literal;  // no ]: a [ as it is
			goto mismatch;
		}
		case '{': {
			// {a,b}: each alternative with the rest of the pattern.
			const char *close = p + 1;
			int nest = 0;
			while (close < pe && (*close != '}' || nest)) {
				if (*close == '{') nest++;
				if (*close == '}') nest--;
				close++;
			}
			if (close >= pe) goto literal;
			const char *alt = p + 1;
			for (const char *q = p + 1;; q++) {
				if (q == close || (*q == ',' && nest == 0)) {
					size_t a = (size_t)(q - alt), r = (size_t)(pe - close - 1);
					char small[256];
					char *buf = a + r < sizeof small ? small : ak_alloc(a + r + 1);
					memcpy(buf, alt, a);
					memcpy(buf + a, close + 1, r);
					bool ok = match_here(buf, buf + a + r, s, se, depth + 1);
					if (buf != small) ak_free(buf);
					if (ok) return true;
					if (q == close) break;
					alt = q + 1;
				} else if (*q == '{') {
					nest++;
				} else if (*q == '}') {
					nest--;
				}
			}
			goto mismatch;
		}
#if !defined(_WIN32)
		case '\\':
			if (p + 1 < pe) p++;
			goto literal;
#endif
		default:
		literal:
			if (sep_at(p)) {
				if (s == se || !sep_at(s)) goto mismatch;
				p++, s++;
				name_start = true;
				continue;
			}
			if (s == se || *p != *s) goto mismatch;
			p++, s++;
			name_start = false;
			continue;
		}
	mismatch:
		// The last * takes one more character, if it may (not a separator).
		if (!star_p || star_s == se || sep_at(star_s)) return false;
		{
			size_t i = 0;
			ak_utf8_decode(star_s, (size_t)(se - star_s), &i);
			star_s += i;
		}
		s = star_s, p = star_p;
		name_start = false;
	}
}

static ak_val matches_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *p = STR(0), *s = STR(1);
	return (ak_val){.i = match_here(p->data, p->data + p->len, s->data, s->data + s->len, 0)};
}

// ---- Modes and links ----

static ak_val chmod_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t mode = args[1].i;
#if defined(_WIN32)
	wchar_t *w = wide(STR(0));
	if (!w) return (ak_val){.i = 0};
	DWORD a = GetFileAttributesW(w);
	bool ok = a != INVALID_FILE_ATTRIBUTES;
	if (ok) {
		// Windows keeps whether it may be written, nothing else.
		a = mode & 0200 ? a & ~(DWORD)FILE_ATTRIBUTE_READONLY : a | FILE_ATTRIBUTE_READONLY;
		ok = SetFileAttributesW(w, a);
	}
	if (!ok) failed_os();
	ak_free(w);
	return (ak_val){.i = ok};
#elif defined(__wasm__)
	AK_UNUSED(mode);
	failed(FAIL_OTHER, "the web's files have no permissions");
	return (ak_val){.i = 0};
#else
	const char *p = path(STR(0));
	if (!p) return (ak_val){.i = 0};
	if (chmod(p, (mode_t)(mode & 07777)) != 0) {
		failed_os();
		return (ak_val){.i = 0};
	}
	return (ak_val){.i = 1};
#endif
}

static ak_val symlink_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
#if defined(_WIN32)
	wchar_t *to = wide(STR(0)), *at = wide(STR(1));
	bool ok = false;
	if (to && at) {
		DWORD attrs = GetFileAttributesW(to);
		DWORD flags = 0x2;  // SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE: Developer Mode's
		if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) flags |= SYMBOLIC_LINK_FLAG_DIRECTORY;
		ok = CreateSymbolicLinkW(at, to, flags) != 0;
		if (!ok) failed_os();
	}
	ak_free(to);
	ak_free(at);
	return (ak_val){.i = ok};
#else
	const char *to = path(STR(0)), *at = path(STR(1));
	if (!to || !at) return (ak_val){.i = 0};
	if (symlink(to, at) != 0) {
		failed_os();
		return (ak_val){.i = 0};
	}
	return (ak_val){.i = 1};
#endif
}

static ak_val read_link_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
#if defined(_WIN32)
	wchar_t *w = wide(STR(0));
	if (!w) return (ak_val){.p = NULL};
	HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
	ak_free(w);
	if (h == INVALID_HANDLE_VALUE) {
		failed_os();
		return (ak_val){.p = NULL};
	}
	// The final path of what it links to: Windows keeps the target as the
	// link's reparse data, which GetFinalPathNameByHandle reads followed.
	CloseHandle(h);
	w = wide(STR(0));
	h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	ak_free(w);
	if (h == INVALID_HANDLE_VALUE) {
		failed_os();
		return (ak_val){.p = NULL};
	}
	wchar_t buf[4096];
	DWORD got = GetFinalPathNameByHandleW(h, buf, 4096, 0);
	CloseHandle(h);
	if (got == 0 || got >= 4096) {
		failed_os();
		return (ak_val){.p = NULL};
	}
	const wchar_t *b = buf;
	if (wcsncmp(b, L"\\\\?\\", 4) == 0) b += 4;
	int len;
	char *s = narrow(b, &len);
	ak_str *out = ak_str_new(s, (size_t)len);
	ak_free(s);
	return (ak_val){.p = out};
#else
	const char *p = path(STR(0));
	if (!p) return (ak_val){.p = NULL};
	char buf[4096];
	ssize_t got = readlink(p, buf, sizeof buf);
	if (got < 0) {
		failed_os();
		return (ak_val){.p = NULL};
	}
	if ((size_t)got >= sizeof buf) {
		failed(FAIL_OTHER, "the link's target is too long");
		return (ak_val){.p = NULL};
	}
	return (ak_val){.p = ak_str_new(buf, (size_t)got)};
#endif
}

const ak_lib_entry ak_std_fs[] = {
	{"fs.appDir", app_dir_fn},
	{"fs.home", home_fn},
	{"fs.tempDir", temp_fn},
	{"fs.absolute", abs_fn},
	{"fs.join", join_fn},
	{"fs.clean", clean_fn},
	{"fs.dir", dir_fn},
	{"fs.base", base_fn},
	{"fs.ext", ext_fn},
	{"fs.listNames", list_fn},
	{"fs.statInfo", stat_fn},
	{"fs.walkInfo", walk_info_fn},
	{"fs.exists", exists_fn},
	{"fs.readFile", read_fn},
	{"fs.writeFile", write_fn},
	{"fs.copyFile", copy_fn},
	{"fs.moveFile", rename_fn},
	{"fs.makeDirs", mkdir_all_fn},
	{"fs.removeOne", remove_fn},
	{"fs.removeTree", remove_all_fn},
	{"fs.fileOpen", file_open_fn},
	{"fs.fileLineReady", file_line_ready_fn},
	{"fs.fileReadLine", file_read_line_fn},
	{"fs.fileReadReady", file_read_ready_fn},
	{"fs.fileRead", file_read_fn},
	{"fs.fileReadAll", file_read_all_fn},
	{"fs.fileBuffer", file_buffer_fn},
	{"fs.fileFlush", file_flush_fn},
	{"fs.fileSeek", file_seek_fn},
	{"fs.fileSize", file_size_fn},
	{"fs.fileClose", file_close_fn},
	{"fs.fileFailed", file_failed_fn},
	{"fs.matches", matches_fn},
	{"fs.chmodPath", chmod_fn},
	{"fs.symlinkPath", symlink_fn},
	{"fs.readLinkPath", read_link_fn},
	{"fs.failureKind", failure},
	{"fs.failureReason", failure_reason},
	{NULL, NULL},
};
