// archive.c: std/archive's builtins: zip (PKWARE's APPNOTE: stored and
// deflated entries, zip64, the extended timestamp, NTFS times and
// Info-ZIP's Unicode paths) and tar (POSIX ustar and pax, GNU's long names
// and base-256 numbers, gzipped or not), read from bytes or a file,
// written to bytes or a file, and extracted into a directory.
//
//	archive.scanBytes(data, withData, limit) -> [string]?
//	archive.scanFile(path) -> [string]?
//	    the entries' names and link targets, two an entry; their numbers
//	    (and with withData, their data) wait for scanInfo() (and scanData())
//	archive.scanInfo() -> [int]   the last scan's: INFO an entry
//	archive.scanData() -> [bytes] the last scanBytes' data
//	archive.entryFile(path, name, limit) -> bytes?
//	    a file's data; none: failed() says why, or there's none
//	archive.extractFile(path, dir, limit) -> bool
//	archive.encodeFiles(format, level, names, targets, datas, info) -> bytes?
//	archive.createFile(path, format, level, names, targets, sources, info) -> bool
//	    info: INFO_IN an entry; create's data is each source's file
//	archive.failed() -> bool, failureKind() / failureReason() / failurePath()
//
// Which format an archive is, its bytes say: gzip's magic is a tar's
// gzipped, zip's signatures a zip, a tar header's checksum a tar.
//
// Extracting writes nothing outside dir: an entry's name must be a path
// inside it (not absolute, no drive, no "..", nothing Windows reserves;
// a backslash separates names, as Windows reads it), it's never written
// through a link (one already there, or one an entry made), a link's
// target must stay inside, and what it all decompresses to may not pass
// limit bytes (a zip bomb's).
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
#endif
#include "std/compress.h"

#include <errno.h>
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
#define ARR(i) ((ak_arr *)args[i].p)

// std/archive's Format and Kind, numbered as they're declared.
enum { F_ZIP, F_TAR, F_TAR_GZ };
enum { K_FILE, K_DIR, K_SYMLINK, K_HARDLINK, K_OTHER };
// What info() gives of each entry: its Kind, size, compressed size,
// permissions, and its time: in ns since 1970 (T_UNIX), or as MS-DOS
// keeps it, the date and time on the clock where it was made (T_DOS:
// date << 16 | time), which std/archive reads in the local zone.
enum { INFO = 6 };
enum { T_UNIX, T_DOS };
// What encode and create take of each: Kind, permissions (0: the
// kind's default; create's -1: the source's), ns since 1970, MS-DOS's
// date << 16 | time.
enum { INFO_IN = 4 };

typedef struct {
	char *name;    // as the archive has it, without "./" first or a directory's "/" last
	char *target;  // a link's
	int kind;
	int64_t size, csize, mode;
	int tkind;
	int64_t time;
	// A zip's.
	int method;
	uint16_t flags;
	uint32_t crc;
	int64_t offset;
	bool sparse;  // a tar's GNU sparse file
} entry;

static void entry_free(entry *e) {
	ak_free(e->name);
	ak_free(e->target);
	memset(e, 0, sizeof *e);
}

typedef AK_VEC(entry) entries;

static void entries_free(entries *v) {
	for (uint32_t i = 0; i < v->len; i++) entry_free(&v->data[i]);
	ak_vec_free(v);
}

static char *dup_n(const char *s, size_t n) {
	char *p = ak_alloc(n + 1);
	memcpy(p, s, n);
	return p;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }
static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }

// ---- Names ----

static bool valid_utf8(const uint8_t *s, size_t n) {
	for (size_t i = 0; i < n;) {
		uint8_t c = s[i];
		size_t len = c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 0;
		if (!len || i + len > n) return false;
		uint32_t cp = len == 1 ? c : c & (0x7f >> len);
		for (size_t k = 1; k < len; k++) {
			if ((s[i + k] & 0xc0) != 0x80) return false;
			cp = cp << 6 | (s[i + k] & 0x3f);
		}
		static const uint32_t min[] = {0, 0, 0x80, 0x800, 0x10000};
		if (cp < min[len] || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
		i += len;
	}
	return true;
}

// The code points of IBM's code page 437's upper half: a zip name that
// doesn't say it's UTF-8 is in it (APPNOTE D.1).
static const uint16_t cp437[128] = {
	0xc7, 0xfc, 0xe9, 0xe2, 0xe4, 0xe0, 0xe5, 0xe7, 0xea, 0xeb, 0xe8, 0xef, 0xee, 0xec, 0xc4, 0xc5,
	0xc9, 0xe6, 0xc6, 0xf4, 0xf6, 0xf2, 0xfb, 0xf9, 0xff, 0xd6, 0xdc, 0xa2, 0xa3, 0xa5, 0x20a7, 0x192,
	0xe1, 0xed, 0xf3, 0xfa, 0xf1, 0xd1, 0xaa, 0xba, 0xbf, 0x2310, 0xac, 0xbd, 0xbc, 0xa1, 0xab, 0xbb,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510,
	0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580,
	0x3b1, 0xdf, 0x393, 0x3c0, 0x3a3, 0x3c3, 0xb5, 0x3c4, 0x3a6, 0x398, 0x3a9, 0x3b4, 0x221e, 0x3c6, 0x3b5, 0x2229,
	0x2261, 0xb1, 0x2265, 0x2264, 0x2320, 0x2321, 0xf7, 0x2248, 0xb0, 0x2219, 0xb7, 0x221a, 0x207f, 0xb2, 0x25a0, 0xa0,
};

// text is s as UTF-8: as it is when it's UTF-8 already, else read in
// code page 437 (zip) or Latin-1 (tar), byte by byte.
static char *text(const uint8_t *s, size_t n, bool zip) {
	if (valid_utf8(s, n)) return dup_n((const char *)s, n);
	char *out = ak_alloc(n * 3 + 1), *o = out;
	for (size_t i = 0; i < n; i++) o += ak_utf8_encode(s[i] < 0x80 ? s[i] : zip ? cp437[s[i] - 0x80] : s[i], o);
	*o = 0;
	return out;
}

// tidy drops a name's "./"s before it and its "/"s after: what a name is
// in the entries listed.
static char *tidy(char *s) {
	size_t n = strlen(s), i = 0;
	while (n - i >= 2 && s[i] == '.' && s[i + 1] == '/') {
		i += 2;
		while (i < n && s[i] == '/') i++;
	}
	while (n > i && s[n - 1] == '/') n--;
	memmove(s, s + i, n - i);
	s[n - i] = 0;
	return s;
}

// parts makes name a relative path inside a directory, its names joined
// with '/' into out: false when it isn't one. here adds this system's
// rules (Windows': no ':', no device names) to everywhere's.
static bool parts(const char *name, ak_buf *out, bool here) {
	ak_buf_reset(out);
	size_t n = strlen(name);
	if (n == 0 || name[0] == '/' || name[0] == '\\') return false;
	for (size_t i = 0, start = 0; i <= n; i++) {
		if (i < n && name[i] != '/' && name[i] != '\\') {
			if (name[i] == 0) return false;
#if defined(_WIN32)
			if (here && name[i] == ':') return false;
#endif
			continue;
		}
		const char *p = name + start;
		size_t len = i - start;
		start = i + 1;
		if (len == 0 || (len == 1 && p[0] == '.')) continue;
		if (len == 2 && p[0] == '.' && p[1] == '.') return false;
		// A drive (C:), everywhere: an archive's names are the same
		// wherever it's extracted.
		if (out->len == 0 && len == 2 && p[1] == ':') return false;
#if defined(_WIN32)
		// What Windows takes for a device whatever follows its name
		// (CON, NUL.txt, COM1), and names it would trim (a. is a).
		size_t stem = 0;
		while (stem < len && p[stem] != '.') stem++;
		static const char *const dev[] = {"CON", "PRN", "AUX", "NUL"};
		bool reserved = false;
		for (int k = 0; k < 4; k++) reserved |= stem == 3 && _strnicmp(p, dev[k], 3) == 0;
		if (stem == 4 && (_strnicmp(p, "COM", 3) == 0 || _strnicmp(p, "LPT", 3) == 0) && p[3] >= '1' && p[3] <= '9') reserved = true;
		if (here && (reserved || p[len - 1] == '.' || p[len - 1] == ' ')) return false;
#else
		AK_UNUSED(here);
#endif
		if (out->len) ak_buf_u8(out, '/');
		ak_buf_put(out, p, len);
	}
	ak_buf_u8(out, 0);
	out->len--;
	return true;
}

// link_inside reports whether a link at rel (a parts path) to target
// stays inside the directory, read from where the link is.
static bool link_inside(const char *rel, const char *target) {
	size_t tn = strlen(target);
	if (tn == 0 || target[0] == '/' || strchr(target, '\\') || (tn >= 2 && target[1] == ':')) return false;
#if defined(_WIN32)
	if (strchr(target, ':')) return false;
#endif
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
	return true;
}

// ---- What's on disk ----

enum { P_NONE, P_FILE, P_DIR, P_LINK, P_OTHER };

#if defined(_WIN32)
static wchar_t *wide(const char *p) {
	int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
	if (n <= 0) n = 1;
	wchar_t *w = ak_alloc(sizeof(wchar_t) * (size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, p, -1, w, n);
	return w;
}

static int path_kind(const char *p) {
	wchar_t *w = wide(p);
	DWORD a = GetFileAttributesW(w);
	ak_free(w);
	if (a == INVALID_FILE_ATTRIBUTES) return P_NONE;
	if (a & FILE_ATTRIBUTE_REPARSE_POINT) return P_LINK;
	return a & FILE_ATTRIBUTE_DIRECTORY ? P_DIR : P_FILE;
}

static bool make_dir(const char *p) {
	wchar_t *w = wide(p);
	bool ok = CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
	ak_free(w);
	if (!ok) ak_cz_fail_os(p);
	return ok;
}

static bool remove_path(const char *p) {
	wchar_t *w = wide(p);
	DWORD a = GetFileAttributesW(w);
	bool ok = (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) ? RemoveDirectoryW(w) : DeleteFileW(w);
	ak_free(w);
	if (!ok) ak_cz_fail_os(p);
	return ok;
}

// make_link makes a link at p to target: 1 made, 0 the system doesn't
// let the app (Windows without Developer Mode), -1 failed.
static int make_link(const char *target, const char *p) {
	wchar_t *t = wide(target), *w = wide(p);
	for (wchar_t *c = t; *c; c++)
		if (*c == L'/') *c = L'\\';
	BOOL ok = CreateSymbolicLinkW(w, t, 0x2 /* SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE */);
	DWORD e = ok ? 0 : GetLastError();
	ak_free(t);
	ak_free(w);
	if (ok) return 1;
	if (e == ERROR_PRIVILEGE_NOT_HELD || e == ERROR_INVALID_PARAMETER || e == ERROR_NOT_SUPPORTED) return 0;
	SetLastError(e);
	ak_cz_fail_os(p);
	return -1;
}

static int64_t file_mode(const char *p, bool dir) {
	AK_UNUSED(p);
	return dir ? 0755 : 0644;
}
#else
static int path_kind(const char *p) {
	struct stat st;
	if (lstat(p, &st) != 0) return P_NONE;
	if (S_ISLNK(st.st_mode)) return P_LINK;
	if (S_ISDIR(st.st_mode)) return P_DIR;
	return S_ISREG(st.st_mode) ? P_FILE : P_OTHER;
}

static bool make_dir(const char *p) {
	if (mkdir(p, 0755) == 0 || (errno == EEXIST && path_kind(p) == P_DIR)) return true;
	ak_cz_fail_os(p);
	return false;
}

static bool remove_path(const char *p) {
	if (unlink(p) == 0 || (errno == EISDIR && rmdir(p) == 0)) return true;
	ak_cz_fail_os(p);
	return false;
}

static int make_link(const char *target, const char *p) {
	if (symlink(target, p) == 0) return 1;
	if (errno == EPERM || errno == ENOTSUP || errno == ENOSYS) return 0;
	ak_cz_fail_os(p);
	return -1;
}

static int64_t file_mode(const char *p, bool dir) {
	struct stat st;
	if (stat(p, &st) == 0) return st.st_mode & 0777;
	return dir ? 0755 : 0644;
}
#endif

// ---- Reading an archive ----

typedef struct {
	const uint8_t *mem;  // its bytes, or
	ak_cz_file *f;       // its file
	int64_t size;
	const char *path;    // for failures ("" for bytes)
	int format;
} arc;

static bool corrupt(const arc *a, const char *what) {
	ak_cz_fail(AK_CZ_CORRUPT, a->path, "%s", what);
	return false;
}

// read_at reads n bytes at off.
static bool read_at(arc *a, int64_t off, void *p, size_t n) {
	if (off < 0 || off > a->size || (int64_t)n > a->size - off) return corrupt(a, "the archive ends too soon");
	if (a->mem) {
		memcpy(p, a->mem + off, n);
		return true;
	}
	return ak_cz_seek(a->f, off) && ak_cz_read_full(a->f, p, n, "the archive");
}

// A source of the archive's bytes from off, len of them (-1: to its end).
static ak_cz_src section(arc *a, int64_t off, int64_t len) {
	if (a->mem) {
		size_t end = len < 0 ? (size_t)a->size : (size_t)(off + len);
		return (ak_cz_src){.mem = a->mem + off, .len = end - (size_t)off};
	}
	ak_cz_seek(a->f, off);
	return (ak_cz_src){.f = a->f, .left = len};
}

static bool tar_checksum(const uint8_t *h);

// detect says a's format from its first bytes.
static bool detect(arc *a) {
	uint8_t h[512] = {0};
	size_t n = a->size < 512 ? (size_t)a->size : 512;
	if (n < 4) return corrupt(a, "not a zip or tar archive");
	if (!read_at(a, 0, h, n)) return false;
	if (h[0] == 0x1f && h[1] == 0x8b) a->format = F_TAR_GZ;
	else if (h[0] == 'P' && h[1] == 'K' && ((h[2] == 3 && h[3] == 4) || (h[2] == 5 && h[3] == 6))) a->format = F_ZIP;
	else if (n == 512 && tar_checksum(h)) a->format = F_TAR;
	else a->format = F_ZIP;  // a zip after something else (a self-extracting one), or none
	return true;
}

// ---- Zip ----

enum { ZIP_LOCAL = 0x04034b50, ZIP_CENTRAL = 0x02014b50, ZIP_END = 0x06054b50, ZIP64_END = 0x06064b50, ZIP64_LOC = 0x07064b50 };

typedef struct {
	int64_t base;  // where offset 0 is (data before the archive moves it)
	int64_t dir, dir_size;
	uint64_t count;
} zip_end;

static bool zip_find_end(arc *a, zip_end *z) {
	int64_t n = a->size < 65557 ? a->size : 65557;
	uint8_t *b = ak_alloc((size_t)n);
	bool ok = read_at(a, a->size - n, b, (size_t)n);
	int64_t at = -1;
	for (int64_t i = n - 22; ok && i >= 0; i--)
		if (le32(b + i) == ZIP_END && i + 22 + le16(b + i + 20) <= n) {
			at = i;
			break;
		}
	if (!ok || at < 0) {
		ak_free(b);
		return ok ? corrupt(a, "not a zip or tar archive") : false;
	}
	const uint8_t *e = b + at;
	int64_t end = a->size - n + at;
	if (le16(e + 4) || le16(e + 6)) {
		ak_free(b);
		ak_cz_fail(AK_CZ_UNSUPPORTED, a->path, "the zip spans several disks");
		return false;
	}
	z->count = le16(e + 10);
	z->dir_size = le32(e + 12);
	z->dir = le32(e + 16);
	ak_free(b);
	// zip64's end, when its locator is just before.
	uint8_t loc[20];
	if (end >= 20 && read_at(a, end - 20, loc, 20) && le32(loc) == ZIP64_LOC) {
		uint8_t e64[56];
		int64_t off = (int64_t)le64(loc + 8);
		if (off < 0 || !read_at(a, off, e64, 56) || le32(e64) != ZIP64_END) return corrupt(a, "zip64's end of the directory is missing");
		z->count = le64(e64 + 32);
		z->dir_size = (int64_t)le64(e64 + 40);
		z->dir = (int64_t)le64(e64 + 48);
		end = off;
	}
	z->base = end - z->dir_size - z->dir;
	if (z->dir_size < 0 || z->dir < 0 || z->base < 0 || z->count > (uint64_t)z->dir_size / 46) return corrupt(a, "the zip's directory is broken");
	return true;
}

static const char *method_name(int m) {
	switch (m) {
	case 9: return "Deflate64";
	case 12: return "bzip2";
	case 14: return "LZMA";
	case 93: return "Zstandard";
	case 95: return "xz";
	case 98: return "PPMd";
	case 99: return "AES encryption";
	default: return NULL;
	}
}

// zip_extra reads what e's central header's extra fields add: zip64's
// sizes and offset, the times, a UTF-8 name.
static void zip_extra(entry *e, const uint8_t *x, size_t n, const uint8_t *raw, size_t raw_n, bool *u32_usize, bool *u32_csize, bool *u32_off) {
	for (size_t i = 0; i + 4 <= n;) {
		uint16_t id = le16(x + i), len = le16(x + i + 2);
		const uint8_t *d = x + i + 4;
		if (i + 4 + len > n) break;
		size_t k = 0;
		switch (id) {
		case 0x0001:  // zip64: the fields that are 0xffffffff, in order
			if (*u32_usize && k + 8 <= len) e->size = (int64_t)le64(d + k), k += 8, *u32_usize = false;
			if (*u32_csize && k + 8 <= len) e->csize = (int64_t)le64(d + k), k += 8, *u32_csize = false;
			if (*u32_off && k + 8 <= len) e->offset = (int64_t)le64(d + k), k += 8, *u32_off = false;
			break;
		case 0x5455:  // the extended timestamp: flags, then the modification time
			if (len >= 5 && (d[0] & 1) && e->tkind == T_DOS) e->tkind = T_UNIX, e->time = (int64_t)le32(d + 1) * 1000000000;
			break;
		case 0x000a:  // NTFS: attributes, tag 1 the times (in 100 ns since 1601)
			for (k = 4; k + 4 <= len;) {
				uint16_t tag = le16(d + k), size = le16(d + k + 2);
				uint64_t t = tag == 1 && size >= 24 && k + 4 + 24 <= len ? le64(d + k + 4) : 0;
				if (t >= 116444736000000000ull && t - 116444736000000000ull < (uint64_t)INT64_MAX / 100) e->tkind = T_UNIX, e->time = (int64_t)(t - 116444736000000000ull) * 100;
				k += 4 + (size_t)size;
			}
			break;
		case 0x7075:  // Info-ZIP's Unicode path: when the name it was made from is this one's
			if (len >= 5 && d[0] == 1 && le32(d + 1) == ak_cz_crc32(0, raw, raw_n) && valid_utf8(d + 5, len - 5u)) {
				ak_free(e->name);
				e->name = dup_n((const char *)d + 5, len - 5u);
			}
			break;
		}
		i += 4 + (size_t)len;
	}
}

// zip_entries reads the central directory.
static bool zip_entries(arc *a, entries *out) {
	zip_end z = {0};
	if (!zip_find_end(a, &z)) return false;
	if (z.base + z.dir + z.dir_size > a->size) return corrupt(a, "the zip's directory is past its end");
	uint8_t *d = ak_alloc((size_t)z.dir_size + 1);
	if (!read_at(a, z.base + z.dir, d, (size_t)z.dir_size)) {
		ak_free(d);
		return false;
	}
	size_t pos = 0, n = (size_t)z.dir_size;
	for (uint64_t i = 0; i < z.count; i++) {
		if (pos + 46 > n || le32(d + pos) != ZIP_CENTRAL) {
			ak_free(d);
			return corrupt(a, "the zip's directory is broken");
		}
		const uint8_t *h = d + pos;
		size_t nlen = le16(h + 28), xlen = le16(h + 30), clen = le16(h + 32);
		if (pos + 46 + nlen + xlen + clen > n) {
			ak_free(d);
			return corrupt(a, "the zip's directory is broken");
		}
		const uint8_t *raw = h + 46;
		entry e = {0};
		e.flags = le16(h + 8);
		e.method = le16(h + 10);
		e.tkind = T_DOS;
		e.time = (int64_t)le16(h + 14) << 16 | le16(h + 12);
		e.crc = le32(h + 16);
		e.csize = le32(h + 20);
		e.size = le32(h + 24);
		e.offset = le32(h + 42);
		e.name = (e.flags & 0x800) ? dup_n((const char *)raw, nlen) : text(raw, nlen, true);
		bool big_u = e.size == 0xffffffff, big_c = e.csize == 0xffffffff, big_o = e.offset == 0xffffffff;
		zip_extra(&e, raw + nlen, xlen, raw, nlen, &big_u, &big_c, &big_o);
		pos += 46 + nlen + xlen + clen;
		// The kind and permissions: Unix's mode when it was made on Unix
		// or a Mac, else MS-DOS's attributes.
		uint32_t ext = le32(h + 38);
		int host = h[5];
		uint32_t mode = (host == 3 || host == 19) ? ext >> 16 : 0;
		bool slash = nlen && (raw[nlen - 1] == '/' || raw[nlen - 1] == '\\');
		if ((mode & 0170000) == 0040000 || slash || (!mode && (ext & 0x10))) e.kind = K_DIR;
		else if ((mode & 0170000) == 0120000) e.kind = K_SYMLINK;
		else if (mode & 0170000 && (mode & 0170000) != 0100000) e.kind = K_OTHER;
		else e.kind = K_FILE;
		e.mode = mode & 0777 ? mode & 0777 : e.kind == K_DIR ? 0755 : (ext & 1) ? 0444 : 0644;
		tidy(e.name);
		if (e.kind == K_DIR && !e.name[0]) {
			entry_free(&e);
			continue;
		}
		ak_vec_push(out, e);
	}
	ak_free(d);
	for (uint32_t i = 0; i < out->len; i++) out->data[i].offset += z.base;
	return true;
}

// zip_data is a source of e's compressed bytes, past its local header.
static bool zip_data(arc *a, const entry *e, ak_cz_src *src) {
	uint8_t h[30];
	if (!read_at(a, e->offset, h, 30)) return false;
	if (le32(h) != ZIP_LOCAL) return corrupt(a, "an entry's local header is missing");
	int64_t start = e->offset + 30 + le16(h + 26) + le16(h + 28);
	if (start > a->size || e->csize > a->size - start) return corrupt(a, "an entry is past the archive's end");
	*src = section(a, start, e->csize);
	return true;
}

// The data's destination: a file, or bytes in memory.
typedef struct {
	ak_cz_file *f;
	ak_buf *mem;
} dest;

static bool put(dest *d, const uint8_t *p, size_t n) {
	if (d->f) return ak_cz_write(d->f, p, n);
	if (d->mem->len + n > UINT32_MAX) {
		ak_cz_fail(AK_CZ_TOO_LARGE, "", "an entry is larger than 4 GB, more than bytes hold");
		return false;
	}
	ak_buf_put(d->mem, p, n);
	return true;
}

enum { CHUNK = 1 << 16 };

static bool readable(arc *a, const entry *e) {
	if (e->flags & 1) {
		ak_cz_fail(AK_CZ_UNSUPPORTED, e->name, "it's encrypted");
		return false;
	}
	if (e->method != 0 && e->method != 8) {
		const char *m = method_name(e->method);
		if (m) ak_cz_fail(AK_CZ_UNSUPPORTED, e->name, "it's compressed with %s", m);
		else ak_cz_fail(AK_CZ_UNSUPPORTED, e->name, "it's compressed with method %d", e->method);
		return false;
	}
	AK_UNUSED(a);
	return true;
}

// zip_copy writes e's data to d, checking its size and CRC-32.
static bool zip_copy(arc *a, const entry *e, dest *d) {
	if (!readable(a, e)) return false;
	ak_cz_src src;
	if (!zip_data(a, e, &src)) return false;
	ak_cz_inflate *z = e->method == 8 ? ak_cz_inflate_new(&src, AK_CZ_RAW, -1) : NULL;
	uint8_t *buf = ak_alloc(CHUNK);
	uint64_t total = 0;
	uint32_t crc = 0;
	bool ok = true;
	for (;;) {
		size_t got = z ? ak_cz_inflate_read(z, buf, CHUNK) : ak_cz_src_read(&src, buf, CHUNK);
		if (got == 0) break;
		total += got;
		if (total > (uint64_t)e->size) {
			ok = false;
			ak_cz_fail(AK_CZ_CORRUPT, e->name, "it's larger than the zip says");
			break;
		}
		crc = ak_cz_crc32(crc, buf, got);
		if (!(ok = put(d, buf, got))) break;
	}
	if (ok && z && !ak_cz_inflate_done(z)) ok = false;
	if (ok && ak_cz_failed()) ok = false;
	if (ok && total != (uint64_t)e->size) ok = false, ak_cz_fail(AK_CZ_CORRUPT, e->name, "it's smaller than the zip says");
	if (ok && crc != e->crc) ok = false, ak_cz_fail(AK_CZ_CORRUPT, e->name, "its checksum doesn't match");
	ak_free(buf);
	if (z) ak_cz_inflate_free(z);
	return ok;
}

// zip_target reads a link's target, its data.
static bool zip_target(arc *a, entry *e) {
	if (e->size > 4096) return corrupt(a, "a link's target is too long");
	ak_buf b = {0};
	dest d = {.mem = &b};
	bool ok = zip_copy(a, e, &d);
	if (ok) {
		ak_free(e->target);
		e->target = text(b.data ? b.data : (const uint8_t *)"", b.len, false);
	}
	ak_buf_free(&b);
	return ok;
}

// ---- Tar ----

// The reader of a tar: its bytes, or what they inflate to.
typedef struct {
	arc *a;
	ak_cz_src src;
	ak_cz_inflate *z;
	int64_t pos;  // in the tar
} tar_rd;

static size_t tar_read(tar_rd *r, uint8_t *p, size_t n) {
	size_t k = 0;
	while (k < n) {
		size_t got = r->z ? ak_cz_inflate_read(r->z, p + k, n - k) : ak_cz_src_read(&r->src, p + k, n - k);
		if (got == 0) break;
		k += got;
	}
	r->pos += (int64_t)k;
	return k;
}

static bool tar_full(tar_rd *r, uint8_t *p, size_t n) {
	if (tar_read(r, p, n) == n) return true;
	if (!ak_cz_failed()) corrupt(r->a, "the tar ends too soon");
	return false;
}

static bool tar_skip(tar_rd *r, int64_t n) {
	if (!r->z && !r->a->mem) {
		if (r->pos + n > r->a->size) return corrupt(r->a, "the tar ends too soon");
		r->pos += n;
		return ak_cz_seek(r->a->f, r->pos);
	}
	if (!r->z) {
		if (n > (int64_t)(r->src.len - r->src.pos)) return corrupt(r->a, "the tar ends too soon");
		r->src.pos += (size_t)n;
		r->pos += n;
		return true;
	}
	uint8_t buf[4096];
	while (n > 0) {
		size_t m = n > (int64_t)sizeof buf ? sizeof buf : (size_t)n;
		if (!tar_full(r, buf, m)) return false;
		n -= (int64_t)m;
	}
	return true;
}

static int64_t pad(int64_t n) { return (512 - n % 512) % 512; }

static bool tar_checksum(const uint8_t *h) {
	int64_t want = 0;
	size_t i = 148;
	while (i < 156 && (h[i] == ' ' || h[i] == 0)) i++;
	if (i == 156) return false;
	for (; i < 156 && h[i] >= '0' && h[i] <= '7'; i++) want = want * 8 + (h[i] - '0');
	int64_t u = 0, s = 0;
	for (int k = 0; k < 512; k++) {
		uint8_t c = k >= 148 && k < 156 ? ' ' : h[k];
		u += c;
		s += (int8_t)c;
	}
	return want == u || want == s;
}

// number reads a header's number: octal, or GNU's base 256; -1 when it's
// neither.
static int64_t number(const uint8_t *p, size_t n) {
	if (p[0] & 0x80) {
		if (p[0] == 0xff) return -1;
		uint64_t v = p[0] & 0x7f;
		for (size_t i = 1; i < n; i++) {
			if (v >> 55) return -1;
			v = v << 8 | p[i];
		}
		return (int64_t)v;
	}
	size_t i = 0;
	while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
	int64_t v = 0;
	for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
		if (v >> 60) return -1;
		v = v * 8 + (p[i] - '0');
	}
	for (; i < n; i++)
		if (p[i] != ' ' && p[i] != 0) return -1;
	return v;
}

static size_t field_len(const uint8_t *p, size_t n) {
	size_t k = 0;
	while (k < n && p[k]) k++;
	return k;
}

// What a pax header (or GNU's long names) says of the next entry.
typedef struct {
	char *path, *link;
	int64_t size, mtime;  // -1: not said; mtime in ns
	bool has_mtime, sparse;
} tar_ext;

static void ext_free(tar_ext *x) {
	ak_free(x->path);
	ak_free(x->link);
	*x = (tar_ext){.size = -1};
}

// pax reads a pax header's records ("%d %s=%s\n").
static bool pax(arc *a, const uint8_t *p, size_t n, tar_ext *x) {
	for (size_t i = 0; i < n;) {
		size_t j = i, len = 0;
		while (j < n && p[j] >= '0' && p[j] <= '9' && len < n) len = len * 10 + (p[j++] - '0');
		if (j >= n || p[j] != ' ' || len <= j - i || i + len > n || p[i + len - 1] != '\n') return corrupt(a, "a pax header is broken");
		const uint8_t *key = p + j + 1, *end = p + i + len - 1;
		const uint8_t *eq = memchr(key, '=', (size_t)(end - key));
		if (!eq) return corrupt(a, "a pax header is broken");
		size_t klen = (size_t)(eq - key), vlen = (size_t)(end - eq - 1);
		const char *v = (const char *)eq + 1;
		if (klen == 4 && memcmp(key, "path", 4) == 0) {
			ak_free(x->path);
			x->path = text((const uint8_t *)v, vlen, false);
		} else if (klen == 8 && memcmp(key, "linkpath", 8) == 0) {
			ak_free(x->link);
			x->link = text((const uint8_t *)v, vlen, false);
		} else if (klen == 4 && memcmp(key, "size", 4) == 0) {
			int64_t s = 0;
			for (size_t k = 0; k < vlen; k++) {
				if (v[k] < '0' || v[k] > '9' || s > INT64_MAX / 10 - 9) return corrupt(a, "a pax header's size is broken");
				s = s * 10 + (v[k] - '0');
			}
			x->size = s;
		} else if (klen == 5 && memcmp(key, "mtime", 5) == 0) {
			// Seconds, perhaps negative, perhaps with a fraction.
			size_t k = 0;
			bool neg = vlen && v[0] == '-';
			if (neg) k++;
			int64_t s = 0, ns = 0, scale = 100000000;
			for (; k < vlen && v[k] >= '0' && v[k] <= '9' && s < 1000000000000ll; k++) s = s * 10 + (v[k] - '0');
			if (k < vlen && v[k] == '.')
				for (k++; k < vlen && v[k] >= '0' && v[k] <= '9'; k++, scale /= 10) ns += (v[k] - '0') * scale;
			int64_t t = s * 1000000000 + ns;
			x->mtime = neg ? -t : t;
			if (s >= 1000000000000ll) x->mtime = 0;
			x->has_mtime = true;
		} else if (klen > 11 && memcmp(key, "GNU.sparse.", 11) == 0) {
			x->sparse = true;
		}
		i += len;
	}
	return true;
}

// tar_next reads the next entry's header(s) into e, and its data's size:
// 1, or 0 at the archive's end, -1 failed.
static int tar_next(tar_rd *r, entry *e, int64_t *data) {
	tar_ext x = {.size = -1};
	uint8_t h[512];
	for (;;) {
		size_t got = tar_read(r, h, 512);
		if (got == 0 && !ak_cz_failed() && !x.path && !x.link) {
			ext_free(&x);
			return 0;  // no end blocks: the end all the same, as tar reads it
		}
		if (got != 512) {
			if (!ak_cz_failed()) corrupt(r->a, "the tar ends too soon");
			ext_free(&x);
			return -1;
		}
		bool zero = true;
		for (int i = 0; i < 512 && zero; i++) zero = h[i] == 0;
		if (zero) {
			ext_free(&x);
			return 0;
		}
		if (!tar_checksum(h)) {
			ext_free(&x);
			corrupt(r->a, r->pos == 512 ? "not a zip or tar archive" : "a tar header's checksum doesn't match");
			return -1;
		}
		int64_t size = number(h + 124, 12);
		if (size < 0) {
			ext_free(&x);
			corrupt(r->a, "a tar header's size is broken");
			return -1;
		}
		uint8_t type = h[156];
		if (type == 'x' || type == 'g' || type == 'L' || type == 'K') {
			if (size > (64 << 20)) {
				ext_free(&x);
				corrupt(r->a, "a tar's extended header is too large");
				return -1;
			}
			uint8_t *b = ak_alloc((size_t)size + 1);
			bool ok = tar_full(r, b, (size_t)size) && tar_skip(r, pad(size));
			if (ok && type == 'x') ok = pax(r->a, b, (size_t)size, &x);
			else if (ok && type != 'g') {
				// GNU's long name or link: its data, up to a NUL.
				char **to = type == 'L' ? &x.path : &x.link;
				ak_free(*to);
				*to = text(b, field_len(b, (size_t)size), false);
			}
			ak_free(b);
			if (!ok) {
				ext_free(&x);
				return -1;
			}
			continue;  // a global pax header is the archive's, not an entry's
		}
		bool posix = memcmp(h + 257, "ustar\0", 6) == 0;
		if (x.path) {
			e->name = x.path, x.path = NULL;
		} else {
			size_t nl = field_len(h, 100), pl = posix ? field_len(h + 345, 155) : 0;
			uint8_t full[257];
			memcpy(full, h + 345, pl);
			size_t k = pl;
			if (pl) full[k++] = '/';
			memcpy(full + k, h, nl);
			e->name = text(full, k + nl, false);
		}
		if (x.link) e->target = x.link, x.link = NULL;
		else e->target = text(h + 157, field_len(h + 157, 100), false);
		if (x.size >= 0) size = x.size;
		int64_t mtime = number(h + 136, 12);
		e->tkind = T_UNIX;
		e->time = x.has_mtime ? x.mtime : mtime < 0 || mtime > 9000000000ll ? 0 : mtime * 1000000000;
		int64_t mode = number(h + 100, 8);
		e->mode = mode < 0 ? 0644 : mode & 0777;
		size_t raw_len = strlen(e->name);
		switch (type) {
		case '0':
		case 0:
		case '7':
			e->kind = raw_len && e->name[raw_len - 1] == '/' ? K_DIR : K_FILE;
			break;
		case '1': e->kind = K_HARDLINK; break;
		case '2': e->kind = K_SYMLINK; break;
		case '5': e->kind = K_DIR; break;
		case 'S':
			e->kind = K_FILE;
			x.sparse = true;
			break;
		case '3':
		case '4':
		case '6':
		case 'V':
			e->kind = K_OTHER;
			break;
		default: e->kind = K_FILE;  // POSIX: an unknown type is a file
		}
		e->sparse = x.sparse;
		// What has no data, whatever its size says (Go's
		// isHeaderOnlyType).
		bool header_only = type == '1' || type == '2' || type == '3' || type == '4' || type == '5' || type == '6';
		*data = header_only ? 0 : size;
		e->size = e->kind == K_FILE ? size : 0;
		e->csize = e->size;
		ext_free(&x);
		if (e->kind != K_SYMLINK && e->kind != K_HARDLINK) {
			ak_free(e->target);
			e->target = dup_n("", 0);
		}
		tidy(e->name);
		if (e->kind == K_HARDLINK) tidy(e->target);
		return 1;
	}
}

static bool tar_open(arc *a, tar_rd *r) {
	*r = (tar_rd){.a = a, .src = section(a, 0, -1)};
	if (a->format == F_TAR_GZ) {
		r->z = ak_cz_inflate_new(&r->src, AK_CZ_GZIP, -1);
		if (ak_cz_failed()) return false;
	}
	return true;
}

static void tar_close(tar_rd *r) {
	if (r->z) ak_cz_inflate_free(r->z);
}

// tar_copy writes the entry's data (n bytes, then its padding) to d.
static bool tar_copy(tar_rd *r, int64_t n, dest *d) {
	uint8_t *buf = ak_alloc(CHUNK);
	bool ok = true;
	while (ok && n > 0) {
		size_t m = n > CHUNK ? CHUNK : (size_t)n;
		ok = tar_full(r, buf, m) && put(d, buf, m);
		n -= (int64_t)m;
	}
	ak_free(buf);
	return ok && tar_skip(r, pad(r->pos));
}

// The end of an entry's data: its padding read past.
static bool tar_past(tar_rd *r, int64_t n) { return tar_skip(r, n + pad(r->pos + n)); }

// ---- Opening ----

static bool open_bytes(arc *a, const ak_bytes *data) {
	static const uint8_t none[1];
	*a = (arc){.mem = data && data->len ? data->data : none, .size = data ? data->len : 0, .path = ""};
	return detect(a);
}

static bool open_file(arc *a, const char *path) {
	*a = (arc){.path = path};
	if (!(a->f = ak_cz_open(path))) return false;
	if ((a->size = ak_cz_size(a->f)) < 0) return false;
	return detect(a);
}

static void close_arc(arc *a) {
	ak_cz_close(a->f);
	a->f = NULL;
}

// ---- Scanning: the entries, perhaps with their data ----

static AK_TLS ak_arr *last_info, *last_data;

static void keep(ak_arr *info, ak_arr *data) {
	ak_release(last_info);
	ak_release(last_data);
	last_info = info;
	last_data = data;
}

static ak_val take(ak_arr **a, ak_kind k) {
	ak_arr *r = *a ? *a : ak_arr_new(k, NULL, 0);
	*a = NULL;
	return (ak_val){.p = r};
}

static ak_val info_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return take(&last_info, AK_K_INT);
}

static ak_val data_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return take(&last_data, AK_K_REF);
}

static void add(ak_arr **names, ak_arr **info, const entry *e) {
	ak_str_arr_push(names, e->name, strlen(e->name));
	ak_str_arr_push(names, e->target ? e->target : "", e->target ? strlen(e->target) : 0);
	bool file = e->kind == K_FILE;
	int64_t v[INFO] = {e->kind, file ? e->size : 0, file ? e->csize : 0, e->mode, e->tkind, e->time};
	for (int i = 0; i < INFO; i++) ak_arr_push(info, (ak_val){.i = v[i]});
}

static bool too_large(int64_t used, int64_t more, int64_t limit, const char *name) {
	if (limit >= 0 && more > limit - used) {
		ak_cz_fail(AK_CZ_TOO_LARGE, name, "it decompresses to more than %lld bytes", (long long)limit);
		return true;
	}
	return false;
}

static void push_bytes(ak_arr **data, ak_buf *b) {
	ak_arr_push(data, (ak_val){.p = ak_bytes_new(b->data, b->len)});
	ak_buf_reset(b);
}

// scan lists a's entries into names and info; with data, their data too
// (no more than limit bytes in all).
static bool scan(arc *a, ak_arr **names, ak_arr **info, ak_arr **data, int64_t limit) {
	ak_buf b = {0};
	dest d = {.mem = &b};
	int64_t used = 0;
	bool ok = true;
	if (a->format == F_ZIP) {
		entries es = {0};
		ok = zip_entries(a, &es);
		for (uint32_t i = 0; ok && i < es.len; i++) {
			entry *e = &es.data[i];
			if (e->kind == K_SYMLINK) ok = zip_target(a, e);
			if (ok && data && e->kind == K_FILE) ok = !too_large(used, e->size, limit, e->name) && zip_copy(a, e, &d);
			if (!ok) break;
			used += e->size;
			add(names, info, e);
			if (data) push_bytes(data, &b);
		}
		entries_free(&es);
	} else {
		tar_rd r;
		ok = tar_open(a, &r);
		while (ok) {
			entry e = {0};
			int64_t size = 0;
			int got = tar_next(&r, &e, &size);
			if (got <= 0) {
				ok = got == 0;
				entry_free(&e);
				break;
			}
			if (data && e.kind == K_FILE) {
				if (e.sparse) {
					ok = false;
					ak_cz_fail(AK_CZ_UNSUPPORTED, e.name, "it's a sparse file");
				} else {
					ok = !too_large(used, size, limit, e.name) && tar_copy(&r, size, &d);
					used += size;
				}
			} else {
				ok = tar_past(&r, size);
			}
			if (ok && !(e.kind == K_DIR && !e.name[0])) {
				add(names, info, &e);
				if (data) push_bytes(data, &b);
			}
			entry_free(&e);
		}
		tar_close(&r);
	}
	ak_buf_free(&b);
	return ok;
}

static ak_val scan_result(bool ok, ak_arr *names, ak_arr *info, ak_arr *data) {
	if (!ok) {
		ak_release(names);
		ak_release(info);
		ak_release(data);
		keep(NULL, NULL);
		return (ak_val){.p = NULL};
	}
	keep(info, data);
	return (ak_val){.p = names};
}

static ak_val scan_bytes(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	arc a;
	ak_arr *names = ak_str_arr(0), *info = ak_arr_new(AK_K_INT, NULL, 0), *data = args[1].i ? ak_arr_new(AK_K_REF, NULL, 0) : NULL;
	bool ok = open_bytes(&a, BYTES(0)) && scan(&a, &names, &info, data ? &data : NULL, args[2].i);
	return scan_result(ok, names, info, data);
}

static ak_val scan_file(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	arc a;
	ak_arr *names = ak_str_arr(0), *info = ak_arr_new(AK_K_INT, NULL, 0);
	bool ok = open_file(&a, STR(0)->data) && scan(&a, &names, &info, NULL, -1);
	close_arc(&a);
	return scan_result(ok, names, info, NULL);
}

// ---- One entry's data ----

// entry_of reads the data of a's first file named name (as tidy makes
// names); false with no failure when there's none.
static bool entry_of(arc *a, const char *want, int64_t limit, ak_buf *b) {
	char *name = tidy(dup_n(want, strlen(want)));
	dest d = {.mem = b};
	bool found = false;
	if (a->format == F_ZIP) {
		entries es = {0};
		if (zip_entries(a, &es))
			for (uint32_t i = 0; i < es.len && !found; i++) {
				entry *e = &es.data[i];
				if (e->kind != K_FILE || strcmp(e->name, name) != 0) continue;
				found = !too_large(0, e->size, limit, e->name) && zip_copy(a, e, &d);
				if (!found) break;
			}
		entries_free(&es);
	} else {
		tar_rd r;
		bool ok = tar_open(a, &r);
		while (ok && !found) {
			entry e = {0};
			int64_t size = 0;
			int got = tar_next(&r, &e, &size);
			if (got <= 0) {
				entry_free(&e);
				break;
			}
			if (e.kind == K_FILE && strcmp(e.name, name) == 0) {
				if (e.sparse) {
					ak_cz_fail(AK_CZ_UNSUPPORTED, e.name, "it's a sparse file");
					ok = false;
				} else {
					found = !too_large(0, size, limit, e.name) && tar_copy(&r, size, &d);
					ok = found;
				}
			} else {
				ok = tar_past(&r, size);
			}
			entry_free(&e);
		}
		tar_close(&r);
	}
	ak_free(name);
	return found;
}

static ak_val entry_result(bool found, ak_buf *b) {
	ak_val r = {.p = found ? ak_bytes_new(b->data, b->len) : NULL};
	ak_buf_free(b);
	return r;
}

static ak_val entry_file(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	arc a;
	ak_buf b = {0};
	bool found = open_file(&a, STR(0)->data) && entry_of(&a, STR(1)->data, args[2].i, &b);
	close_arc(&a);
	return entry_result(found, &b);
}

static ak_val failed_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.i = ak_cz_failed()};
}

// ---- Extracting ----

typedef struct {
	const char *dir;
	int64_t limit, used;
	ak_buf rel;   // the entry's path inside dir (parts')
	ak_buf path;  // dir joined with it
} ext;

#if defined(_WIN32)
#define SEP '\\'
#else
#define SEP '/'
#endif

// join makes out dir joined with rel (its '/'s the system's).
static void join(const char *dir, const char *rel, ak_buf *out) {
	ak_buf_reset(out);
	size_t dn = strlen(dir);
	ak_buf_put(out, dir, dn);
	if (dn && dir[dn - 1] != '/' && dir[dn - 1] != '\\') ak_buf_u8(out, SEP);
	for (const char *c = rel; *c; c++) ak_buf_u8(out, *c == '/' ? SEP : (uint8_t)*c);
	ak_buf_u8(out, 0);
}

static const char *path_of(ext *x) { return (const char *)x->path.data; }

// dirs makes the directories x->path is in, under dir; none may be a
// link or a file.
static bool dirs(ext *x, const char *name) {
	size_t dn = strlen(x->dir);
	char *p = (char *)x->path.data;
	for (size_t i = dn + 1; p[i]; i++) {
		if (p[i] != SEP) continue;
		p[i] = 0;
		int k = path_kind(p);
		bool ok = k == P_DIR || (k == P_NONE && make_dir(p));
		if (k == P_LINK) ak_cz_fail(AK_CZ_UNSAFE, name, "it would be written through the link %s", p);
		else if (k == P_FILE || k == P_OTHER) ak_cz_fail(AK_CZ_NOT_DIR, p, "not a directory");
		p[i] = SEP;
		if (!ok) return false;
	}
	return true;
}

// clear readies x->path for what an entry makes there: a link or a file
// that's there goes; a directory stays only for a directory.
static bool clear(ext *x, bool dir) {
	int k = path_kind(path_of(x));
	if (k == P_NONE) return true;
	if (k == P_DIR) {
		if (dir) return true;
		ak_cz_fail(AK_CZ_IS_DIR, path_of(x), "is a directory");
		return false;
	}
	return remove_path(path_of(x));
}

// prepare checks e's name and makes what's above it: false when it
// failed, or (skip) when there's nothing to make (the archive's own
// directory, a kind extract leaves out).
static bool prepare(ext *x, const entry *e, bool *skip) {
	*skip = false;
	if (!parts(e->name, &x->rel, true) || (x->rel.len == 0 && e->kind != K_DIR)) {
		ak_cz_fail(AK_CZ_UNSAFE, e->name, "it isn't a path inside the directory");
		return false;
	}
	if (x->rel.len == 0 || e->kind == K_OTHER) {
		*skip = true;
		return true;
	}
	join(x->dir, (const char *)x->rel.data, &x->path);
	return dirs(x, e->name) && clear(x, e->kind == K_DIR);
}

static bool make(ext *x, const entry *e, ak_cz_file **f) {
	*f = ak_cz_create(path_of(x), e->mode & 0777 ? (int)(e->mode & 0777) : 0644, NULL);
	return *f != NULL;
}

// made closes the file an entry made, its time set.
static bool made(ak_cz_file *f, const entry *e, bool ok) {
	if (ok && e->tkind == T_UNIX) ak_cz_set_modified(f, e->time);
	return ak_cz_close(f) && ok;
}

// link_entry makes e (a link) at x->path: a symlink, or a hard link's
// copy of the file it names, made before it.
static bool link_entry(ext *x, const entry *e) {
	if (e->kind == K_SYMLINK) {
		if (!link_inside((const char *)x->rel.data, e->target)) {
			ak_cz_fail(AK_CZ_UNSAFE, e->name, "its link's target %s isn't inside the directory", e->target);
			return false;
		}
		return make_link(e->target, path_of(x)) >= 0;
	}
	ak_buf rel = {0}, from = {0};
	bool ok = parts(e->target, &rel, true) && rel.len;
	if (!ok) ak_cz_fail(AK_CZ_UNSAFE, e->name, "its link's target %s isn't inside the directory", e->target);
	if (ok) {
		join(x->dir, (const char *)rel.data, &from);
		if (path_kind((const char *)from.data) != P_FILE) {
			ak_cz_fail(AK_CZ_CORRUPT, e->name, "the file it links to, %s, isn't before it", e->target);
			ok = false;
		}
	}
	ak_cz_file *in = ok ? ak_cz_open((const char *)from.data) : NULL, *out = NULL;
	ok = in && make(x, e, &out);
	int64_t size = ok ? ak_cz_size(in) : -1;
	ok = ok && size >= 0 && !too_large(x->used, size, x->limit, e->name);
	if (ok) {
		x->used += size;
		uint8_t *buf = ak_alloc(CHUNK);
		for (;;) {
			int64_t got = ak_cz_read(in, buf, CHUNK);
			if (got <= 0) {
				ok = got == 0;
				break;
			}
			if (!(ok = ak_cz_write(out, buf, (size_t)got))) break;
		}
		ak_free(buf);
	}
	if (out) ok = made(out, e, ok);
	ak_cz_close(in);
	ak_buf_free(&rel);
	ak_buf_free(&from);
	return ok;
}

static bool extract(arc *a, const char *dir, int64_t limit) {
	if (path_kind(dir) == P_NONE && !ak_fs_make_dirs(dir)) {
		ak_cz_fail_os(dir);
		return false;
	}
	ext x = {.dir = dir, .limit = limit};
	bool ok = true, skip;
	if (a->format == F_ZIP) {
		entries es = {0};
		ok = zip_entries(a, &es);
		for (uint32_t i = 0; ok && i < es.len; i++) {
			entry *e = &es.data[i];
			if (e->kind == K_SYMLINK && !(ok = zip_target(a, e))) break;
			if (!(ok = prepare(&x, e, &skip)) || skip) continue;
			if (e->kind == K_DIR) {
				ok = make_dir(path_of(&x));
			} else if (e->kind == K_FILE) {
				ak_cz_file *f;
				ok = readable(a, e) && !too_large(x.used, e->size, limit, e->name) && make(&x, e, &f);
				if (ok) {
					x.used += e->size;
					dest d = {.f = f};
					ok = made(f, e, zip_copy(a, e, &d));
				}
			} else {
				ok = link_entry(&x, e);
			}
		}
		entries_free(&es);
	} else {
		tar_rd r;
		ok = tar_open(a, &r);
		while (ok) {
			entry e = {0};
			int64_t size = 0;
			int got = tar_next(&r, &e, &size);
			if (got <= 0) {
				ok = got == 0;
				entry_free(&e);
				break;
			}
			if ((ok = prepare(&x, &e, &skip)) && !skip) {
				if (e.kind == K_DIR) {
					ok = make_dir(path_of(&x));
				} else if (e.kind == K_FILE) {
					ak_cz_file *f;
					if (e.sparse) {
						ak_cz_fail(AK_CZ_UNSUPPORTED, e.name, "it's a sparse file");
						ok = false;
					} else if ((ok = !too_large(x.used, size, limit, e.name) && make(&x, &e, &f))) {
						x.used += size;
						dest d = {.f = f};
						ok = made(f, &e, tar_copy(&r, size, &d));
						size = -1;
					}
				} else {
					ok = link_entry(&x, &e);
				}
			}
			if (ok && size >= 0) ok = tar_past(&r, size);
			entry_free(&e);
		}
		tar_close(&r);
	}
	ak_buf_free(&x.rel);
	ak_buf_free(&x.path);
	return ok;
}

static ak_val extract_file(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	arc a;
	bool ok = open_file(&a, STR(0)->data) && extract(&a, STR(1)->data, args[2].i);
	close_arc(&a);
	return (ak_val){.i = ok};
}

// ---- Writing ----

// What's written: an entry, and where its data comes from (bytes, or a
// file at source; none for a directory or a link).
typedef struct {
	const char *name, *target;  // name: tidied (in nb)
	size_t name_len, target_len;
	ak_buf nb;
	int kind;
	int64_t mode, time;  // time: ns since 1970
	uint32_t dos;
	const ak_bytes *data;
	const char *source;
} item;

// The writer: the sink, and for a tar.gz the deflater into it.
typedef struct {
	ak_cz_sink *sink;
	ak_cz_deflate *z;
	int format, level;
	// A zip's directory so far.
	ak_buf dir;
	uint64_t count;
} wr;

static bool w_put(wr *w, const void *p, size_t n) { return w->z ? ak_cz_deflate_write(w->z, p, n) : ak_cz_sink_write(w->sink, p, n); }

// The data's source, read from its start.
typedef struct {
	const ak_bytes *mem;
	size_t pos;
	ak_cz_file *f;
} feed;

static int64_t feed_read(feed *s, uint8_t *p, size_t n) {
	if (s->f) return ak_cz_read(s->f, p, n);
	size_t len = s->mem ? s->mem->len : 0, k = len - s->pos < n ? len - s->pos : n;
	if (k) memcpy(p, s->mem->data + s->pos, k);
	s->pos += k;
	return (int64_t)k;
}

static bool feed_rewind(feed *s) {
	s->pos = 0;
	return !s->f || ak_cz_seek(s->f, 0);
}

static bool ascii(const char *s, size_t n) {
	for (size_t i = 0; i < n; i++)
		if ((uint8_t)s[i] >= 0x80) return false;
	return true;
}

// -- Tar --

static void octal(uint8_t *p, size_t n, int64_t v) {
	// n - 1 digits, then a NUL.
	p[n - 1] = 0;
	for (size_t i = n - 1; i-- > 0;) {
		p[i] = (uint8_t)('0' + (v & 7));
		v >>= 3;
	}
}

static void record(ak_buf *b, const char *key, const char *v, size_t vn) {
	size_t base = 1 + strlen(key) + 1 + vn + 1, len = base;
	for (;;) {
		char d[24];
		size_t want = base + (size_t)snprintf(d, sizeof d, "%zu", len);
		if (want == len) break;
		len = want;
	}
	ak_buf_printf(b, "%zu %s=", len, key);
	ak_buf_put(b, v, vn);
	ak_buf_u8(b, '\n');
}

static bool tar_block(wr *w, uint8_t *h) {
	memset(h + 148, ' ', 8);
	uint32_t sum = 0;
	for (int i = 0; i < 512; i++) sum += h[i];
	snprintf((char *)h + 148, 8, "%06o", sum);
	h[155] = ' ';
	return w_put(w, h, 512);
}

static bool tar_pad(wr *w, int64_t n) {
	static const uint8_t zeros[512];
	return w_put(w, zeros, (size_t)pad(n));
}

// tar_header writes an entry's header: ustar's when its fields fit, a
// pax header first for what doesn't (a long or non-ASCII name, a size of
// 8 GB or more, a time before 1970).
static bool tar_header(wr *w, const char *name, size_t nn, const char *target, size_t tn, uint8_t type, int64_t mode, int64_t size, int64_t mtime) {
	uint8_t h[512] = {0};
	ak_buf px = {0};
	size_t split = 0;  // where the name goes after ustar's prefix
	bool fits = ascii(name, nn) && nn <= 100;
	if (!fits && ascii(name, nn) && nn <= 256)
		for (size_t i = nn - 1; i > 0; i--)
			if (name[i] == '/' && i <= 155 && nn - i - 1 <= 100 && nn - i - 1 > 0) {
				split = i + 1;
				fits = true;
				break;
			}
	if (!fits) record(&px, "path", name, nn);
	if (tn > 100 || !ascii(target, tn)) record(&px, "linkpath", target, tn);
	int64_t max = (int64_t)1 << 33;  // 11 octal digits
	if (size >= max) {
		char d[24];
		record(&px, "size", d, (size_t)snprintf(d, sizeof d, "%lld", (long long)size));
	}
	if (mtime < 0 || mtime >= max) {
		char d[24];
		record(&px, "mtime", d, (size_t)snprintf(d, sizeof d, "%lld", (long long)mtime));
	}
	bool ok = true;
	if (px.len) {
		// The pax header's own: named as Go's archive/tar names it.
		char pn[100];
		const char *base = name;
		for (size_t i = 0; i < nn; i++)
			if (name[i] == '/') base = name + i + 1;
		size_t bl = 0;
		char b[80];
		for (const char *c = base; c < name + nn && bl < sizeof b - 1; c++) b[bl++] = (uint8_t)*c < 0x80 ? *c : '_';
		b[bl] = 0;
		snprintf(pn, sizeof pn, "PaxHeaders.0/%s", b);
		uint8_t ph[512] = {0};
		memcpy(ph, pn, strlen(pn));
		octal(ph + 100, 8, 0644);
		octal(ph + 108, 8, 0);
		octal(ph + 116, 8, 0);
		octal(ph + 124, 12, (int64_t)px.len);
		octal(ph + 136, 12, 0);
		ph[156] = 'x';
		memcpy(ph + 257, "ustar\0" "00", 8);
		ok = tar_block(w, ph) && w_put(w, px.data, px.len) && tar_pad(w, (int64_t)px.len);
	}
	ak_buf_free(&px);
	// ustar's fields, with as much as fits where pax says the rest.
	if (split) {
		memcpy(h + 345, name, split - 1);
		memcpy(h, name + split, nn - split);
	} else {
		size_t k = 0;
		for (size_t i = 0; i < nn && k < 100; i++) h[k++] = (uint8_t)name[i] < 0x80 ? (uint8_t)name[i] : '_';
	}
	octal(h + 100, 8, mode & 07777);
	octal(h + 108, 8, 0);
	octal(h + 116, 8, 0);
	octal(h + 124, 12, size < max ? size : 0);
	octal(h + 136, 12, mtime >= 0 && mtime < max ? mtime : 0);
	h[156] = type;
	for (size_t i = 0, k = 0; i < tn && k < 100; i++) h[157 + k++] = (uint8_t)target[i] < 0x80 ? (uint8_t)target[i] : '_';
	memcpy(h + 257, "ustar\0" "00", 8);
	octal(h + 329, 8, 0);
	octal(h + 337, 8, 0);
	return ok && tar_block(w, h);
}

static int64_t floor_div(int64_t a, int64_t b) { return a / b - (a % b != 0 && (a < 0) != (b < 0)); }

static bool tar_item(wr *w, const item *it, feed *src, int64_t size) {
	static const uint8_t types[] = {'0', '5', '2', '1', '0'};
	ak_buf name = {0};
	ak_buf_put(&name, it->name, it->name_len);
	if (it->kind == K_DIR) ak_buf_u8(&name, '/');
	int64_t mode = it->mode ? it->mode : it->kind == K_DIR ? 0755 : it->kind == K_SYMLINK ? 0777 : 0644;
	bool link = it->kind == K_SYMLINK || it->kind == K_HARDLINK;
	bool ok = tar_header(w, (const char *)name.data, name.len, link ? it->target : "", link ? it->target_len : 0, types[it->kind], mode, it->kind == K_FILE ? size : 0, floor_div(it->time, 1000000000));
	ak_buf_free(&name);
	if (!ok || it->kind != K_FILE) return ok;
	uint8_t *buf = ak_alloc(CHUNK);
	int64_t left = size;
	while (ok && left > 0) {
		int64_t got = feed_read(src, buf, left > CHUNK ? CHUNK : (size_t)left);
		if (got <= 0) {
			if (got == 0) ak_cz_fail(AK_CZ_OTHER, it->source ? it->source : it->name, "it changed while it was archived");
			ok = false;
			break;
		}
		ok = w_put(w, buf, (size_t)got);
		left -= got;
	}
	ak_free(buf);
	return ok && tar_pad(w, size);
}

// -- Zip --

enum { ZIP64_AT = 0xffffffff };

// zip_local writes (or, at off, rewrites) an entry's local header.
static bool zip_local(wr *w, const item *it, int64_t off, bool rewrite, bool z64, int method, uint32_t crc, int64_t csize, int64_t usize, size_t nn) {
	bool ut = it->time >= 0 && it->time / 1000000000 <= 0xffffffffll;
	size_t xlen = (ut ? 9 : 0) + (z64 ? 20 : 0);
	uint8_t *h = ak_alloc(30 + nn + xlen);
	put32(h, ZIP_LOCAL);
	put16(h + 4, z64 ? 45 : 20);
	put16(h + 6, ascii(it->name, it->name_len) ? 0 : 0x800);
	put16(h + 8, (uint32_t)method);
	put16(h + 10, it->dos & 0xffff);
	put16(h + 12, it->dos >> 16);
	put32(h + 14, crc);
	put32(h + 18, z64 ? ZIP64_AT : (uint32_t)csize);
	put32(h + 22, z64 ? ZIP64_AT : (uint32_t)usize);
	put16(h + 26, (uint32_t)nn);
	put16(h + 28, (uint32_t)xlen);
	memcpy(h + 30, it->name, it->name_len);
	if (it->kind == K_DIR) h[30 + it->name_len] = '/';
	uint8_t *x = h + 30 + nn;
	if (ut) {
		put16(x, 0x5455), put16(x + 2, 5), x[4] = 1, put32(x + 5, (uint32_t)(it->time / 1000000000));
		x += 9;
	}
	if (z64) put16(x, 1), put16(x + 2, 16), put64(x + 4, (uint64_t)usize), put64(x + 12, (uint64_t)csize);
	bool ok = rewrite ? ak_cz_sink_patch(w->sink, off, h, 30 + nn + xlen) : ak_cz_sink_write(w->sink, h, 30 + nn + xlen);
	ak_free(h);
	return ok;
}

// zip_central adds the entry's central header to the directory.
static void zip_central(wr *w, const item *it, int64_t off, int method, uint32_t crc, int64_t csize, int64_t usize, size_t nn) {
	bool z64 = usize >= ZIP64_AT || csize >= ZIP64_AT || off >= ZIP64_AT;
	bool ut = it->time >= 0 && it->time / 1000000000 <= 0xffffffffll;
	size_t xlen = (ut ? 9 : 0) + (z64 ? 28 : 0);
	uint8_t h[46];
	uint32_t type = it->kind == K_DIR ? 0040000 : it->kind == K_SYMLINK ? 0120000 : 0100000;
	int64_t mode = it->mode ? it->mode : it->kind == K_DIR ? 0755 : it->kind == K_SYMLINK ? 0777 : 0644;
	put32(h, ZIP_CENTRAL);
	put16(h + 4, 3 << 8 | (z64 ? 45 : 20));  // made on Unix: the mode's in the attributes
	put16(h + 6, z64 ? 45 : 20);
	put16(h + 8, ascii(it->name, it->name_len) ? 0 : 0x800);
	put16(h + 10, (uint32_t)method);
	put16(h + 12, it->dos & 0xffff);
	put16(h + 14, it->dos >> 16);
	put32(h + 16, crc);
	put32(h + 20, z64 ? ZIP64_AT : (uint32_t)csize);
	put32(h + 24, z64 ? ZIP64_AT : (uint32_t)usize);
	put16(h + 28, (uint32_t)nn);
	put16(h + 30, (uint32_t)xlen);
	put16(h + 32, 0);
	put16(h + 34, 0);
	put16(h + 36, 0);
	put32(h + 38, (type | (uint32_t)(mode & 07777)) << 16 | (it->kind == K_DIR ? 0x10 : 0));
	put32(h + 42, z64 ? ZIP64_AT : (uint32_t)off);
	ak_buf_put(&w->dir, h, 46);
	ak_buf_put(&w->dir, it->name, it->name_len);
	if (it->kind == K_DIR) ak_buf_u8(&w->dir, '/');
	if (ut) {
		uint8_t x[9];
		put16(x, 0x5455), put16(x + 2, 5), x[4] = 1, put32(x + 5, (uint32_t)(it->time / 1000000000));
		ak_buf_put(&w->dir, x, 9);
	}
	if (z64) {
		uint8_t x[28];
		put16(x, 1), put16(x + 2, 24), put64(x + 4, (uint64_t)usize), put64(x + 12, (uint64_t)csize), put64(x + 20, (uint64_t)off);
		ak_buf_put(&w->dir, x, 28);
	}
	w->count++;
}

// zip_store copies the data as it is.
static bool zip_store(wr *w, feed *src, uint32_t *crc, int64_t *n) {
	uint8_t *buf = ak_alloc(CHUNK);
	bool ok = true;
	*crc = 0, *n = 0;
	for (;;) {
		int64_t got = feed_read(src, buf, CHUNK);
		if (got <= 0) {
			ok = got == 0;
			break;
		}
		*crc = ak_cz_crc32(*crc, buf, (size_t)got);
		*n += got;
		if (!(ok = ak_cz_sink_write(w->sink, buf, (size_t)got))) break;
	}
	ak_free(buf);
	return ok;
}

static bool zip_deflate(wr *w, feed *src, uint32_t *crc, int64_t *n) {
	uint8_t *buf = ak_alloc(CHUNK);
	ak_cz_deflate *z = ak_cz_deflate_new(w->sink, AK_CZ_RAW, w->level);
	bool ok = true;
	*crc = 0, *n = 0;
	for (;;) {
		int64_t got = feed_read(src, buf, CHUNK);
		if (got <= 0) {
			ok = got == 0;
			break;
		}
		*crc = ak_cz_crc32(*crc, buf, (size_t)got);
		*n += got;
		if (!(ok = ak_cz_deflate_write(z, buf, (size_t)got))) break;
	}
	ok = ok && ak_cz_deflate_finish(z);
	ak_cz_deflate_free(z);
	ak_free(buf);
	return ok;
}

// zip_item writes an entry: deflated, or stored when deflating doesn't
// make it smaller (or level is 0), its local header rewritten once its
// sizes and CRC-32 are known.
static bool zip_item(wr *w, const item *it, feed *src, int64_t size_hint) {
	size_t nn = it->name_len + (it->kind == K_DIR);
	if (nn > 0xffff) {
		ak_cz_fail(AK_CZ_UNSUPPORTED, it->name, "its name is too long for a zip");
		return false;
	}
	int64_t off = w->sink->pos;
	bool z64 = size_hint >= ZIP64_AT;
	if (!zip_local(w, it, off, false, z64, 0, 0, 0, 0, nn)) return false;
	int64_t start = w->sink->pos, usize = 0;
	uint32_t crc = 0;
	int method = 0;
	bool ok;
	feed link = {0};
	ak_bytes *target = NULL;
	if (it->kind == K_SYMLINK) {
		target = ak_bytes_new(it->target, it->target_len);
		link.mem = target;
		src = &link;
	}
	if (it->kind == K_DIR) {
		ok = true;
	} else if (it->kind == K_FILE && w->level > 0) {
		method = 8;
		ok = zip_deflate(w, src, &crc, &usize);
		if (ok && w->sink->pos - start >= usize) {
			method = 0;
			ok = ak_cz_sink_truncate(w->sink, start) && feed_rewind(src) && zip_store(w, src, &crc, &usize);
		}
	} else {
		ok = zip_store(w, src, &crc, &usize);
	}
	ak_release(target);
	if (!ok) return false;
	int64_t csize = w->sink->pos - start;
	if (!z64 && (usize >= ZIP64_AT || csize >= ZIP64_AT)) {
		ak_cz_fail(AK_CZ_OTHER, it->source ? it->source : it->name, "it changed while it was archived");
		return false;
	}
	if (!zip_local(w, it, off, true, z64, method, crc, csize, usize, nn)) return false;
	zip_central(w, it, off, method, crc, csize, usize, nn);
	return true;
}

static bool zip_finish(wr *w) {
	int64_t dir = w->sink->pos, size = (int64_t)w->dir.len;
	if (!ak_cz_sink_write(w->sink, w->dir.data ? w->dir.data : (const uint8_t *)"", w->dir.len)) return false;
	bool z64 = w->count >= 0xffff || dir >= ZIP64_AT || size >= ZIP64_AT;
	if (z64) {
		uint8_t e[56 + 20];
		int64_t at64 = w->sink->pos;
		put32(e, ZIP64_END);
		put64(e + 4, 44);
		put16(e + 12, 3 << 8 | 45);
		put16(e + 14, 45);
		put32(e + 16, 0);
		put32(e + 20, 0);
		put64(e + 24, w->count);
		put64(e + 32, w->count);
		put64(e + 40, (uint64_t)size);
		put64(e + 48, (uint64_t)dir);
		put32(e + 56, ZIP64_LOC);
		put32(e + 60, 0);
		put64(e + 64, (uint64_t)at64);
		put32(e + 72, 1);
		if (!ak_cz_sink_write(w->sink, e, sizeof e)) return false;
	}
	uint8_t e[22] = {0};
	put32(e, ZIP_END);
	put16(e + 8, z64 ? 0xffff : (uint32_t)w->count);
	put16(e + 10, z64 ? 0xffff : (uint32_t)w->count);
	put32(e + 12, z64 ? ZIP64_AT : (uint32_t)size);
	put32(e + 16, z64 ? ZIP64_AT : (uint32_t)dir);
	return ak_cz_sink_write(w->sink, e, sizeof e);
}

// -- Both --

// item_at reads the i'th entry of what Asuka gave.
static bool item_at(item *it, uint32_t i, const ak_arr *names, const ak_arr *targets, const ak_arr *info, bool w_zip) {
	const ak_str *name = names->data[i].p, *target = targets->data[i].p;
	const ak_val *v = info->data + (size_t)i * INFO_IN;
	*it = (item){.target = target->data, .target_len = target->len, .kind = (int)v[0].i, .mode = v[1].i, .time = v[2].i, .dos = (uint32_t)v[3].i};
	if (it->kind < K_FILE || it->kind > K_OTHER) it->kind = K_OTHER;
	// Names with / between them; what extracting would refuse anywhere
	// isn't written.
	if (!parts(name->data, &it->nb, false) || it->nb.len == 0 || memchr(name->data, 0, name->len)) {
		ak_cz_fail(AK_CZ_UNSAFE, name->data, "it isn't a path inside the archive");
		return false;
	}
	it->name = (const char *)it->nb.data;
	it->name_len = it->nb.len;
	if (it->kind == K_OTHER || (it->kind == K_HARDLINK && w_zip)) {
		ak_cz_fail(AK_CZ_UNSUPPORTED, it->name, it->kind == K_OTHER ? "an entry of kind other can't be written" : "a zip has no hard links");
		return false;
	}
	if (it->kind == K_SYMLINK && !link_inside(it->name, it->target)) {
		ak_cz_fail(AK_CZ_UNSAFE, it->name, "its link's target %s isn't inside the archive", it->target);
		return false;
	}
	return true;
}

static void wr_init(wr *w, ak_cz_sink *s, int64_t format, int64_t level) {
	*w = (wr){.sink = s, .format = format == F_TAR || format == F_TAR_GZ ? (int)format : F_ZIP, .level = level < 0 ? 0 : level > 9 ? 9 : (int)level};
	if (w->format == F_TAR_GZ) w->z = ak_cz_deflate_new(s, AK_CZ_GZIP, w->level);
}

static bool wr_item(wr *w, const item *it, feed *src, int64_t size) {
	return w->format == F_ZIP ? zip_item(w, it, src, size) : tar_item(w, it, src, size);
}

static bool wr_finish(wr *w, bool ok) {
	if (ok && w->format == F_ZIP) ok = zip_finish(w);
	if (ok && w->format != F_ZIP) {
		static const uint8_t end[1024];
		ok = w_put(w, end, sizeof end);
	}
	if (w->z) {
		ok = ok && ak_cz_deflate_finish(w->z);
		ak_cz_deflate_free(w->z);
	}
	ak_buf_free(&w->dir);
	return ok;
}

static bool lengths(uint32_t n, const ak_arr *names, const ak_arr *targets, const ak_arr *third, const ak_arr *info) {
	if (names->len == n && targets->len == n && third->len == n && info->len == (uint64_t)n * INFO_IN) return true;
	ak_throw_msg("archive: the entries' arrays don't agree");
	return false;
}

static ak_val encode_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	const ak_arr *names = ARR(2), *targets = ARR(3), *datas = ARR(4), *info = ARR(5);
	if (!lengths(names->len, names, targets, datas, info)) return (ak_val){.p = NULL};
	ak_cz_sink s = {0};
	wr w;
	wr_init(&w, &s, args[0].i, args[1].i);
	bool ok = true;
	for (uint32_t i = 0; ok && i < names->len; i++) {
		item it;
		ok = item_at(&it, i, names, targets, info, w.format == F_ZIP);
		if (ok) {
			it.data = datas->data[i].p;
			feed src = {.mem = it.data};
			ok = wr_item(&w, &it, &src, it.kind == K_FILE && it.data ? it.data->len : 0);
		}
		ak_buf_free(&it.nb);
	}
	ok = wr_finish(&w, ok);
	ak_val r = {.p = ok ? ak_bytes_new(s.mem.data, s.mem.len) : NULL};
	ak_buf_free(&s.mem);
	return r;
}

static ak_val create_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	const char *path = STR(0)->data;
	const ak_arr *names = ARR(3), *targets = ARR(4), *sources = ARR(5), *info = ARR(6);
	if (!lengths(names->len, names, targets, sources, info)) return (ak_val){.i = 0};
	ak_cz_sink s = {.f = ak_cz_create(path, 0644, NULL)};
	if (!s.f) return (ak_val){.i = 0};
	wr w;
	wr_init(&w, &s, args[1].i, args[2].i);
	bool ok = true;
	for (uint32_t i = 0; ok && i < names->len; i++) {
		item it;
		const ak_str *source = sources->data[i].p;
		feed src = {0};
		int64_t size = 0;
		ok = item_at(&it, i, names, targets, info, w.format == F_ZIP);
		if (ok && it.kind == K_FILE) {
			it.source = source->data;
			ok = (src.f = ak_cz_open(it.source)) != NULL && (size = ak_cz_size(src.f)) >= 0;
		}
		if (ok && it.mode < 0) it.mode = it.kind == K_FILE || it.kind == K_DIR ? file_mode(source->data, it.kind == K_DIR) : 0;
		if (ok) ok = wr_item(&w, &it, &src, size);
		ak_cz_close(src.f);
		ak_buf_free(&it.nb);
	}
	ok = wr_finish(&w, ok);
	ok = ak_cz_close(s.f) && ok;
	if (!ok) ak_cz_remove(path);
	return (ak_val){.i = ok};
}

const ak_lib_entry ak_std_archive[] = {
	{"archive.scanBytes", scan_bytes},
	{"archive.scanFile", scan_file},
	{"archive.scanInfo", info_fn},
	{"archive.scanData", data_fn},
	{"archive.entryFile", entry_file},
	{"archive.extractFile", extract_file},
	{"archive.encodeFiles", encode_fn},
	{"archive.createFile", create_fn},
	{"archive.failed", failed_fn},
	{"archive.failureKind", ak_cz_failure_kind},
	{"archive.failureReason", ak_cz_failure_reason},
	{"archive.failurePath", ak_cz_failure_path},
	{NULL, NULL},
};
