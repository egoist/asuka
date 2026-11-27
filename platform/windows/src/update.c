// update.c: the host's "update" module (DESIGN.md, "Updates": what
// platform/update, written once in Asuka, asks of a desktop host), JSON both
// ways through platform/host's call, each method answering once:
//
//   info                 {installable, reason, message, target, version,
//                        name, feed, key, kind, root, exe, work}: a Windows
//                        app is a folder (the .exe and its assets, with
//                        .asuka-files), updated where it is
//   download {url, to}   {id}, at once: a GET of url (WinHTTP: redirects,
//                        the system's proxy, User-Agent asuka-updater) into
//                        the file to, made or emptied
//   next {id}            {downloaded, total, done, error, status}: a long
//                        poll, answered when downloaded has moved by 1% of
//                        total (64 KB while it's unknown) or it's ended
//   cancel {id}          {}: stops it ("canceled")
//   install {staged}     {}: the staged tree's entries replace the
//                        installed ones now (internal/update's Swap)
//
// The running .exe is renamed aside (Windows lets it, not delete it) and
// removed at the next launch, with what a stopped update left
// (wh_update_clean). relaunch() is the Quit command's relaunch
// (instance.c), whose new process waits for this one (wh_relaunch_wait).
#include "modules.h"


#define FILE_LIST ".asuka-files"
#define FILE_LIST_W L".asuka-files"
#define WORK_PREFIX ".asuka-update-"

// ---- Where the app is installed ----

// exe_path is the running executable, its links resolved (free it).
static wchar_t *exe_path(void) {
	wchar_t raw[MAX_PATH * 4];
	GetModuleFileNameW(NULL, raw, WH_LEN(raw));
	HANDLE f = CreateFileW(raw, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
	if (f != INVALID_HANDLE_VALUE) {
		wchar_t real[MAX_PATH * 4];
		DWORD n = GetFinalPathNameByHandleW(f, real, WH_LEN(real), FILE_NAME_NORMALIZED);
		CloseHandle(f);
		if (n && n < WH_LEN(real)) {
			const wchar_t *p = real;
			if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) {
				wchar_t *o = wh_alloc((wcslen(p) + 2) * sizeof(wchar_t));
				o[0] = '\\';
				wcscpy(o + 1, p + 7);
				return o;
			}
			if (!wcsncmp(p, L"\\\\?\\", 4)) p += 4;
			return _wcsdup(p);
		}
	}
	return _wcsdup(raw);
}

// full_path is p made absolute and normal (its separators single, none
// at its end): free it.
static wchar_t *full_path(const wchar_t *p) {
	DWORD n = GetFullPathNameW(p, 0, NULL, NULL);
	if (!n) return _wcsdup(p);
	wchar_t *o = wh_alloc((n + 1) * sizeof(wchar_t));
	GetFullPathNameW(p, n + 1, o, NULL);
	size_t l = wcslen(o);
	while (l > 3 && o[l - 1] == '\\') o[--l] = 0;
	return o;
}

static wchar_t *dir_of(const wchar_t *p) {
	wchar_t *d = _wcsdup(p);
	wchar_t *s = wcsrchr(d, '\\');
	if (s) *s = 0;
	return d;
}

static wchar_t *wjoin(const wchar_t *a, const wchar_t *b) {
	wchar_t *o = wh_alloc((wcslen(a) + wcslen(b) + 2) * sizeof(wchar_t));
	wcscpy(o, a);
	if (*a && a[wcslen(a) - 1] != '\\') wcscat(o, L"\\");
	wcscat(o, b);
	return o;
}

static bool writable(const wchar_t *dir) {
	wchar_t name[64];
	swprintf(name, WH_LEN(name), L".asuka-probe-%lu-%llu", GetCurrentProcessId(), (unsigned long long)wh_now_ns());
	wchar_t *p = wjoin(dir, name);
	bool ok = CreateDirectoryW(p, NULL);
	if (ok) RemoveDirectoryW(p);
	free(p);
	return ok;
}

static const char *own_target(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
	return "windows-arm64";
#else
	return "windows-amd64";
#endif
}

// has_file_list reports whether dir has the file list asuka build writes
// beside a Windows app's .exe.
static bool has_file_list(const wchar_t *dir) {
	wchar_t p[MAX_PATH * 2];
	swprintf(p, WH_LEN(p), L"%ls\\" FILE_LIST_W, dir);
	DWORD a = GetFileAttributesW(p);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static void m_info(wh_reply *r, const uint8_t *a, size_t n) {
	(void)a, (void)n;
	wchar_t *exe = exe_path(), *dir = dir_of(exe);
	char *exe8 = wh_utf8(exe, -1, NULL), *dir8 = wh_utf8(dir, -1, NULL);
	const dsk_config *c = &H.config;
	const char *reason = "", *message = "";
	// The folder, lower case, with a separator at its end (room for it).
	size_t dl_ = wcslen(dir);
	wchar_t *low = wh_alloc((dl_ + 2) * sizeof *low);
	wcscpy(low, dir);
	_wcslwr(low);
	low[dl_] = L'\\', low[dl_ + 1] = 0;
	if (!c->update_feed || !*c->update_feed || !c->update_key || !*c->update_key) {
		reason = "notConfigured", message = "updates aren't set up for this app (asuka.toml's updates: a feed and a key)";
	} else if (wh_packaged() || wcsstr(low, L"\\windowsapps\\")) {
		reason = "managed", message = "a package manager or an app store installed the app, and updates it";  // MSIX: the Store, App Installer
	} else if (!has_file_list(dir)) {
		reason = "notInstalled", message = "the app isn't in a folder asuka build made (its .asuka-files)";
	} else if (!writable(dir)) {
		reason = "notWritable", message = "the app can't write where it's installed";
	}
	free(low);
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "installable"), dsk_jw_bool(&w, !*reason);
	dsk_jw_key(&w, "reason"), dsk_jw_str(&w, reason);
	dsk_jw_key(&w, "message"), dsk_jw_str(&w, message);
	dsk_jw_key(&w, "target"), dsk_jw_str(&w, c->update_target && *c->update_target ? c->update_target : own_target());
	dsk_jw_key(&w, "version"), dsk_jw_str(&w, c->version ? c->version : "");
	dsk_jw_key(&w, "name"), dsk_jw_str(&w, H.app_name ? H.app_name : "");
	dsk_jw_key(&w, "feed"), dsk_jw_str(&w, c->update_feed ? c->update_feed : "");
	dsk_jw_key(&w, "key"), dsk_jw_str(&w, c->update_key ? c->update_key : "");
	dsk_jw_key(&w, "kind"), dsk_jw_str(&w, "folder");
	dsk_jw_key(&w, "root"), dsk_jw_str(&w, dir8);
	dsk_jw_key(&w, "exe"), dsk_jw_str(&w, exe8);
	dsk_jw_key(&w, "work"), dsk_jw_str(&w, dir8);
	dsk_jw_end(&w);
	free(exe), free(dir), free(exe8), free(dir8);
	wh_reply_json(r, &w);
}

// ---- Downloads ----

typedef struct dl {
	uint32_t id;
	char *url, *to;
	SRWLOCK lock;
	volatile bool cancel;
	asuka_abort *abort;  // m_cancel's: stops the request in flight
	// Under lock, as the thread has them.
	uint64_t downloaded, total;
	int status;
	bool done;
	char *error;
	int posts;  // wake-ups of the main thread on their way
	// The main thread's: what next answered last, who waits; the end
	// reported (its id forgotten), the thread's last wake-up run.
	uint64_t reported;
	bool status_reported, done_reported, finished;
	wh_reply *waiting;
	struct dl *next;
} dl;

static dl *downloads;
static uint32_t next_id;

static dl *find_dl(uint32_t id) {
	for (dl *d = downloads; d; d = d->next)
		if (d->id == id) return d;
	return NULL;
}

static void forget(dl *d) {
	for (dl **p = &downloads; *p; p = &(*p)->next)
		if (*p == d) {
			*p = d->next;
			break;
		}
	free(d->url), free(d->to), free(d->error);
	asuka_abort_free(d->abort);
	free(d);
}

static uint64_t step(uint64_t total) { return total ? (total / 100 ? total / 100 : 1) : 64 * 1024; }

// answer_next answers the waiting next if there's news: progress by a
// step, the status known, or the end. After the end the id is forgotten
// (once its thread has ended too).
static bool answer_next(dl *d, wh_reply *r, bool force) {
	AcquireSRWLockShared(&d->lock);
	uint64_t got = d->downloaded, total = d->total;
	int status = d->status;
	bool done = d->done;
	char *error = d->error ? wh_strdup(d->error) : NULL;
	ReleaseSRWLockShared(&d->lock);
	bool news = done || got - d->reported >= step(total) || (status && !d->status_reported);
	if (!news && !force) {
		free(error);
		return false;
	}
	d->reported = got, d->status_reported = status != 0;
	if (done) d->done_reported = true, d->id = 0;  // forgotten by id
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "downloaded"), dsk_jw_int(&w, (int64_t)got);
	dsk_jw_key(&w, "total"), dsk_jw_int(&w, (int64_t)total);
	dsk_jw_key(&w, "done"), dsk_jw_bool(&w, done);
	dsk_jw_key(&w, "error"), dsk_jw_str(&w, error ? error : "");
	dsk_jw_key(&w, "status"), dsk_jw_int(&w, status);
	dsk_jw_end(&w);
	free(error);
	wh_reply_json(r, &w);
	return true;
}

// maybe_free frees a download once its end was reported and no wake-up
// of its thread is to come.
static void maybe_free(dl *d) {
	if (d->done_reported && d->finished) forget(d);
}

static void wake(void *ctx) {
	dl *d = ctx;
	AcquireSRWLockExclusive(&d->lock);
	d->posts--;
	// Done, and this the last wake-up: the thread posts no more.
	if (d->done && !d->posts) d->finished = true;
	ReleaseSRWLockExclusive(&d->lock);
	if (d->waiting && answer_next(d, d->waiting, false)) d->waiting = NULL;
	maybe_free(d);
}

// want_wake says whether the thread posts the main thread a wake-up
// (under lock: one at a time, but the end's always comes), counting it.
// It's posted after the lock is released: once the end's is posted the
// main thread may free d, its lock with it, so the thread touches d no
// more.
static bool want_wake(dl *d) {
	if (d->posts && !d->done) return false;
	d->posts++;
	return true;
}

// The download's file, as asuka_fetch_stream hands it over: a 200's body
// only, its progress reported as it comes.
typedef struct {
	dl *d;
	HANDLE f;
	bool keep;
} dl_file;

static void progress(dl *d, int status, uint64_t total, uint64_t downloaded) {
	AcquireSRWLockExclusive(&d->lock);
	d->downloaded = downloaded, d->total = total, d->status = status;
	bool post = want_wake(d);
	ReleaseSRWLockExclusive(&d->lock);
	if (post) wh_post(wake, d);  // not the end's: d stays
}

static bool dl_head(void *ctx, const asuka_response *r, asuka_error *err) {
	dl_file *x = ctx;
	uint64_t total = 0;
	for (size_t i = 0; i < r->nheaders; i++)
		if (!strcmp(r->names[i], "content-length")) total = _strtoui64(r->values[i], NULL, 10);
	progress(x->d, r->status, total, 0);
	x->keep = r->status == 200;
	return x->keep;  // a status but 200 is no failure: its body isn't kept
}

static bool dl_data(void *ctx, const void *p, size_t n, asuka_error *err) {
	dl_file *x = ctx;
	DWORD wrote = 0;
	if (!WriteFile(x->f, p, (DWORD)n, &wrote, NULL) || wrote != n) {
		asuka_win_fail(err, HRESULT_FROM_WIN32(GetLastError()), "writing the download");
		return false;
	}
	AcquireSRWLockShared(&x->d->lock);
	uint64_t got = x->d->downloaded + n, total = x->d->total;
	int status = x->d->status;
	ReleaseSRWLockShared(&x->d->lock);
	progress(x->d, status, total, got);
	return true;
}

static DWORD WINAPI download_thread(void *p) {
	dl *d = p;
	char *err = NULL;
	// The file to, made or emptied, whatever comes.
	wchar_t *to = wh_wide(d->to, -1);
	HANDLE f = CreateFileW(to, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	free(to);
	if (f == INVALID_HANDLE_VALUE) {
		char *m = wh_win_error(GetLastError());
		err = wh_alloc(strlen(m) + strlen(d->to) + 16);
		sprintf(err, "writing %s: %s", d->to, m);
		free(m);
	} else {
		dl_file x = {d, f, false};
		asuka_error e = {0};
		asuka_request rq = {.url = d->url, .agent = "asuka-updater", .abort = d->abort, .head = dl_head, .data = dl_data, .ctx = &x};
		if (!asuka_fetch_stream(&rq, NULL, &e)) err = wh_strdup(e.message ? e.message : "downloading failed");
		asuka_error_free(&e);
		CloseHandle(f);
	}
	AcquireSRWLockExclusive(&d->lock);
	if (d->cancel) {
		free(err);
		err = wh_strdup("canceled");
	}
	d->error = err;
	d->done = true;
	bool post = want_wake(d);
	ReleaseSRWLockExclusive(&d->lock);
	if (post) wh_post(wake, d);  // the last touch of d: its wake may free it
	return 0;
}

static dsk_json *args_of(const uint8_t *a, size_t n) {
	dsk_json *j = dsk_json_parse((const char *)a, n);
	if (j && j->type != DSK_JOBJ) {
		dsk_json_free(j);
		j = NULL;
	}
	return j;
}

static void m_download(wh_reply *r, const uint8_t *a, size_t n) {
	dsk_json *j = args_of(a, n);
	const char *url = dsk_json_str(j, "url", ""), *to = dsk_json_str(j, "to", "");
	if (!*url || !*to) {
		dsk_json_free(j);
		wh_reply_fail(r, "update.download: bad arguments");
		return;
	}
	dl *d = wh_alloc(sizeof *d);
	d->id = ++next_id;
	if (!d->id) d->id = ++next_id;
	d->url = wh_strdup(url), d->to = wh_strdup(to);
	InitializeSRWLock(&d->lock);
	d->abort = asuka_abort_new();
	dsk_json_free(j);
	d->next = downloads, downloads = d;
	HANDLE t = CreateThread(NULL, 0, download_thread, d, 0, NULL);
	if (!t) {
		forget(d);
		wh_reply_fail(r, "update.download: no thread");
		return;
	}
	CloseHandle(t);
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "id"), dsk_jw_int(&w, d->id);
	dsk_jw_end(&w);
	wh_reply_json(r, &w);
}

static dl *dl_of(const uint8_t *a, size_t n) {
	dsk_json *j = args_of(a, n);
	uint32_t id = (uint32_t)dsk_json_num(j, "id", 0);
	dsk_json_free(j);
	return id ? find_dl(id) : NULL;
}

static void m_next(wh_reply *r, const uint8_t *a, size_t n) {
	dl *d = dl_of(a, n);
	if (!d) {
		wh_reply_fail(r, "no such download");
		return;
	}
	if (d->waiting) wh_reply_fail(d->waiting, "stopped");  // one poll at a time
	d->waiting = NULL;
	if (answer_next(d, r, false)) {
		maybe_free(d);
		return;
	}
	d->waiting = r;
}

static void m_cancel(wh_reply *r, const uint8_t *a, size_t n) {
	dl *d = dl_of(a, n);
	if (d) {
		AcquireSRWLockExclusive(&d->lock);
		d->cancel = true;
		ReleaseSRWLockExclusive(&d->lock);
		asuka_abort_now(d->abort);  // ends what the thread waits on
	}
	wh_reply_str(r, "{}");
}

// ---- Installing: internal/update's Swap, for a folder ----

typedef struct {
	wchar_t **v;
	uint32_t n, cap;
} wlist;

static void wl_push(wlist *l, wchar_t *s) {
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 16;
		l->v = wh_realloc(l->v, l->cap * sizeof *l->v);
	}
	l->v[l->n++] = s;
}

static void wl_free(wlist *l) {
	for (uint32_t i = 0; i < l->n; i++) free(l->v[i]);
	free(l->v);
	*l = (wlist){0};
}

// valid_list_path reports whether p can be in a FileList: a path inside
// the install's root, its entries separated by slashes, each one that
// Windows reads as written (dsk_windows_name: no drive, stream, device,
// backslash or name Windows trims to another, such as ".. "), that isn't
// a FileList itself.
static bool valid_list_path(const char *p, size_t n) {
	if (!n || p[0] == '/' || p[n - 1] == '/') return false;
	size_t at = 0;
	while (at <= n) {
		size_t e = at;
		while (e < n && p[e] != '/') e++;
		size_t l = e - at;
		if (!dsk_windows_name(p + at, l)) return false;
		if (e == n && l == strlen(FILE_LIST) && !memcmp(p + at, FILE_LIST, l)) return false;
		at = e + 1;
	}
	return true;
}

// installed is the units the installed version's FileList names: none
// without one.
static void installed(const wchar_t *root, wlist *out) {
	wchar_t *lp = wjoin(root, L"" FILE_LIST);
	char *lp8 = wh_utf8(lp, -1, NULL);
	free(lp);
	size_t n = 0;
	char *s = dsk_read_file(lp8, &n);
	free(lp8);
	for (size_t at = 0; s && at < n;) {
		size_t e = at;
		while (e < n && s[e] != '\n') e++;
		size_t l = e - at;
		if (l && s[at + l - 1] == '\r') l--;
		if (valid_list_path(s + at, l)) {
			char *u = wh_strndup(s + at, l);
			wl_push(out, wh_wide(u, -1));
			free(u);
		}
		at = e + 1;
	}
	free(s);
}

static int wcmp(const void *a, const void *b) {
	char *x = wh_utf8(*(wchar_t *const *)a, -1, NULL), *y = wh_utf8(*(wchar_t *const *)b, -1, NULL);
	int c = strcmp(x, y);
	free(x), free(y);
	return c;
}

// write_file_list writes the list at file, naming paths (sorted, each
// once, the invalid left out), through a temporary file renamed into
// place.
static bool write_file_list(const wchar_t *file, wlist *paths) {
	wchar_t **v = wh_alloc((paths->n ? paths->n : 1) * sizeof *v);
	memcpy(v, paths->v, paths->n * sizeof *v);
	qsort(v, paths->n, sizeof *v, wcmp);
	ak_buf b = {0};
	for (uint32_t i = 0; i < paths->n; i++) {
		if (i && !wcscmp(v[i], v[i - 1])) continue;
		char *u = wh_utf8(v[i], -1, NULL);
		for (char *c = u; *c; c++)
			if (*c == '\\') *c = '/';
		if (valid_list_path(u, strlen(u))) {
			ak_buf_put(&b, u, strlen(u));
			ak_buf_u8(&b, '\n');
		}
		free(u);
	}
	free(v);
	char *f8 = wh_utf8(file, -1, NULL);
	bool ok = dsk_write_file(f8, b.data, b.len);
	free(f8);
	ak_buf_free(&b);
	return ok;
}

// remove_all removes a file or a directory and what's in it.
static void remove_all(const wchar_t *p) {
	DWORD a = GetFileAttributesW(p);
	if (a == INVALID_FILE_ATTRIBUTES) return;
	if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(p, a & ~FILE_ATTRIBUTE_READONLY);
	if (!(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT)) {
		if (a & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p);
		else DeleteFileW(p);
		return;
	}
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
	RemoveDirectoryW(p);
}

typedef struct {
	wchar_t *from, *to;
} move;

typedef struct {
	move *v;
	uint32_t n, cap;
} moves;

static void moves_push(moves *m, wchar_t *from, wchar_t *to) {
	if (m->n == m->cap) {
		m->cap = m->cap ? m->cap * 2 : 16;
		m->v = wh_realloc(m->v, m->cap * sizeof *m->v);
	}
	m->v[m->n++] = (move){from, to};
}

static void moves_free(moves *m) {
	for (uint32_t i = 0; i < m->n; i++) free(m->v[i].from), free(m->v[i].to);
	free(m->v);
}

static wchar_t *slashes_back(const wchar_t *u) {
	wchar_t *s = _wcsdup(u);
	for (wchar_t *c = s; *c; c++)
		if (*c == '/') *c = '\\';
	return s;
}

// swap puts the update in tree in place of the version installed at root
// (internal/update's Install.Swap, for a Windows folder): false with *err,
// the old version whole again.
static bool swap(const wchar_t *root, const wchar_t *exe, const wchar_t *tree, char **err) {
	wlist units = {0}, inst = {0}, left = {0};
	moves moved = {0}, placed = {0};
	bool ok = false;
	// The units: tree's entries, but its FileList.
	wchar_t *pat = wjoin(tree, L"*");
	WIN32_FIND_DATAW fd;
	HANDLE h = FindFirstFileW(pat, &fd);
	free(pat);
	if (h == INVALID_HANDLE_VALUE) {
		*err = wh_strdup("the update can't be read");
		goto out;
	}
	do {
		if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..") || !wcscmp(fd.cFileName, L"" FILE_LIST)) continue;
		wl_push(&units, _wcsdup(fd.cFileName));
	} while (FindNextFileW(h, &fd));
	FindClose(h);
	if (!units.n) {
		*err = wh_strdup("the update is empty");
		goto out;
	}
	// The running executable's unit: never removed.
	const wchar_t *running = NULL;
	size_t rl = wcslen(root);
	if (!_wcsnicmp(exe, root, rl) && exe[rl] == '\\') running = exe + rl + 1;
	wchar_t suffix[32];
	swprintf(suffix, WH_LEN(suffix), L".old-%lu", GetCurrentProcessId());

#define ASIDE(dst)                                                                                     \
	do {                                                                                               \
		wchar_t *dir_ = dir_of(dst);                                                                   \
		const wchar_t *base_ = wcsrchr(dst, '\\') ? wcsrchr(dst, '\\') + 1 : dst;                      \
		wchar_t *name_ = wh_alloc((wcslen(base_) + wcslen(suffix) + 2) * sizeof(wchar_t));             \
		swprintf(name_, wcslen(base_) + wcslen(suffix) + 2, L".%ls%ls", base_, suffix);                \
		aside = wjoin(dir_, name_);                                                                    \
		free(dir_), free(name_);                                                                       \
		remove_all(aside); /* a leftover of the same pid */                                           \
		if (!MoveFileExW(dst, aside, 0)) {                                                             \
			free(aside);                                                                               \
			aside = NULL;                                                                              \
		}                                                                                              \
	} while (0)

	// The units that the replaced version installed and tree lacks go
	// first; one that can't be moved stays listed, for the next update.
	installed(root, &inst);
	for (uint32_t i = 0; i < inst.n; i++) {
		wchar_t *u = slashes_back(inst.v[i]);
		bool ours = false;
		for (uint32_t k = 0; k < units.n && !ours; k++) ours = !_wcsicmp(units.v[k], u);
		if (ours || (running && !_wcsicmp(u, running))) {
			free(u);
			continue;
		}
		wchar_t *dst = wjoin(root, u);
		if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
			wchar_t *aside;
			ASIDE(dst);
			if (aside) moves_push(&moved, dst, aside), dst = NULL;
			else {
				wh_log("update: leaving %ls, which the update no longer has", dst);
				wl_push(&left, _wcsdup(inst.v[i]));
			}
		}
		free(dst);
		free(u);
	}
	wlist list = {0};
	for (uint32_t i = 0; i < units.n; i++) wl_push(&list, units.v[i]);
	for (uint32_t i = 0; i < left.n; i++) wl_push(&list, left.v[i]);
	wchar_t *lf = wjoin(tree, L"" FILE_LIST);
	bool wrote = write_file_list(lf, &list);
	free(lf);
	free(list.v);  // its strings are units' and left's
	if (!wrote) {
		*err = wh_strdup("writing the update's file list");
		goto undo;
	}
	wl_push(&units, _wcsdup(L"" FILE_LIST));
	for (uint32_t i = 0; i < units.n; i++) {
		wchar_t *src = wjoin(tree, units.v[i]), *dst = wjoin(root, units.v[i]);
		if (GetFileAttributesW(dst) != INVALID_FILE_ATTRIBUTES) {
			wchar_t *aside;
			ASIDE(dst);
			if (!aside) {
				char *m = wh_win_error(GetLastError()), *d8 = wh_utf8(dst, -1, NULL);
				*err = wh_alloc(strlen(m) + strlen(d8) + 32);
				sprintf(*err, "replacing %s: %s", d8, m);
				free(m), free(d8), free(src), free(dst);
				goto undo;
			}
			moves_push(&moved, _wcsdup(dst), aside);
		}
		if (!MoveFileExW(src, dst, 0)) {
			char *m = wh_win_error(GetLastError()), *d8 = wh_utf8(dst, -1, NULL);
			*err = wh_alloc(strlen(m) + strlen(d8) + 32);
			sprintf(*err, "replacing %s: %s", d8, m);
			free(m), free(d8), free(src), free(dst);
			goto undo;
		}
		moves_push(&placed, src, dst);
	}
#undef ASIDE
	// What was put aside goes; the running .exe stays until the next
	// launch (wh_update_clean).
	for (uint32_t i = 0; i < moved.n; i++) remove_all(moved.v[i].to);
	ok = true;
	goto out;
undo:
	for (uint32_t i = placed.n; i-- > 0;)
		if (!MoveFileExW(placed.v[i].to, placed.v[i].from, 0)) remove_all(placed.v[i].to);
	for (uint32_t i = moved.n; i-- > 0;) MoveFileExW(moved.v[i].to, moved.v[i].from, 0);
out:
	wl_free(&units), wl_free(&inst), wl_free(&left);
	moves_free(&moved), moves_free(&placed);
	return ok;
}

static void m_install(wh_reply *r, const uint8_t *a, size_t n) {
	dsk_json *j = args_of(a, n);
	const char *staged = dsk_json_str(j, "staged", "");
	if (!*staged) {
		dsk_json_free(j);
		wh_reply_fail(r, "update.install: bad arguments");
		return;
	}
	wchar_t *given = wh_wide(staged, -1), *exe = exe_path(), *root = dir_of(exe);
	dsk_json_free(j);
	wchar_t *tree = full_path(given);
	free(given);
	// The staged tree is in the install's work directory, the folder (the
	// package's .asuka-update-<id>\<tree>), below it.
	size_t rl = wcslen(root);
	while (rl && (root[rl - 1] == L'\\' || root[rl - 1] == L'/')) rl--;
	bool inside = wcslen(tree) > rl + 1 && !_wcsnicmp(tree, root, rl) && (tree[rl] == L'\\' || tree[rl] == L'/');
	char *err = NULL;
	if (!inside) {
		char *t8 = wh_utf8(tree, -1, NULL), *r8 = wh_utf8(root, -1, NULL);
		wh_reply_fail(r, "update.install: %s isn't in the app's folder (%s)", t8, r8);
		free(t8), free(r8);
	} else if (swap(root, exe, tree, &err)) {
		remove_all(tree);  // what's left of the work directory
		wh_reply_str(r, "{}");
	} else wh_reply_fail(r, "%s", err ? err : "installing the update failed");
	free(err);
	free(tree), free(exe), free(root);
}

static const wh_method_def update_methods[] = {
	{"info", m_info}, {"download", m_download}, {"next", m_next}, {"cancel", m_cancel}, {"install", m_install}, {NULL, NULL},
};

const wh_module_def wh_update_module = {"update", update_methods};

// ---- At launch ----

// wh_relaunch_wait waits for the process a relaunch came from to exit
// (ASUKA_RELAUNCH_WAIT, its pid; 30 seconds at most), before the app
// takes its single instance.
void wh_relaunch_wait(void) {
	wchar_t v[32];
	DWORD n = GetEnvironmentVariableW(L"ASUKA_RELAUNCH_WAIT", v, WH_LEN(v));
	if (!n || n >= WH_LEN(v)) return;
	SetEnvironmentVariableW(L"ASUKA_RELAUNCH_WAIT", NULL);
	DWORD pid = wcstoul(v, NULL, 10);
	HANDLE p = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : NULL;
	if (!p) return;
	WaitForSingleObject(p, 30000);
	CloseHandle(p);
}

// installed_entry keeps the app's entry in Installed apps (its
// installer's: HKCU's Uninstall key named for its ID) saying the version
// that runs, when the entry is the app in dir's (its InstallLocation): an
// update replaced the version the installer wrote there.
static void installed_entry(const wchar_t *dir) {
	const char *version = H.config.version;
	if (!version || !*version || wh_packaged()) return;
	wchar_t *id = wh_wide(wh_app_user_model_id(), -1);
	wchar_t key[600];
	swprintf(key, WH_LEN(key), L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\%ls", id);
	free(id);
	wchar_t *where = wh_reg_string(HKEY_CURRENT_USER, key, L"InstallLocation");
	wchar_t *have = where ? wh_reg_string(HKEY_CURRENT_USER, key, L"DisplayVersion") : NULL;
	wchar_t *want = wh_wide(version, -1);
	if (where && !_wcsicmp(where, dir) && (!have || wcscmp(have, want))) {
		HKEY k;
		if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
			RegSetValueExW(k, L"DisplayVersion", 0, REG_SZ, (const BYTE *)want, (DWORD)((wcslen(want) + 1) * sizeof(wchar_t)));
			RegCloseKey(k);
		}
	}
	free(where), free(have), free(want);
}

// wh_update_clean removes what an update left in the app's folder: the
// .exe and entries it put aside (.*.old-*), and the work directories of
// updates that were stopped (.asuka-update-*); and keeps the app's entry
// in Installed apps saying its version. Once the app runs alone.
void wh_update_clean(void) {
	wchar_t *exe = exe_path(), *dir = dir_of(exe);
	const wchar_t *pats[] = {L".*.old-*", L"" WORK_PREFIX L"*"};
	for (size_t i = 0; i < WH_LEN(pats); i++) {
		wchar_t *pat = wjoin(dir, pats[i]);
		WIN32_FIND_DATAW fd;
		HANDLE h = FindFirstFileW(pat, &fd);
		free(pat);
		if (h == INVALID_HANDLE_VALUE) continue;
		do {
			if (i == 0 && !wcsstr(fd.cFileName, L".old-")) continue;
			wchar_t *p = wjoin(dir, fd.cFileName);
			remove_all(p);
			free(p);
		} while (FindNextFileW(h, &fd));
		FindClose(h);
	}
	installed_entry(dir);
	free(exe), free(dir);
}
