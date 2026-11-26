// update.c: platform/update's builtins, the updater's work over files that
// Asuka shouldn't do itself: checking a download's signature, unpacking
// an archive, and making a new version of the app from a delta. They
// block, so the package calls them in spawn { }; each is safe on any
// thread. The formats are what the CLI writes (internal/update: archive
// and delta writing, bsdiff.go's Diff), read here as delta.go and
// bsdiff.go read them.
//
//	update.verifyFile(path, publicKey, signature) -> bool
//	    the Ed25519 signature of the file's SHA-256, streamed
//	update.extract(archive, dir) -> bool
//	    a gzipped tar into dir: files, directories and links, inside dir
//	update.deltaIndex(delta) -> string?    the delta's index (JSON)
//	update.deltaStart(delta) -> int        where its files' data starts
//	update.deltaFile(delta, offset, length, oldDir, from, newDir, path,
//	                 size, sha256, mode) -> bool
//	    one file of the new app: its data inflated (no from), from's
//	    copied (no data) or patched (bsdiff), checked against its size
//	    and SHA-256 (hex)
//	update.makeDir(root, path) / makeLink(root, path, target) -> bool
//	update.validPath(path) -> bool         a path inside a tree: a/b, no ..
//	update.failure() -> string             why the last call failed
//
// What they make goes in a directory the package made, never through a
// link: a path whose parent is a link (or a file) is refused, so that an
// archive can't write outside it whatever its links say. A link's
// target, read from where it is, stays inside too.
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif
#include "std/std.h"
#include "abi/ota.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#define STR(i) ((ak_str *)args[i].p)
#define BYTES(i) ((ak_bytes *)args[i].p)

static AK_TLS char why[320];

static void failf(const char *fmt, ...) AK_PRINTF(1, 2);
static void failf(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(why, sizeof why, fmt, ap);
	va_end(ap);
}

static ak_val failure_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(why)};
}

// update.otaInfo() -> string: a phone release's over-the-air updates
// (RFC 0006), a line each: the build's fingerprint ("" : it takes none),
// the feed, the public key, the version of the update running ("": the
// build's code), the app's version, and when the build was made.
static ak_val ota_info_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	size_t len = strlen(ak_link_fingerprint) + strlen(ak_link_update_feed) + strlen(ak_link_update_key) + strlen(ak_ota_running()) +
	             strlen(ak_link_update_version) + 40;
	char *s = malloc(len);
	snprintf(s, len, "%s\n%s\n%s\n%s\n%s\n%lld", ak_link_fingerprint, ak_link_update_feed, ak_link_update_key, ak_ota_running(),
	         ak_link_update_version, ak_link_update_built);
	ak_val v = {.p = ak_str_c(s)};
	free(s);
	return v;
}

// update.otaRelaunch(): runs the app again on the next turn, the update
// in place if there's one this build takes (abi/ota.c).
static ak_val ota_relaunch_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	ak_ota_relaunch();
	return (ak_val){0};
}

// ---- Paths ----

// ak_update_valid_path reports whether p is a path inside a tree, as Go's
// fs.ValidPath says (names separated by slashes, none empty, "." or
// ".."), without backslashes (nor, on Windows, colons): "." isn't.
bool ak_update_valid_path(const char *p, size_t n) {
	if (n == 0 || p[0] == '/' || p[n - 1] == '/') return false;
	size_t start = 0;
	for (size_t i = 0; i <= n; i++) {
		if (i < n && p[i] != '/') {
			if (p[i] == '\\' || p[i] == 0) return false;
#if defined(_WIN32)
			if (p[i] == ':') return false;
#endif
			continue;
		}
		size_t len = i - start;
		if (len == 0 || (len == 1 && p[start] == '.') || (len == 2 && p[start] == '.' && p[start + 1] == '.')) return false;
		start = i + 1;
	}
	return true;
}

static char *join(const char *root, const char *rel) {
	size_t a = strlen(root), b = strlen(rel);
	char *p = ak_alloc(a + b + 2);
	memcpy(p, root, a);
	size_t k = a;
	if (k && root[k - 1] != '/' && root[k - 1] != '\\') p[k++] = '/';
	memcpy(p + k, rel, b + 1);
	return p;
}

enum { K_NONE, K_FILE, K_DIR, K_LINK, K_OTHER };

#if defined(_WIN32)
static wchar_t *wide(const char *s) {
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	if (n <= 0) return NULL;
	wchar_t *w = ak_alloc(sizeof(wchar_t) * (size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}
#endif

// kind_of is what's at path, links not followed; size, a file's.
static int kind_of(const char *path, int64_t *size) {
#if defined(_WIN32)
	wchar_t *w = wide(path);
	if (!w) return K_NONE;
	WIN32_FILE_ATTRIBUTE_DATA d;
	BOOL ok = GetFileAttributesExW(w, GetFileExInfoStandard, &d);
	ak_free(w);
	if (!ok) return K_NONE;
	if (size) *size = (int64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow;
	if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return K_LINK;
	return d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? K_DIR : K_FILE;
#else
	struct stat st;
	if (lstat(path, &st) != 0) return K_NONE;
	if (size) *size = st.st_size;
	if (S_ISLNK(st.st_mode)) return K_LINK;
	if (S_ISDIR(st.st_mode)) return K_DIR;
	return S_ISREG(st.st_mode) ? K_FILE : K_OTHER;
#endif
}

static bool make_dir(const char *path) {
#if defined(_WIN32)
	wchar_t *w = wide(path);
	BOOL ok = w && CreateDirectoryW(w, NULL);
	ak_free(w);
	return ok || kind_of(path, NULL) == K_DIR;
#else
	return mkdir(path, 0755) == 0 || (errno == EEXIST && kind_of(path, NULL) == K_DIR);
#endif
}

// dirs_under makes the directories of rel under root (the last name too
// when last), refusing a name that's a link or a file.
static bool dirs_under(const char *root, const char *rel, bool last) {
	char *p = join(root, rel);
	size_t base = strlen(p) - strlen(rel);
	bool ok = true;
	for (size_t i = base;; i++) {
		char c = p[i];
		if (c != '/' && c != 0) continue;
		if (c == 0 && !last) break;
		p[i] = 0;
		int k = kind_of(p, NULL);
		if (k == K_NONE) {
			if (!make_dir(p)) {
				failf("%s: can't be made", p);
				ok = false;
			}
		} else if (k != K_DIR) {
			failf("%s: %s", p, k == K_LINK ? "a link, which the update doesn't write through" : "not a directory");
			ok = false;
		}
		p[i] = c;
		if (!ok || c == 0) break;
	}
	ak_free(p);
	return ok;
}

// no_links reports whether rel's parents under root are directories,
// none a link.
static bool no_links(const char *root, const char *rel) {
	char *p = join(root, rel);
	size_t base = strlen(p) - strlen(rel);
	bool ok = true;
	for (size_t i = base; p[i] && ok; i++) {
		if (p[i] != '/') continue;
		p[i] = 0;
		int k = kind_of(p, NULL);
		if (k != K_DIR) {
			failf("%s: %s", p, k == K_LINK ? "a link, which the update doesn't read through" : "not a directory");
			ok = false;
		}
		p[i] = '/';
	}
	ak_free(p);
	return ok;
}

// create makes the file at path for writing (a new one when excl), its
// permissions mode once written (set_mode); owner-writable meanwhile.
static FILE *create(const char *path, unsigned mode, bool excl) {
#if defined(_WIN32)
	AK_UNUSED(mode);
	if (excl && kind_of(path, NULL) != K_NONE) {
		failf("%s: already exists", path);
		return NULL;
	}
	FILE *f = ak_update_fopen(path, "wb");
#else
	int fd = open(path, O_CREAT | O_WRONLY | (excl ? O_EXCL : O_TRUNC) | O_NOFOLLOW, (mode & 0777) | 0200);
	FILE *f = fd < 0 ? NULL : fdopen(fd, "wb");
	if (fd >= 0 && !f) close(fd);
#endif
	if (!f) failf("%s: can't be written", path);
	return f;
}

static void set_mode(const char *path, unsigned mode) {
#if defined(_WIN32) || defined(__wasm__)
	AK_UNUSED(path), AK_UNUSED(mode);
#else
	chmod(path, mode & 0777);
#endif
}

// link_inside reports whether a link at rel to target stays in its tree,
// read from where it is.
static bool link_inside(const char *rel, const char *target) {
	size_t tn = strlen(target);
	if (tn == 0 || target[0] == '/' || strchr(target, '\\')) return false;
#if defined(_WIN32)
	if (strchr(target, ':')) return false;
#endif
	// The depth of rel's directory, then each name of target.
	int depth = 0;
	for (const char *c = rel; *c; c++) depth += *c == '/';
	for (size_t i = 0, start = 0; i <= tn; i++) {
		if (i < tn && target[i] != '/') continue;
		size_t len = i - start;
		if (len == 2 && target[start] == '.' && target[start + 1] == '.') {
			if (--depth < 0) return false;
		} else if (len && !(len == 1 && target[start] == '.')) {
			depth++;
		}
		start = i + 1;
	}
	return depth >= 0;
}

static bool make_link(const char *target, const char *path) {
#if defined(_WIN32)
	wchar_t *t = wide(target), *p = wide(path);
	for (wchar_t *c = t; c && *c; c++)
		if (*c == L'/') *c = L'\\';
	BOOL ok = t && p && CreateSymbolicLinkW(p, t, 0x2 /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE */);
	ak_free(t);
	ak_free(p);
	if (!ok) failf("%s: the link can't be made", path);
	return ok;
#else
	if (symlink(target, path) != 0) {
		failf("%s: the link can't be made", path);
		return false;
	}
	return true;
#endif
}

static ak_val valid_path_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return (ak_val){.i = ak_update_valid_path(STR(0)->data, STR(0)->len)};
}

static ak_val make_dir_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const char *rel = STR(1)->data;
	if (!ak_update_valid_path(rel, STR(1)->len)) {
		failf("%s: not a path inside the update", rel);
		return (ak_val){.i = 0};
	}
	return (ak_val){.i = dirs_under(STR(0)->data, rel, true)};
}

static ak_val make_link_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const char *root = STR(0)->data, *rel = STR(1)->data, *target = STR(2)->data;
	if (!ak_update_valid_path(rel, STR(1)->len)) {
		failf("%s: not a path inside the update", rel);
		return (ak_val){.i = 0};
	}
	if (!link_inside(rel, target)) {
		failf("the update's link %s points outside the app", rel);
		return (ak_val){.i = 0};
	}
	if (!dirs_under(root, rel, false)) return (ak_val){.i = 0};
	char *p = join(root, rel);
	bool ok = make_link(target, p);
	ak_free(p);
	return (ak_val){.i = ok};
}

// ---- Signatures ----

// verifyFile(path, publicKey, signature): the file's SHA-256, streamed,
// signed with the key (Ed25519), as `asuka build` signs an archive.
static ak_val verify_file_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_bytes *key = BYTES(1), *sig = BYTES(2);
	const char *path = STR(0)->data;
	if (!key || key->len != 32) {
		failf("the app's update key isn't an Ed25519 public key");
		return (ak_val){.i = 0};
	}
	FILE *f = ak_update_fopen(path, "rb");
	if (!f) {
		failf("%s: can't be read", path);
		return (ak_val){.i = 0};
	}
	ak_sha256_ctx c;
	ak_sha256_init(&c);
	uint8_t *buf = ak_alloc(65536), digest[32];
	size_t got;
	while ((got = fread(buf, 1, 65536, f)) > 0) ak_sha256_update(&c, buf, got);
	bool read_err = ferror(f) != 0;
	fclose(f);
	ak_free(buf);
	if (read_err) {
		failf("%s: can't be read", path);
		return (ak_val){.i = 0};
	}
	ak_sha256_final(&c, digest);
	if (!sig || sig->len != 64 || !ak_ed25519_verify(key->data, digest, 32, sig->data)) {
		failf("the update is not signed with the app's key");
		return (ak_val){.i = 0};
	}
	return (ak_val){.i = 1};
}

// ---- Archives: a gzipped tar (POSIX ustar, with PAX and GNU long names) ----

static bool read_full(ak_inflate *z, uint8_t *p, size_t n) {
	while (n) {
		size_t got = ak_inflate_read(z, p, n);
		if (got == 0) return false;
		p += got, n -= got;
	}
	return true;
}

static bool skip(ak_inflate *z, int64_t n) {
	uint8_t buf[4096];
	while (n > 0) {
		size_t k = n < (int64_t)sizeof buf ? (size_t)n : sizeof buf;
		if (!read_full(z, buf, k)) return false;
		n -= (int64_t)k;
	}
	return true;
}

// octal reads a header's number: octal digits, or base 256 (GNU's) when
// its first byte's high bit is set; -1 when it's neither.
static int64_t octal(const uint8_t *p, size_t n) {
	if (p[0] & 0x80) {
		int64_t v = p[0] & 0x3f;
		for (size_t i = 1; i < n; i++) {
			if (v > (INT64_MAX >> 8)) return -1;
			v = v << 8 | p[i];
		}
		return v;
	}
	size_t i = 0;
	while (i < n && p[i] == ' ') i++;
	int64_t v = 0;
	bool any = false;
	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
		if (v > (INT64_MAX >> 3)) return -1;
		v = v * 8 + (p[i] - '0');
		any = true;
	}
	for (; i < n; i++)
		if (p[i] != ' ' && p[i] != 0) return -1;
	return any ? v : 0;
}

static void field(ak_buf *b, const uint8_t *p, size_t n) {
	size_t k = 0;
	while (k < n && p[k]) k++;
	b->len = 0;
	ak_buf_put(b, p, k);
}

// pax reads a PAX header's records ("<length> <key>=<value>\n") into the
// next entry's path and link.
static bool pax(const uint8_t *p, size_t n, ak_buf *path, ak_buf *link, int64_t *size) {
	size_t i = 0;
	while (i < n) {
		size_t len = 0, j = i;
		while (j < n && p[j] >= '0' && p[j] <= '9' && len <= n) len = len * 10 + (size_t)(p[j++] - '0');
		if (j >= n || p[j] != ' ' || len <= j - i || i + len > n || p[i + len - 1] != '\n') return false;
		const uint8_t *kv = p + j + 1, *end = p + i + len - 1;
		const uint8_t *eq = memchr(kv, '=', (size_t)(end - kv));
		if (!eq) return false;
		size_t kn = (size_t)(eq - kv), vn = (size_t)(end - eq - 1);
		if (kn == 4 && memcmp(kv, "path", 4) == 0) {
			path->len = 0;
			ak_buf_put(path, eq + 1, vn);
		} else if (kn == 8 && memcmp(kv, "linkpath", 8) == 0) {
			link->len = 0;
			ak_buf_put(link, eq + 1, vn);
		} else if (kn == 4 && memcmp(kv, "size", 4) == 0) {
			char num[32];
			if (vn == 0 || vn >= sizeof num) return false;
			memcpy(num, eq + 1, vn);
			num[vn] = 0;
			char *stop;
			errno = 0;
			long long v = strtoll(num, &stop, 10);
			if (*stop || v < 0 || errno == ERANGE) return false;
			*size = v;
		}
		i += len;
	}
	return true;
}

// name_of makes a tar name a path inside the tree: without "./" before
// it, nor the slash after a directory's; NULL when it isn't one.
static const char *name_of(ak_buf *b) {
	ak_buf_put(b, "", 1);  // NUL-terminated
	b->len--;
	char *s = (char *)b->data;
	size_t n = b->len;
	while (n >= 2 && s[0] == '.' && s[1] == '/') s += 2, n -= 2;
	while (n && s[n - 1] == '/') n--;
	s[n] = 0;
	return ak_update_valid_path(s, n) ? s : NULL;
}

static bool copy_out(ak_inflate *z, FILE *f, int64_t n, const char *name) {
	uint8_t buf[16384];
	while (n > 0) {
		size_t k = n < (int64_t)sizeof buf ? (size_t)n : sizeof buf;
		if (!read_full(z, buf, k)) {
			failf("the update's archive is cut short (in %s)", name);
			return false;
		}
		if (fwrite(buf, 1, k, f) != k) {
			failf("%s: can't be written", name);
			return false;
		}
		n -= (int64_t)k;
	}
	return true;
}

static bool extract(const char *archive, const char *dir) {
	if (!make_dir(dir)) {
		failf("%s: can't be made", dir);
		return false;
	}
	ak_inflate *z = ak_gunzip_open(archive);
	ak_buf longname = {0}, longlink = {0}, name = {0}, link = {0}, ext = {0};
	bool have_name = false, have_link = false, ok = false;
	int64_t ext_size = -1;
	uint8_t h[512];
	if (ak_inflate_error(z)) {
		failf("%s", ak_inflate_error(z));
		goto done;
	}
	for (;;) {
		if (!read_full(z, h, 512)) {
			failf("the update's archive is cut short");
			goto done;
		}
		bool zero = true;
		for (int i = 0; i < 512 && zero; i++) zero = h[i] == 0;
		if (zero) break;  // the end: what's left is padding
		unsigned sum = 0;
		for (int i = 0; i < 512; i++) sum += i >= 148 && i < 156 ? ' ' : h[i];
		int64_t want = octal(h + 148, 8);
		if (want < 0 || (unsigned)want != sum) {
			failf("the update's archive is damaged (a header's checksum)");
			goto done;
		}
		int64_t size = octal(h + 124, 12);
		int64_t mode64 = octal(h + 100, 8);
		unsigned mode = (unsigned)mode64 & 0777;
		char type = (char)h[156];
		if (size < 0 || mode64 < 0) {
			failf("the update's archive is damaged (a size or a mode)");
			goto done;
		}
		if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
			if (size > (1 << 20)) {
				failf("the update's archive has a header too large");
				goto done;
			}
			ext.len = 0;
			ak_buf_reserve(&ext, (size_t)size + 1);
			if (!read_full(z, ext.data, (size_t)size) || !skip(z, (512 - size % 512) % 512)) {
				failf("the update's archive is cut short");
				goto done;
			}
			ext.len = (size_t)size;
			if (type == 'x') {
				if (!pax(ext.data, ext.len, &longname, &longlink, &ext_size)) {
					failf("the update's archive is damaged (a PAX header)");
					goto done;
				}
				have_name = have_name || longname.len > 0;
				have_link = have_link || longlink.len > 0;
			} else if (type == 'L') {
				field(&longname, ext.data, ext.len);
				have_name = true;
			} else if (type == 'K') {
				field(&longlink, ext.data, ext.len);
				have_link = true;
			}
			continue;
		}
		if (ext_size >= 0) size = ext_size;
		if (size > INT64_MAX - 511) {
			// Padded to its 512 bytes, past int64: no archive holds it.
			failf("the update's archive is damaged (a size)");
			goto done;
		}
		if (have_name) {
			name.len = 0;
			ak_buf_put(&name, longname.data, longname.len);
		} else {
			// ustar: the prefix, a slash, the name.
			ak_buf part = {0};
			field(&name, h, 100);
			if (memcmp(h + 257, "ustar\0", 6) == 0 && h[345]) {
				field(&part, h + 345, 155);
				ak_buf_put(&part, "/", 1);
				ak_buf_put(&part, name.data, name.len);
				name.len = 0;
				ak_buf_put(&name, part.data, part.len);
			}
			ak_buf_free(&part);
		}
		if (have_link) {
			link.len = 0;
			ak_buf_put(&link, longlink.data, longlink.len);
		} else {
			field(&link, h + 157, 100);
		}
		have_name = have_link = false;
		longname.len = longlink.len = 0;
		ext_size = -1;
		ak_buf raw = {0};
		ak_buf_put(&raw, name.data, name.len);
		const char *rel = name_of(&name);
		if (!rel) {
			ak_buf_put(&raw, "", 1);
			failf("the update's archive has a path outside the app: %s", (char *)raw.data);
			ak_buf_free(&raw);
			goto done;
		}
		ak_buf_free(&raw);
		char *path = join(dir, rel);
		bool entry_ok = true;
		switch (type) {
		case '5':
			entry_ok = dirs_under(dir, rel, true) && skip(z, size + (512 - size % 512) % 512);
			break;
		case '0':
		case 0: {
			entry_ok = dirs_under(dir, rel, false);
			FILE *f = entry_ok ? create(path, mode, false) : NULL;
			entry_ok = f != NULL;
			if (f) {
				entry_ok = copy_out(z, f, size, rel);
				if (fclose(f) != 0 && entry_ok) {
					failf("%s: can't be written", rel);
					entry_ok = false;
				}
				if (entry_ok) set_mode(path, mode);
			}
			entry_ok = entry_ok && skip(z, (512 - size % 512) % 512);
			break;
		}
		case '2':
			ak_buf_put(&link, "", 1);
			if (!link_inside(rel, (char *)link.data)) {
				failf("the update's archive has a link pointing outside the app: %s", rel);
				entry_ok = false;
				break;
			}
			entry_ok = dirs_under(dir, rel, false) && make_link((char *)link.data, path) && skip(z, size + (512 - size % 512) % 512);
			break;
		default:
			failf("the update's archive holds what isn't a file, directory or link: %s", rel);
			entry_ok = false;
		}
		ak_free(path);
		if (!entry_ok) goto done;
	}
	// Read to the end, so that gzip's checksum is checked.
	uint8_t rest[4096];
	while (ak_inflate_read(z, rest, sizeof rest) > 0) {}
	if (ak_inflate_error(z)) {
		failf("the update's archive: %s", ak_inflate_error(z));
		goto done;
	}
	ok = true;
done:
	// The decompression's own failure says best why the archive ended.
	if (!ok && ak_inflate_error(z) && strstr(why, "cut short")) failf("the update's archive: %s", ak_inflate_error(z));
	ak_inflate_close(z);
	ak_buf *bs[] = {&longname, &longlink, &name, &link, &ext};
	for (size_t i = 0; i < AK_LEN(bs); i++) ak_buf_free(bs[i]);
	return ok;
}

static ak_val extract_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	why[0] = 0;
	return (ak_val){.i = extract(STR(0)->data, STR(1)->data)};
}

// ---- Deltas (delta.go) ----

static const char magic[] = "asuka delta 1\n";
enum { MAGIC_LEN = sizeof magic - 1, MAX_INDEX = 16 << 20 };

// uvarint decodes Go's binary.Uvarint from p (n bytes); the bytes it
// takes, 0 when it's cut short or overflows.
static size_t uvarint(const uint8_t *p, size_t n, uint64_t *v) {
	uint64_t x = 0;
	for (size_t i = 0; i < n && i < 10; i++) {
		if (i == 9 && p[i] > 1) return 0;
		x |= (uint64_t)(p[i] & 0x7f) << (7 * i);
		if (p[i] < 0x80) {
			*v = x;
			return i + 1;
		}
	}
	return 0;
}

// delta_head reads a delta's head: where its index is (start) and how
// long, and its size.
static bool delta_head(const char *path, int64_t *start, int64_t *index_len, int64_t *size) {
	int64_t sz;
	if (kind_of(path, &sz) != K_FILE) {
		failf("%s: no such file", path);
		return false;
	}
	FILE *f = ak_update_fopen(path, "rb");
	if (!f) {
		failf("%s: can't be read", path);
		return false;
	}
	uint8_t head[MAGIC_LEN + 10];
	size_t got = fread(head, 1, sizeof head, f);
	fclose(f);
	uint64_t len;
	size_t k;
	if (got < MAGIC_LEN || memcmp(head, magic, MAGIC_LEN) != 0) {
		failf("not a delta update");
		return false;
	}
	if (!(k = uvarint(head + MAGIC_LEN, got - MAGIC_LEN, &len)) || len > (uint64_t)(sz - MAGIC_LEN - (int64_t)k)) {
		failf("damaged delta update");
		return false;
	}
	*start = MAGIC_LEN + (int64_t)k;
	*index_len = (int64_t)len;
	*size = sz;
	return true;
}

static ak_val delta_index_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t start, len, size;
	if (!delta_head(STR(0)->data, &start, &len, &size)) return (ak_val){.p = NULL};
	ak_inflate *z = ak_inflate_open(STR(0)->data, start, len);
	ak_buf b = {0};
	for (;;) {
		ak_buf_reserve(&b, 65536);
		size_t got = ak_inflate_read(z, b.data + b.len, 65536);
		if (got == 0) break;
		b.len += got;
		if (b.len > MAX_INDEX) break;
	}
	bool ok = ak_inflate_done(z) && b.len <= MAX_INDEX;
	ak_inflate_close(z);
	if (!ok) {
		ak_buf_free(&b);
		failf("damaged delta update");
		return (ak_val){.p = NULL};
	}
	ak_str *s = ak_str_new((const char *)b.data, b.len);
	ak_buf_free(&b);
	return (ak_val){.p = s};
}

static ak_val delta_start_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int64_t start, len, size;
	if (!delta_head(STR(0)->data, &start, &len, &size)) return (ak_val){.i = -1};
	return (ak_val){.i = start + len};
}

// A file being made: written through, hashed, and no longer than its
// size (delta.go's limitWriter).
typedef struct {
	FILE *f;
	ak_sha256_ctx h;
	int64_t left;
	bool too_long, io_err;
} sink;

static bool sink_write(sink *s, const uint8_t *p, size_t n) {
	if ((int64_t)n > s->left) {
		s->too_long = true;
		return false;
	}
	s->left -= (int64_t)n;
	ak_sha256_update(&s->h, p, n);
	if (fwrite(p, 1, n, s->f) != n) {
		s->io_err = true;
		return false;
	}
	return true;
}

// A stream of bytes read from an inflater, a byte at a time when need be.
typedef struct {
	ak_inflate *z;
	uint8_t buf[4096];
	size_t pos, len;
} pull;

static int pull_byte(pull *p) {
	if (p->pos == p->len) {
		p->len = ak_inflate_read(p->z, p->buf, sizeof p->buf);
		p->pos = 0;
		if (p->len == 0) return -1;
	}
	return p->buf[p->pos++];
}

static bool pull_full(pull *p, uint8_t *out, size_t n) {
	while (n) {
		if (p->pos == p->len) {
			p->len = ak_inflate_read(p->z, p->buf, sizeof p->buf);
			p->pos = 0;
			if (p->len == 0) return false;
		}
		size_t k = p->len - p->pos < n ? p->len - p->pos : n;
		memcpy(out, p->buf + p->pos, k);
		p->pos += k, out += k, n -= k;
	}
	return true;
}

static bool pull_uvarint(pull *p, uint64_t *v) {
	uint64_t x = 0;
	for (int i = 0; i < 10; i++) {
		int b = pull_byte(p);
		if (b < 0 || (i == 9 && b > 1)) return false;
		x |= (uint64_t)(b & 0x7f) << (7 * i);
		if (b < 0x80) {
			*v = x;
			return true;
		}
	}
	return false;
}

static bool pull_varint(pull *p, int64_t *v) {
	uint64_t u;
	if (!pull_uvarint(p, &u)) return false;
	int64_t x = (int64_t)(u >> 1);
	if (u & 1) x = ~x;
	*v = x;
	return true;
}

enum { PATCH_OK, PATCH_DAMAGED, PATCH_FAILED };

// ak_update_patch writes to s the file of size bytes that the bsdiff
// patch at [off, off+len) of patch_path makes of old, a file of old_size
// bytes (bsdiff.go's Patch): the number of blocks, the lengths of the
// first two streams, then the control, difference and extra streams,
// each DEFLATE on its own. A damaged patch fails cleanly: it makes no
// more than size bytes, and old's bytes out of its range count as zeros.
static int patch(sink *s, int64_t size, FILE *old, int64_t old_size, const char *patch_path, int64_t off, int64_t len) {
	FILE *pf = ak_update_fopen(patch_path, "rb");
	if (!pf) return PATCH_FAILED;
	uint8_t head[30];
	size_t got = 0;
	if (off <= 0x7fffffff && fseek(pf, (long)off, SEEK_SET) == 0) got = fread(head, 1, len < 30 ? (size_t)len : 30, pf);
	fclose(pf);
	uint64_t blocks, ctrl_len, diff_len;
	size_t a, b, c;
	if (!(a = uvarint(head, got, &blocks)) || !(b = uvarint(head + a, got - a, &ctrl_len)) || !(c = uvarint(head + a + b, got - a - b, &diff_len)))
		return PATCH_DAMAGED;
	int64_t start = (int64_t)(a + b + c);
	if (size < 0 || blocks > (uint64_t)size + 1 || ctrl_len > (uint64_t)(len - start) || diff_len > (uint64_t)(len - start) - ctrl_len) return PATCH_DAMAGED;
	int64_t extra_start = start + (int64_t)ctrl_len + (int64_t)diff_len;
	pull *ctrl = ak_alloc(sizeof *ctrl), *diff = ak_alloc(sizeof *diff), *extra = ak_alloc(sizeof *extra);
	memset(ctrl, 0, sizeof *ctrl), memset(diff, 0, sizeof *diff), memset(extra, 0, sizeof *extra);
	ctrl->z = ak_inflate_open(patch_path, off + start, (int64_t)ctrl_len);
	diff->z = ak_inflate_open(patch_path, off + start + (int64_t)ctrl_len, (int64_t)diff_len);
	extra->z = ak_inflate_open(patch_path, off + extra_start, len - extra_start);
	enum { BUF = 32768 };
	uint8_t *buf = ak_alloc(BUF), *old_buf = ak_alloc(BUF);
	int64_t old_pos = 0, left = size;
	int r = PATCH_OK;
	for (uint64_t blk = 0; blk < blocks && r == PATCH_OK; blk++) {
		uint64_t d, e;
		int64_t seek;
		if (!pull_uvarint(ctrl, &d) || !pull_uvarint(ctrl, &e) || !pull_varint(ctrl, &seek) || d > (uint64_t)left || e > (uint64_t)left - d) {
			r = PATCH_DAMAGED;
			break;
		}
		left -= (int64_t)(d + e);
		while (d > 0) {
			size_t k = d < BUF ? (size_t)d : BUF;
			if (!pull_full(diff, buf, k)) {
				r = PATCH_DAMAGED;
				break;
			}
			memset(old_buf, 0, k);
			// Only the part of the block inside old is read.
			int64_t from = old_pos > 0 ? old_pos : 0, to = old_pos + (int64_t)k < old_size ? old_pos + (int64_t)k : old_size;
			if (from < to) {
				if (from > 0x7fffffff || fseek(old, (long)from, SEEK_SET) != 0 || fread(old_buf + (from - old_pos), 1, (size_t)(to - from), old) != (size_t)(to - from)) {
					r = PATCH_FAILED;
					break;
				}
			}
			for (size_t i = 0; i < k; i++) buf[i] = (uint8_t)(buf[i] + old_buf[i]);
			if (!sink_write(s, buf, k)) {
				r = s->too_long ? PATCH_DAMAGED : PATCH_FAILED;
				break;
			}
			old_pos += (int64_t)k;
			d -= k;
		}
		while (r == PATCH_OK && e > 0) {
			size_t k = e < BUF ? (size_t)e : BUF;
			if (!pull_full(extra, buf, k)) {
				r = PATCH_DAMAGED;
				break;
			}
			if (!sink_write(s, buf, k)) {
				r = s->too_long ? PATCH_DAMAGED : PATCH_FAILED;
				break;
			}
			e -= k;
		}
		if (r != PATCH_OK) break;
		if (seek > ((int64_t)1 << 40) || seek < -((int64_t)1 << 40)) {
			r = PATCH_DAMAGED;
			break;
		}
		old_pos += seek;
		if (old_pos < -((int64_t)1 << 40) || old_pos > ((int64_t)1 << 40)) r = PATCH_DAMAGED;
	}
	if (r == PATCH_OK && left != 0) r = PATCH_DAMAGED;
	ak_free(buf);
	ak_free(old_buf);
	pull *ps[] = {ctrl, diff, extra};
	for (int i = 0; i < 3; i++) {
		ak_inflate_close(ps[i]->z);
		ak_free(ps[i]);
	}
	return r;
}

static void hex32(const uint8_t *p, char out[65]) {
	static const char digits[] = "0123456789abcdef";
	for (int i = 0; i < 32; i++) out[2 * i] = digits[p[i] >> 4], out[2 * i + 1] = digits[p[i] & 15];
	out[64] = 0;
}

// deltaFile(delta, offset, length, oldDir, from, newDir, path, size,
// sha256, mode): delta.go's applyFile.
static ak_val delta_file_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const char *delta = STR(0)->data, *old_dir = STR(3)->data, *from = STR(4)->data, *new_dir = STR(5)->data, *rel = STR(6)->data;
	int64_t offset = args[1].i, length = args[2].i, size = args[7].i;
	const char *want = STR(8)->data;
	unsigned mode = (unsigned)args[9].i & 0777;
	why[0] = 0;
	if (!ak_update_valid_path(rel, STR(6)->len) || (STR(4)->len && !ak_update_valid_path(from, STR(4)->len)) || offset < 0 || length < 0 || size < 0) {
		failf("damaged delta update");
		return (ak_val){.i = 0};
	}
	if (!dirs_under(new_dir, rel, false)) return (ak_val){.i = 0};
	char *out_path = join(new_dir, rel);
	sink s = {.left = size};
	ak_sha256_init(&s.h);
	s.f = create(out_path, mode, true);
	if (!s.f) {
		ak_free(out_path);
		return (ak_val){.i = 0};
	}
	int r = PATCH_OK;
	if (!STR(4)->len) {
		ak_inflate *z = ak_inflate_open(delta, offset, length);
		uint8_t buf[16384];
		size_t got;
		while (r == PATCH_OK && (got = ak_inflate_read(z, buf, sizeof buf)) > 0)
			if (!sink_write(&s, buf, got)) r = s.too_long ? PATCH_DAMAGED : PATCH_FAILED;
		if (r == PATCH_OK && !ak_inflate_done(z)) r = PATCH_DAMAGED;
		ak_inflate_close(z);
	} else {
		int64_t old_size = 0;
		char *src_path = join(old_dir, from);
		FILE *src = NULL;
		if (!no_links(old_dir, from)) {
			r = PATCH_FAILED;
		} else if (kind_of(src_path, &old_size) != K_FILE) {
			failf("%s is not a file", from);
			r = PATCH_FAILED;
		} else if (!(src = ak_update_fopen(src_path, "rb"))) {
			failf("%s: can't be read", from);
			r = PATCH_FAILED;
		} else if (length == 0) {
			uint8_t buf[16384];
			size_t got;
			while (r == PATCH_OK && (got = fread(buf, 1, sizeof buf, src)) > 0)
				if (!sink_write(&s, buf, got)) r = s.too_long ? PATCH_DAMAGED : PATCH_FAILED;
			if (ferror(src)) r = PATCH_FAILED;
		} else {
			r = patch(&s, size, src, old_size, delta, offset, length);
		}
		if (src) fclose(src);
		ak_free(src_path);
	}
	if (fclose(s.f) != 0 && r == PATCH_OK) s.io_err = true, r = PATCH_FAILED;
	char sum[65];
	uint8_t digest[32];
	ak_sha256_final(&s.h, digest);
	hex32(digest, sum);
	bool ok = r == PATCH_OK && s.left == 0 && strcmp(sum, want) == 0;
	if (ok) {
		set_mode(out_path, mode);
	} else if (r == PATCH_DAMAGED || r == PATCH_OK) {
		// Most likely, the app isn't the one the delta was made from.
		failf("the delta update made %s wrong", rel);
	} else if (!why[0]) {
		failf("%s: %s", rel, s.io_err ? "can't be written" : "can't be made");
	}
	ak_free(out_path);
	return (ak_val){.i = ok};
}

const ak_lib_entry ak_std_update[] = {
	{"update.verifyFile", verify_file_fn},
	{"update.extract", extract_fn},
	{"update.deltaIndex", delta_index_fn},
	{"update.deltaStart", delta_start_fn},
	{"update.deltaFile", delta_file_fn},
	{"update.makeDir", make_dir_fn},
	{"update.makeLink", make_link_fn},
	{"update.validPath", valid_path_fn},
	{"update.failure", failure_fn},
	{"update.otaInfo", ota_info_fn},
	{"update.otaRelaunch", ota_relaunch_fn},
	{NULL, NULL},
};
