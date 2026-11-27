// update.c: the host's "update" module (DESIGN.md, Updates): what only
// the host can do for platform/update, which is Asuka (the feed, versions,
// verifying, extracting and deltas are the package's and the core's).
// JSON both ways, each method answering once:
//
//   - info: where and how the app is installed. Linux's layout is an
//     install prefix (asuka build's tarball): bin/<exe> and share/<exe>/
//     (its assets and .asuka-files), with the desktop entry, icons,
//     metainfo and MIME types beside other programs'; or an AppImage
//     (kind appimage: the file $APPIMAGE, which the runtime mounted or
//     extracted at $APPDIR, where the executable is). Flatpak, Snap and
//     the system's prefixes are managed; anything else isn't installed as
//     asuka build installs.
//   - download {url, to} → {id}, then next {id}: a long poll of its
//     progress, answered as it moves by 1% (64 KB while the size is
//     unknown) or ends; cancel {id}. libsoup (GIO's proxy, redirects
//     followed, User-Agent asuka-updater).
//   - install {staged}: the units of the new tree (the prefix's files and
//     links) replace the installed ones now, those the old version's
//     .asuka-files names and the new lacks go, what no list names stays
//     (the user's files, other programs of ~/.local); everything is moved
//     aside first and put back if one rename fails; the new .asuka-files
//     goes in with them. Then the desktop's cache tools run. The app
//     finishes it by relaunching (relaunch(): the Quit command's flag),
//     which starts the executable at the path it started from, with
//     ASUKA_RELAUNCH_WAIT.
//   - install {staged} of an AppImage: the new AppImage, which the
//     package made from its delta (or downloaded) beside the running one
//     and verified (.<name>.asuka-update-*; the delta, with .delta after),
//     takes its place by a rename (the running one's mount keeps the old
//     file open), with its permissions. Relaunching starts $APPIMAGE, the new file,
//     extracted again (APPIMAGE_EXTRACT_AND_RUN) when this one was.
//
// At launch what an update left (.asuka-update-* work directories, the
// .*.old-* set aside; an AppImage's .<name>.asuka-update-* downloads)
// goes. internal/update's install.go and files.go at
// bf1839e2 are the Go reference.
#include "host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FILE_LIST ".asuka-files"
#define WORK_PREFIX ".asuka-update-"

// ---- Where the app is ----

static char *launch_exe;  // the executable as the app started: /proc names a replaced one "(deleted)"
static char *appimage;    // the AppImage the app runs from ($APPIMAGE, real), NULL for none
static bool extracted;    // and it was extracted, not mounted (--appimage-extract-and-run)

// relaunch_exe is what relaunching starts: the AppImage, the file an
// update replaced, as the runtime starts it (its environment for this
// launch's mount taken off), else the executable.
static const char *relaunch_exe(void) {
	if (!appimage) return launch_exe;
	const char *dir = getenv("APPDIR");
	const char *data = getenv("XDG_DATA_DIRS");
	if (dir && *dir && data) {
		// AppRun put the mount's share first: the next launch puts its own.
		ak_buf kept = {0};
		gchar **parts = g_strsplit(data, ":", -1);
		size_t n = strlen(dir);
		for (gchar **p = parts; *p; p++) {
			if (!**p || (!strncmp(*p, dir, n) && ((*p)[n] == '/' || !(*p)[n]))) continue;
			if (kept.len) ak_buf_u8(&kept, ':');
			ak_buf_put(&kept, *p, strlen(*p));
		}
		if (kept.len) {
			ak_buf_u8(&kept, 0);
			setenv("XDG_DATA_DIRS", (const char *)kept.data, 1);
		} else {
			unsetenv("XDG_DATA_DIRS");
		}
		g_strfreev(parts);
		ak_buf_free(&kept);
	}
	const char *const gone[] = {"APPIMAGE", "APPDIR", "ARGV0", "OWD"};
	for (size_t i = 0; i < AK_LEN(gone); i++) unsetenv(gone[i]);
	if (extracted) setenv("APPIMAGE_EXTRACT_AND_RUN", "1", 1);
	return appimage;
}

typedef struct {
	char *root, *exe, *work, *list;  // list: the FileList's path in root (a prefix's)
	const char *reason;              // NULL: installable
	bool appimage;                   // root and exe: the AppImage; work: its directory
} install_t;

static void install_free(install_t *in) {
	free(in->root);
	free(in->exe);
	free(in->work);
	free(in->list);
}

static bool is_dir(const char *p) {
	struct stat st;
	return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

// locate finds how the app is installed: reason says why it can't update
// itself.
static install_t locate(void) {
	install_t in = {0};
	const char *env[] = {"FLATPAK_ID", "SNAP"};
	for (size_t i = 0; i < AK_LEN(env); i++) {
		const char *v = getenv(env[i]);
		if (v && *v) in.reason = "managed";
	}
	struct stat st;
	if (stat("/.flatpak-info", &st) == 0) in.reason = "managed";
	if (in.reason) return in;
	if (appimage) {
		// The AppImage file, replaced in its directory.
		in.appimage = true;
		in.root = str_dup(ak_cstr(appimage));
		in.exe = str_dup(ak_cstr(appimage));
		in.work = dsk_dir(appimage);
		return in;
	}
	if (!launch_exe) {
		in.reason = "notInstalled";
		return in;
	}
	char real[PATH_MAX];
	const char *exe = realpath(launch_exe, real) ? real : launch_exe;
	char *bin = dsk_dir(exe), *prefix = dsk_dir(bin);
	const char *name = dsk_base(exe);
	char share_rel[512];
	snprintf(share_rel, sizeof share_rel, "share/%s", name);
	char *share = dsk_join(prefix, share_rel);
	if (strcmp(dsk_base(bin), "bin") || !is_dir(share)) {
		in.reason = "notInstalled";
	} else if (!strcmp(prefix, "/usr") || !strcmp(prefix, "/") || !strcmp(prefix, "/app") || !strcmp(prefix, "/snap")) {
		in.reason = "managed";  // the system's packages
	} else {
		in.root = str_dup(ak_cstr(prefix));
		in.exe = str_dup(ak_cstr(exe));
		in.work = str_dup(ak_cstr(share));
		in.list = g_strdup_printf("share/%s/%s", name, FILE_LIST);
	}
	free(bin);
	free(prefix);
	free(share);
	return in;
}

// writable reports whether an update can be made in work and put in
// place in bin.
static bool writable(const install_t *in) {
	char *bin = dsk_dir(in->exe);
	const char *dirs[] = {in->work, bin};
	bool ok = true;
	for (size_t i = 0; i < AK_LEN(dirs) && ok; i++) {
		char probe[PATH_MAX];
		snprintf(probe, sizeof probe, "%s/.asuka-probe-XXXXXX", dirs[i]);
		ok = mkdtemp(probe) != NULL;
		if (ok) rmdir(probe);
	}
	free(bin);
	return ok;
}

static const char *reason_words(const char *reason) {
	if (!reason) return "";
	if (!strcmp(reason, "notConfigured")) return "the app wasn't built with an update feed and key";
	if (!strcmp(reason, "managed")) return "a package manager or an app store installed the app, and updates it";
	if (!strcmp(reason, "notWritable")) return "the app can't write where it's installed";
	return "the app isn't laid out as asuka build makes it (an install prefix)";
}

// default_target is this build's target, where asuka-app.json names none.
static const char *default_target(void) {
#if defined(__x86_64__)
	return "linux-amd64";
#elif defined(__aarch64__)
	return "linux-arm64";
#elif defined(__riscv) && __riscv_xlen == 64
	return "linux-riscv64";
#else
	return "linux";
#endif
}

static void put(dsk_jw *w, const char *k, const char *v) {
	dsk_jw_key(w, k);
	dsk_jw_str(w, S(v));
}

// info: {installable, reason, message, target, version, name, feed, key,
// kind, root, exe, work}.
static void u_info(ak_span args, reply *r) {
	(void)args;
	install_t in = locate();
	if (str_empty(H.cfg.update_feed) || str_empty(H.cfg.update_key)) in.reason = "notConfigured";
	if (!in.reason && !writable(&in)) in.reason = "notWritable";
	dsk_jw w = {0};
	dsk_jw_obj(&w);
	dsk_jw_key(&w, "installable");
	dsk_jw_bool(&w, !in.reason);
	put(&w, "reason", in.reason);
	put(&w, "message", reason_words(in.reason));
	put(&w, "target", !str_empty(H.cfg.update_target) ? H.cfg.update_target : default_target());
	put(&w, "version", H.cfg.version);
	put(&w, "name", H.app_name);
	put(&w, "feed", H.cfg.update_feed);
	put(&w, "key", H.cfg.update_key);
	put(&w, "kind", in.appimage ? "appimage" : "prefix");
	put(&w, "root", in.root);
	put(&w, "exe", in.exe ? in.exe : launch_exe);
	put(&w, "work", in.work);
	dsk_jw_end(&w);
	size_t n;
	char *json = dsk_jw_take(&w, &n);
	reply_ok(r, json, n);
	free(json);
	install_free(&in);
}

// ---- Downloads ----

typedef struct {
	uint32_t id;
	SoupMessage *msg;
	GCancellable *cancel;
	GInputStream *in;
	int fd;
	int64_t downloaded, total, reported;
	int status;
	bool done;
	char *error;
	reply *waiting;
	uint8_t buf[64 * 1024];
} download_t;

static AK_VEC(download_t *) downloads;
static SoupSession *session;
static uint32_t next_id;

static download_t *download_of(ak_span args) {
	dsk_json *j = dsk_json_parse(args.p, args.len);
	uint32_t id = (uint32_t)dsk_json_num(j, "id", 0);
	dsk_json_free(j);
	for (uint32_t i = 0; i < downloads.len; i++)
		if (downloads.data[i]->id == id) return downloads.data[i];
	return NULL;
}

static void forget(download_t *d) {
	for (uint32_t i = 0; i < downloads.len; i++)
		if (downloads.data[i] == d) {
			downloads.data[i] = downloads.data[--downloads.len];
			break;
		}
	if (d->in) g_object_unref(d->in);
	if (d->msg) g_object_unref(d->msg);
	g_object_unref(d->cancel);
	if (d->fd >= 0) close(d->fd);
	g_free(d->error);
	free(d);
}

// answer answers the next waiting: {downloaded, total, done, error,
// status}; after done the download is forgotten.
static void answer(download_t *d) {
	if (!d->waiting) return;
	reply *w = d->waiting;
	d->waiting = NULL;
	d->reported = d->downloaded;
	dsk_jw j = {0};
	dsk_jw_obj(&j);
	dsk_jw_key(&j, "downloaded");
	dsk_jw_int(&j, d->downloaded);
	dsk_jw_key(&j, "total");
	dsk_jw_int(&j, d->total);
	dsk_jw_key(&j, "done");
	dsk_jw_bool(&j, d->done);
	put(&j, "error", d->error);
	dsk_jw_key(&j, "status");
	dsk_jw_int(&j, d->status);
	dsk_jw_end(&j);
	size_t n;
	char *json = dsk_jw_take(&j, &n);
	reply_ok(w, json, n);
	free(json);
	if (d->done) forget(d);
}

// moved reports whether the progress moved enough to answer: 1% of the
// size, 64 KB while it's unknown.
static bool moved(download_t *d) {
	int64_t step = d->total > 0 ? d->total / 100 : 64 * 1024;
	if (step < 1) step = 1;
	return d->downloaded - d->reported >= step;
}

static void end(download_t *d, const char *err) {
	d->done = true;
	if (err && !d->error) d->error = g_strdup(err);
	if (d->fd >= 0) {
		if (close(d->fd) != 0 && !d->error) d->error = g_strdup(g_strerror(errno));
		d->fd = -1;
	}
	answer(d);
}

static void on_read(GObject *src, GAsyncResult *res, gpointer data);

static void read_more(download_t *d) { g_input_stream_read_async(d->in, d->buf, sizeof d->buf, G_PRIORITY_DEFAULT, d->cancel, on_read, d); }

static void on_read(GObject *src, GAsyncResult *res, gpointer data) {
	download_t *d = data;
	GError *e = NULL;
	gssize n = g_input_stream_read_finish((GInputStream *)src, res, &e);
	if (n < 0) {
		end(d, g_cancellable_is_cancelled(d->cancel) ? "canceled" : e ? e->message : "the download failed");
		if (e) g_error_free(e);
		return;
	}
	if (n == 0) {
		end(d, NULL);
		return;
	}
	for (gssize off = 0; off < n;) {
		ssize_t w = write(d->fd, d->buf + off, (size_t)(n - off));
		if (w < 0) {
			if (errno == EINTR) continue;
			end(d, g_strerror(errno));
			return;
		}
		off += w;
	}
	d->downloaded += n;
	if (moved(d)) answer(d);
	read_more(d);
}

static void on_sent(GObject *src, GAsyncResult *res, gpointer data) {
	download_t *d = data;
	GError *e = NULL;
	d->in = soup_session_send_finish((SoupSession *)src, res, &e);
	if (!d->in) {
		end(d, g_cancellable_is_cancelled(d->cancel) ? "canceled" : e ? e->message : "the download failed");
		if (e) g_error_free(e);
		return;
	}
	d->status = (int)soup_message_get_status(d->msg);
	if (d->status != 200) {  // a body is kept for 200 only
		end(d, NULL);
		return;
	}
	goffset len = soup_message_headers_get_content_length(soup_message_get_response_headers(d->msg));
	d->total = len > 0 ? len : 0;
	answer(d);
	read_more(d);
}

// download: {url, to} → {id}, at once.
static void u_download(ak_span args, reply *r) {
	dsk_json *j = dsk_json_parse(args.p, args.len);
	const char *url = dsk_json_str(j, "url", ""), *to = dsk_json_str(j, "to", "");
	if (!*url || !*to) {
		dsk_json_free(j);
		reply_err(r, "update.download: bad arguments");
		return;
	}
	if (!lx_lib_load(ASUKA_LIB_SOUP) || !soup_session_send_async) {
		dsk_json_free(j);
		reply_err(r, "libsoup 3 isn't installed: updates can't download");
		return;
	}
	if (!session) {
		session = soup_session_new();
		soup_session_set_user_agent(session, "asuka-updater");
	}
	download_t *d = calloc(1, sizeof *d);
	if (!d) abort();
	d->id = ++next_id;
	d->cancel = g_cancellable_new();
	d->fd = open(to, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	ak_vec_push(&downloads, d);
	if (d->fd < 0) {
		d->done = true;
		d->error = g_strdup_printf("open %s: %s", to, g_strerror(errno));
	} else {
		d->msg = soup_message_new("GET", url);
		if (!d->msg) {
			end(d, "not a URL");
		} else {
			soup_session_send_async(session, d->msg, G_PRIORITY_DEFAULT, d->cancel, on_sent, d);
		}
	}
	dsk_json_free(j);
	char out[48];
	int n = snprintf(out, sizeof out, "{\"id\":%u}", (unsigned)d->id);
	reply_ok(r, out, (size_t)n);
}

// next: {id} → its progress, a long poll.
static void u_next(ak_span args, reply *r) {
	download_t *d = download_of(args);
	if (!d) {
		reply_err(r, "no such download");
		return;
	}
	if (d->waiting) reply_err(d->waiting, "stopped");  // one poll at a time
	d->waiting = r;
	if (d->done || moved(d)) answer(d);
}

// cancel: {id}: a next waiting answers done with "canceled".
static void u_cancel(ak_span args, reply *r) {
	download_t *d = download_of(args);
	if (d && !d->done) {
		if (!d->error) d->error = g_strdup("canceled");
		g_cancellable_cancel(d->cancel);
	}
	reply_ok(r, "{}", 2);
}

// ---- Installing ----

typedef AK_VEC(char *) strs;

static void strs_free(strs *s) {
	for (uint32_t i = 0; i < s->len; i++) free(s->data[i]);
	ak_vec_free(s);
}

// valid_list_path reports whether p can be in a FileList: a path inside
// the root, with slashes, that isn't a FileList itself.
static bool valid_list_path(const char *p) {
	if (!*p || !strcmp(p, ".") || p[0] == '/' || strpbrk(p, "\\\r\n")) return false;
	for (const char *s = p;;) {
		const char *e = strchr(s, '/');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.')) return false;
		if (!e) return strcmp(s, FILE_LIST) != 0;
		s = e + 1;
	}
}

// units appends the files and links under dir (rel its path in the tree),
// without FileLists.
static bool walk(const char *dir, const char *rel, strs *out) {
	DIR *d = opendir(dir);
	if (!d) return false;
	struct dirent *e;
	bool ok = true;
	while ((e = readdir(d)) && ok) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		char *p = dsk_join(dir, e->d_name);
		char *r = *rel ? g_strdup_printf("%s/%s", rel, e->d_name) : g_strdup(e->d_name);
		struct stat st;
		if (lstat(p, &st) == 0 && S_ISDIR(st.st_mode))
			ok = walk(p, r, out);
		else if (strcmp(e->d_name, FILE_LIST))
			ak_vec_push(out, str_dup(ak_cstr(r)));
		g_free(r);
		free(p);
	}
	closedir(d);
	return ok;
}

static int by_str(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

// write_list writes a FileList naming paths (sorted, each once; what
// valid_list_path refuses left out), whole, then renamed into place.
static bool write_list(const char *file, strs *paths) {
	if (paths->len > 1) qsort(paths->data, paths->len, sizeof *paths->data, by_str);
	ak_buf b = {0};
	for (uint32_t i = 0; i < paths->len; i++) {
		if (!valid_list_path(paths->data[i]) || (i && !strcmp(paths->data[i], paths->data[i - 1]))) continue;
		ak_buf_put(&b, paths->data[i], strlen(paths->data[i]));
		ak_buf_u8(&b, '\n');
	}
	char *dir = dsk_dir(file);
	bool ok = dsk_mkdirs(dir) && dsk_write_file(file, b.data ? (const char *)b.data : "", b.len);
	free(dir);
	ak_buf_free(&b);
	return ok;
}

static strs installed(const install_t *in) {
	strs out = {0};
	char *p = dsk_join(in->root, in->list);
	size_t n;
	char *text = dsk_read_file(p, &n);
	free(p);
	for (char *line = text ? strtok(text, "\n") : NULL; line; line = strtok(NULL, "\n")) {
		size_t k = strlen(line);
		while (k && line[k - 1] == '\r') line[--k] = 0;
		if (valid_list_path(line)) ak_vec_push(&out, str_dup(ak_cstr(line)));
	}
	free(text);
	return out;
}

static bool has(const strs *s, const char *v) {
	for (uint32_t i = 0; i < s->len; i++)
		if (!strcmp(s->data[i], v)) return true;
	return false;
}

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw) {
	(void)st;
	(void)flag;
	(void)ftw;
	return remove(path);
}

static void rm_rf(const char *p) {
	struct stat st;
	if (lstat(p, &st) != 0) return;
	if (S_ISDIR(st.st_mode))
		nftw(p, remove_one, 16, FTW_DEPTH | FTW_PHYS);
	else
		remove(p);
}

typedef struct {
	char *from, *to;
} renamed;

typedef AK_VEC(renamed) renames;

static void renames_free(renames *r) {
	for (uint32_t i = 0; i < r->len; i++) {
		free(r->data[i].from);
		free(r->data[i].to);
	}
	ak_vec_free(r);
}

// swap puts the update in tree in place of the version installed (see
// the file's comment); false with what went wrong.
static bool swap(const install_t *in, const char *tree, char **err) {
	strs units = {0};
	if (!walk(tree, "", &units) || !units.len) {
		*err = g_strdup(units.len ? "can't read the update" : "the update is empty");
		strs_free(&units);
		return false;
	}
	char suffix[32];
	snprintf(suffix, sizeof suffix, ".old-%d", (int)getpid());
	renames moved = {0}, placed = {0};
	bool ok = true;
	// The running unit: never removed, whatever the new version calls its
	// own (relaunch starts it).
	char *running = NULL;
	char root_real[PATH_MAX];
	const char *root = realpath(in->root, root_real) ? root_real : in->root;
	size_t rl = strlen(root);
	if (!strncmp(in->exe, root, rl) && in->exe[rl] == '/') running = str_dup(ak_cstr(in->exe + rl + 1));
#define MOVE_ASIDE(dst)                                                                   \
	({                                                                                    \
		char *dir_ = dsk_dir(dst);                                                        \
		char *aside_ = g_strdup_printf("%s/.%s%s", dir_, dsk_base(dst), suffix);          \
		free(dir_);                                                                       \
		rm_rf(aside_); /* a leftover of the same pid */                                   \
		bool ok_ = rename(dst, aside_) == 0;                                              \
		if (ok_) ak_vec_push(&moved, ((renamed){str_dup(ak_cstr(dst)), str_dup(ak_cstr(aside_))})); \
		g_free(aside_);                                                                   \
		ok_;                                                                              \
	})
	// The units that the replaced version installed and tree lacks go
	// first. One that can't be moved stays listed, for the next update.
	strs left = {0}, old = installed(in);
	for (uint32_t i = 0; i < old.len; i++) {
		const char *u = old.data[i];
		if (has(&units, u) || (running && !strcmp(u, running))) continue;
		char *dst = dsk_join(in->root, u);
		struct stat st;
		if (lstat(dst, &st) == 0 && !MOVE_ASIDE(dst)) {
			lx_log("update: leaving %s, which the update no longer has: %s", dst, g_strerror(errno));
			ak_vec_push(&left, str_dup(ak_cstr(u)));
		}
		free(dst);
	}
	strs listed = {0};
	for (uint32_t i = 0; i < units.len; i++) ak_vec_push(&listed, str_dup(ak_cstr(units.data[i])));
	for (uint32_t i = 0; i < left.len; i++) ak_vec_push(&listed, str_dup(ak_cstr(left.data[i])));
	char *list = dsk_join(tree, in->list);
	if (!write_list(list, &listed)) {
		*err = g_strdup_printf("can't write %s", list);
		ok = false;
	}
	free(list);
	ak_vec_push(&units, str_dup(ak_cstr(in->list)));
	for (uint32_t i = 0; ok && i < units.len; i++) {
		char *src = dsk_join(tree, units.data[i]), *dst = dsk_join(in->root, units.data[i]), *dir = dsk_dir(dst);
		struct stat st;
		if (lstat(dst, &st) == 0 && !MOVE_ASIDE(dst)) {
			*err = g_strdup_printf("replacing %s: %s", dst, g_strerror(errno));
			ok = false;
		} else if (!dsk_mkdirs(dir)) {
			*err = g_strdup_printf("mkdir %s: %s", dir, g_strerror(errno));
			ok = false;
		} else if (rename(src, dst) != 0) {
			*err = g_strdup_printf("replacing %s: %s", dst, g_strerror(errno));
			ok = false;
		} else {
			ak_vec_push(&placed, ((renamed){str_dup(ak_cstr(src)), str_dup(ak_cstr(dst))}));
		}
		free(src);
		free(dst);
		free(dir);
	}
#undef MOVE_ASIDE
	if (!ok) {
		// Undone: those placed go back, the others are put back.
		for (uint32_t i = placed.len; i-- > 0;)
			if (rename(placed.data[i].to, placed.data[i].from) != 0) rm_rf(placed.data[i].to);
		for (uint32_t i = moved.len; i-- > 0;) rename(moved.data[i].to, moved.data[i].from);
	} else {
		for (uint32_t i = 0; i < moved.len; i++) rm_rf(moved.data[i].to);
	}
	free(running);
	renames_free(&moved);
	renames_free(&placed);
	strs_free(&units);
	strs_free(&left);
	strs_free(&old);
	strs_free(&listed);
	return ok;
}

// refresh_caches runs the desktop's cache tools on the prefix: its
// desktop entries, MIME types and icons (each if it's there).
static void refresh_caches(const char *root) {
	char *apps = dsk_join(root, "share/applications"), *mime = dsk_join(root, "share/mime"), *icons = dsk_join(root, "share/icons/hicolor");
	const char *const a[] = {"update-desktop-database", "-q", apps, NULL};
	const char *const m[] = {"update-mime-database", mime, NULL};
	const char *const i[] = {"gtk4-update-icon-cache", "-q", "-t", "-f", icons, NULL};
	const char *const *cmds[] = {a, m, i};
	const char *dirs[] = {apps, mime, icons};
	for (size_t k = 0; k < AK_LEN(cmds); k++) {
		gchar *exe = g_find_program_in_path(cmds[k][0]);
		if (exe && is_dir(dirs[k])) {
			GSubprocess *p = g_subprocess_newv(cmds[k], G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL);
			if (p) g_object_unref(p);  // it runs on; GLib reaps it
		}
		g_free(exe);
	}
	free(apps);
	free(mime);
	free(icons);
}

// staged_name reports whether name is a download of an update of the
// AppImage named base: .<base>.asuka-update-<id>.
static bool staged_name(const char *name, const char *base) {
	size_t n = strlen(base);
	return name[0] == '.' && !strncmp(name + 1, base, n) && g_str_has_prefix(name + 1 + n, WORK_PREFIX);
}

// swap_appimage puts the AppImage staged (beside it, verified) in the
// running one's place: executable as the old one is, written out, then
// renamed over it; false with what went wrong.
static bool swap_appimage(const install_t *in, const char *staged, char **err) {
	struct stat old, st;
	char *dir = dsk_dir(staged);
	bool beside = !strcmp(dir, in->work) && staged_name(dsk_base(staged), dsk_base(in->root));
	free(dir);
	if (!beside || lstat(staged, &st) != 0 || !S_ISREG(st.st_mode)) {
		*err = g_strdup("update.install: the staged AppImage isn't beside the running one");
		return false;
	}
	// An AppImage: an ELF whose header says AI, type 2.
	unsigned char head[11] = {0};
	int fd = open(staged, O_RDWR | O_CLOEXEC);
	bool ok = fd >= 0 && pread(fd, head, sizeof head, 0) == (ssize_t)sizeof head && !memcmp(head, "\x7f" "ELF", 4) && !memcmp(head + 8, "AI\x02", 3);
	if (!ok) {
		*err = g_strdup(fd < 0 ? g_strerror(errno) : "the update isn't an AppImage");
	} else {
		mode_t mode = (stat(in->root, &old) == 0 ? old.st_mode : 0755) & 07777;
		if (fchmod(fd, mode | 0100) != 0 || fsync(fd) != 0) {
			*err = g_strdup_printf("%s: %s", staged, g_strerror(errno));
			ok = false;
		}
	}
	if (fd >= 0) close(fd);
	if (ok && rename(staged, in->root) != 0) {
		*err = g_strdup_printf("replacing %s: %s", in->root, g_strerror(errno));
		ok = false;
	}
	if (ok) {
		int d = open(in->work, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (d >= 0) {
			fsync(d);  // the rename, kept
			close(d);
		}
	}
	return ok;
}

// install: {staged}: a directory in work, the new version's prefix tree,
// which the package has verified; what's left of it goes once it's in
// place. An AppImage's: the new AppImage file.
static void u_install(ak_span args, reply *r) {
	dsk_json *j = dsk_json_parse(args.p, args.len);
	const char *staged = dsk_json_str(j, "staged", "");
	install_t in = locate();
	char *err = NULL;
	char real[PATH_MAX], work[PATH_MAX];
	if (in.reason) {
		err = g_strdup(reason_words(in.reason));
	} else if (in.appimage) {
		if (swap_appimage(&in, staged, &err)) lx_log("update: %s replaced", in.root);
	} else if (!*staged || !realpath(staged, real) || !realpath(in.work, work) || strncmp(real, work, strlen(work)) || real[strlen(work)] != '/') {
		err = g_strdup("update.install: the staged update isn't in the app's work directory");
	} else if (swap(&in, real, &err)) {
		rm_rf(real);  // what's left of it: its emptied directories
		refresh_caches(in.root);
	}
	if (err)
		reply_err(r, "%s", err);
	else
		reply_ok(r, "{}", 2);
	g_free(err);
	install_free(&in);
	dsk_json_free(j);
}

// clean_leftovers removes what an update put aside and work directories
// of updates that were stopped (the app quit while one downloaded).
static void clean_leftovers(void) {
	install_t in = locate();
	if (in.reason) {
		install_free(&in);
		return;
	}
	if (in.appimage) {
		// Its downloads that weren't put in place (the app quit first);
		// other AppImages' beside it stay.
		DIR *d = opendir(in.work);
		struct dirent *e;
		strs gone = {0};
		while (d && (e = readdir(d)))
			if (staged_name(e->d_name, dsk_base(in.root))) ak_vec_push(&gone, dsk_join(in.work, e->d_name));
		if (d) closedir(d);
		for (uint32_t k = 0; k < gone.len; k++) rm_rf(gone.data[k]);
		strs_free(&gone);
		install_free(&in);
		return;
	}
	char *bin = dsk_dir(in.exe);
	const char *dirs[] = {bin, in.root, in.work};
	for (size_t i = 0; i < AK_LEN(dirs); i++) {
		DIR *d = opendir(dirs[i]);
		if (!d) continue;
		struct dirent *e;
		strs gone = {0};
		while ((e = readdir(d)))
			if ((e->d_name[0] == '.' && strstr(e->d_name, ".old-")) || g_str_has_prefix(e->d_name, WORK_PREFIX)) ak_vec_push(&gone, dsk_join(dirs[i], e->d_name));
		closedir(d);
		for (uint32_t k = 0; k < gone.len; k++) rm_rf(gone.data[k]);
		strs_free(&gone);
	}
	free(bin);
	install_free(&in);
}

// find_appimage finds the AppImage the app runs from: $APPIMAGE, a file,
// when the executable is in $APPDIR, where the runtime mounted or
// extracted it (another AppImage's environment, which a program it
// started inherits, isn't this app's).
static void find_appimage(void) {
	const char *file = getenv("APPIMAGE"), *dir = getenv("APPDIR");
	if (!file || !*file || !dir || !*dir || !launch_exe) return;
	char f[PATH_MAX], d[PATH_MAX], exe[PATH_MAX];
	struct stat st, dst, parent;
	if (!realpath(file, f) || !realpath(dir, d) || !realpath(launch_exe, exe)) return;
	size_t n = strlen(d);
	if (strncmp(exe, d, n) || exe[n] != '/' || stat(f, &st) != 0 || !S_ISREG(st.st_mode)) return;
	appimage = str_dup(ak_cstr(f));
	// Mounted, the runtime's directory is another file system's root;
	// extracted, a directory of the temporary one's.
	char *up = dsk_dir(d);
	extracted = stat(d, &dst) == 0 && stat(up, &parent) == 0 && dst.st_dev == parent.st_dev;
	free(up);
}

// lx_update_start notes the executable as the app starts (what relaunch
// starts, an update having replaced the file /proc names), waits for the
// process that relaunched it (ASUKA_RELAUNCH_WAIT) to end, and removes
// what an update left.
void lx_update_start(void) {
	launch_exe = dsk_exe_path();
	if (launch_exe) {
		size_t n = strlen(launch_exe);
		const char *del = " (deleted)";
		if (n > strlen(del) && !strcmp(launch_exe + n - strlen(del), del)) launch_exe[n - strlen(del)] = 0;
	}
	find_appimage();
	lx_relaunch_exe = relaunch_exe;
	const char *wait = getenv("ASUKA_RELAUNCH_WAIT");
	if (wait && *wait) {
		pid_t pid = (pid_t)atoi(wait);
		unsetenv("ASUKA_RELAUNCH_WAIT");
		for (int i = 0; pid > 0 && i < 300 && (kill(pid, 0) == 0 || errno == EPERM); i++) {  // 30 s at most
			struct timespec ts = {0, 100000000};
			nanosleep(&ts, NULL);
		}
	}
	clean_leftovers();
}

static const method_t update_methods[] = {
	{"update", "info", u_info},
	{"update", "download", u_download},
	{"update", "next", u_next},
	{"update", "cancel", u_cancel},
	{"update", "install", u_install},
};

static void update_modules(void) { lx_register_methods(update_methods, AK_LEN(update_methods)); }

const area lx_area_update = {
	.name = "update",
	.modules = update_modules,
};
