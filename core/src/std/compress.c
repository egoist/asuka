// compress.c: std/compress's builtins, and what std/archive shares with
// it (compress.h): failures, files, and DEFLATE streams over miniz's
// compressor and decompressor (core/third_party/miniz), read and written
// a piece at a time.
//
//	compress.encodeBytes(data, format, level) -> bytes
//	compress.decodeBytes(data, format, limit) -> bytes?
//	compress.deflateFile(from, to, format, level) -> bool
//	compress.inflateFile(from, to, format, limit) -> bool
//	compress.failureKind() -> int / failureReason() / failurePath() -> string
//
// format is std/compress's Format (0 gzip, 1 zlib, 2 deflate); limit is
// the most bytes a decoding may make (bytes: at most 4 GB). The file
// calls stream: a file of any size takes a few hundred KB.
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#define _FILE_OFFSET_BITS 64
#endif
#include "std/compress.h"

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

// miniz, compiled in without what the core doesn't use; its exported
// names the core's own.
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_MALLOC
#define miniz_def_alloc_func ak_mz_def_alloc_func
#define miniz_def_free_func ak_mz_def_free_func
#define miniz_def_realloc_func ak_mz_def_realloc_func
#define mz_adler32 ak_mz_adler32
#define mz_crc32 ak_mz_crc32
#define mz_free ak_mz_free
#define mz_version ak_mz_version
#define tdefl_compress ak_mz_tdefl_compress
#define tdefl_compress_buffer ak_mz_tdefl_compress_buffer
#define tdefl_compress_mem_to_heap ak_mz_tdefl_compress_mem_to_heap
#define tdefl_compress_mem_to_mem ak_mz_tdefl_compress_mem_to_mem
#define tdefl_compress_mem_to_output ak_mz_tdefl_compress_mem_to_output
#define tdefl_create_comp_flags_from_zip_params ak_mz_tdefl_create_comp_flags_from_zip_params
#define tdefl_get_adler32 ak_mz_tdefl_get_adler32
#define tdefl_get_prev_return_status ak_mz_tdefl_get_prev_return_status
#define tdefl_init ak_mz_tdefl_init
#define tdefl_write_image_to_png_file_in_memory ak_mz_tdefl_write_image_to_png_file_in_memory
#define tdefl_write_image_to_png_file_in_memory_ex ak_mz_tdefl_write_image_to_png_file_in_memory_ex
#define tinfl_decompress ak_mz_tinfl_decompress
#define tinfl_decompress_mem_to_callback ak_mz_tinfl_decompress_mem_to_callback
#define tinfl_decompress_mem_to_heap ak_mz_tinfl_decompress_mem_to_heap
#define tinfl_decompress_mem_to_mem ak_mz_tinfl_decompress_mem_to_mem
#include "../../third_party/miniz/miniz.c"

#define STR(i) ((ak_str *)args[i].p)
#define BYTES(i) ((ak_bytes *)args[i].p)

// ---- Failures ----

static AK_TLS int fail_kind;
static AK_TLS char fail_reason[256];
static AK_TLS char fail_path[1024];

void ak_cz_reset(void) {
	fail_kind = 0;
	fail_reason[0] = fail_path[0] = 0;
}

bool ak_cz_failed(void) { return fail_kind != 0; }

static void vfail(int kind, const char *path, const char *fmt, va_list ap) {
	if (fail_kind) return;
	fail_kind = kind;
	vsnprintf(fail_reason, sizeof fail_reason, fmt, ap);
	snprintf(fail_path, sizeof fail_path, "%s", path ? path : "");
}

void ak_cz_fail(int kind, const char *path, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vfail(kind, path, fmt, ap);
	va_end(ap);
}

#if defined(_WIN32)
void ak_cz_fail_os(const char *path) {
	DWORD e = GetLastError();
	int kind;
	switch (e) {
	case ERROR_FILE_NOT_FOUND:
	case ERROR_PATH_NOT_FOUND:
	case ERROR_INVALID_NAME:
		kind = AK_CZ_NOT_FOUND;
		break;
	case ERROR_ALREADY_EXISTS:
	case ERROR_FILE_EXISTS:
		kind = AK_CZ_EXISTS;
		break;
	case ERROR_ACCESS_DENIED:
	case ERROR_SHARING_VIOLATION:
	case ERROR_PRIVILEGE_NOT_HELD:
		kind = AK_CZ_DENIED;
		break;
	case ERROR_DIRECTORY:
		kind = AK_CZ_NOT_DIR;
		break;
	case ERROR_DIR_NOT_EMPTY:
		kind = AK_CZ_NOT_EMPTY;
		break;
	default:
		kind = AK_CZ_OTHER;
	}
	char msg[160] = "";
	DWORD n = FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, NULL, e, 0, msg, sizeof msg, NULL);
	while (n > 0 && (msg[n - 1] == '\n' || msg[n - 1] == '\r' || msg[n - 1] == '.')) msg[--n] = 0;
	if (n == 0) snprintf(msg, sizeof msg, "error %lu", (unsigned long)e);
	ak_cz_fail(kind, path, "%s", msg);
}
#else
void ak_cz_fail_os(const char *path) {
	int e = errno;
	int kind;
	switch (e) {
	case ENOENT: kind = AK_CZ_NOT_FOUND; break;
	case EEXIST: kind = AK_CZ_EXISTS; break;
	case EACCES:
	case EPERM: kind = AK_CZ_DENIED; break;
	case ENOTDIR: kind = AK_CZ_NOT_DIR; break;
	case EISDIR: kind = AK_CZ_IS_DIR; break;
	case ENOTEMPTY: kind = AK_CZ_NOT_EMPTY; break;
	default: kind = AK_CZ_OTHER;
	}
	ak_cz_fail(kind, path, "%s", strerror(e));
}
#endif

ak_val ak_cz_failure_kind(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.i = fail_kind ? fail_kind : AK_CZ_OTHER};
}

ak_val ak_cz_failure_reason(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(fail_reason[0] ? fail_reason : "failed")};
}

ak_val ak_cz_failure_path(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(fail_path)};
}

// ---- Files ----

struct ak_cz_file {
#if defined(_WIN32)
	HANDLE h;
#else
	int fd;
#endif
	char path[];
};

static ak_cz_file *file_new(const char *path) {
	size_t n = strlen(path);
	ak_cz_file *f = ak_alloc(sizeof *f + n + 1);
	memcpy(f->path, path, n + 1);
	return f;
}

const char *ak_cz_path(const ak_cz_file *f) { return f->path; }

#if defined(_WIN32)

static wchar_t *wide(const char *p) {
	int n = MultiByteToWideChar(CP_UTF8, 0, p, -1, NULL, 0);
	if (n <= 0) n = 1;
	wchar_t *w = ak_alloc(sizeof(wchar_t) * (size_t)n);
	MultiByteToWideChar(CP_UTF8, 0, p, -1, w, n);
	return w;
}

ak_cz_file *ak_cz_open(const char *path) {
	wchar_t *w = wide(path);
	HANDLE h = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	ak_free(w);
	if (h == INVALID_HANDLE_VALUE) {
		ak_cz_fail_os(path);
		return NULL;
	}
	ak_cz_file *f = file_new(path);
	f->h = h;
	return f;
}

static bool same_file(HANDLE a, HANDLE b) {
	BY_HANDLE_FILE_INFORMATION x, y;
	if (!GetFileInformationByHandle(a, &x) || !GetFileInformationByHandle(b, &y)) return false;
	return x.dwVolumeSerialNumber == y.dwVolumeSerialNumber && x.nFileIndexHigh == y.nFileIndexHigh && x.nFileIndexLow == y.nFileIndexLow;
}

ak_cz_file *ak_cz_create(const char *path, int mode, const ak_cz_file *same) {
	AK_UNUSED(mode);
	wchar_t *w = wide(path);
	HANDLE h = CreateFileW(w, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	ak_free(w);
	if (h == INVALID_HANDLE_VALUE) {
		ak_cz_fail_os(path);
		return NULL;
	}
	if (same && same_file(h, same->h)) {
		CloseHandle(h);
		ak_cz_fail(AK_CZ_OTHER, path, "it's the file being read");
		return NULL;
	}
	ak_cz_file *f = file_new(path);
	f->h = h;
	if (!ak_cz_truncate(f, 0)) {
		ak_cz_close(f);
		return NULL;
	}
	return f;
}

int64_t ak_cz_read(ak_cz_file *f, void *p, size_t n) {
	DWORD got;
	if (n > (1u << 30)) n = 1u << 30;
	if (!ReadFile(f->h, p, (DWORD)n, &got, NULL)) {
		ak_cz_fail_os(f->path);
		return -1;
	}
	return got;
}

bool ak_cz_write(ak_cz_file *f, const void *p, size_t n) {
	const uint8_t *b = p;
	while (n > 0) {
		DWORD put, chunk = n > (1u << 30) ? (1u << 30) : (DWORD)n;
		if (!WriteFile(f->h, b, chunk, &put, NULL)) {
			ak_cz_fail_os(f->path);
			return false;
		}
		b += put, n -= put;
	}
	return true;
}

bool ak_cz_seek(ak_cz_file *f, int64_t off) {
	LARGE_INTEGER li;
	li.QuadPart = off;
	if (!SetFilePointerEx(f->h, li, NULL, FILE_BEGIN)) {
		ak_cz_fail_os(f->path);
		return false;
	}
	return true;
}

int64_t ak_cz_size(ak_cz_file *f) {
	LARGE_INTEGER li;
	if (!GetFileSizeEx(f->h, &li)) {
		ak_cz_fail_os(f->path);
		return -1;
	}
	return li.QuadPart;
}

bool ak_cz_truncate(ak_cz_file *f, int64_t len) {
	if (!ak_cz_seek(f, len)) return false;
	if (!SetEndOfFile(f->h)) {
		ak_cz_fail_os(f->path);
		return false;
	}
	return true;
}

void ak_cz_set_modified(ak_cz_file *f, int64_t unix_ns) {
	uint64_t t = (uint64_t)(unix_ns / 100 + 116444736000000000ll);
	FILETIME ft = {.dwLowDateTime = (DWORD)t, .dwHighDateTime = (DWORD)(t >> 32)};
	SetFileTime(f->h, NULL, NULL, &ft);
}

bool ak_cz_close(ak_cz_file *f) {
	if (!f) return true;
	bool ok = CloseHandle(f->h) != 0;
	if (!ok) ak_cz_fail_os(f->path);
	ak_free(f);
	return ok;
}

void ak_cz_remove(const char *path) {
	wchar_t *w = wide(path);
	DeleteFileW(w);
	ak_free(w);
}

#else

#if !defined(O_CLOEXEC)
#define O_CLOEXEC 0
#endif

ak_cz_file *ak_cz_open(const char *path) {
	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		ak_cz_fail_os(path);
		return NULL;
	}
	struct stat st;
	if (fstat(fd, &st) == 0 && S_ISDIR(st.st_mode)) {
		close(fd);
		ak_cz_fail(AK_CZ_IS_DIR, path, "is a directory");
		return NULL;
	}
	ak_cz_file *f = file_new(path);
	f->fd = fd;
	return f;
}

ak_cz_file *ak_cz_create(const char *path, int mode, const ak_cz_file *same) {
	int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, (mode_t)(mode ? mode : 0644));
	if (fd < 0) {
		ak_cz_fail_os(path);
		return NULL;
	}
	struct stat a, b;
	if (fstat(fd, &a) == 0 && S_ISDIR(a.st_mode)) {
		close(fd);
		ak_cz_fail(AK_CZ_IS_DIR, path, "is a directory");
		return NULL;
	}
	if (same && fstat(same->fd, &b) == 0 && fstat(fd, &a) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
		close(fd);
		ak_cz_fail(AK_CZ_OTHER, path, "it's the file being read");
		return NULL;
	}
	ak_cz_file *f = file_new(path);
	f->fd = fd;
	if (!ak_cz_truncate(f, 0)) {
		ak_cz_close(f);
		return NULL;
	}
	return f;
}

int64_t ak_cz_read(ak_cz_file *f, void *p, size_t n) {
	if (n > (1u << 30)) n = 1u << 30;
	for (;;) {
		ssize_t got = read(f->fd, p, n);
		if (got >= 0) return got;
		if (errno == EINTR) continue;
		ak_cz_fail_os(f->path);
		return -1;
	}
}

bool ak_cz_write(ak_cz_file *f, const void *p, size_t n) {
	const uint8_t *b = p;
	while (n > 0) {
		ssize_t put = write(f->fd, b, n > (1u << 30) ? (1u << 30) : n);
		if (put < 0) {
			if (errno == EINTR) continue;
			ak_cz_fail_os(f->path);
			return false;
		}
		b += put, n -= (size_t)put;
	}
	return true;
}

bool ak_cz_seek(ak_cz_file *f, int64_t off) {
	if (lseek(f->fd, (off_t)off, SEEK_SET) < 0) {
		ak_cz_fail_os(f->path);
		return false;
	}
	return true;
}

int64_t ak_cz_size(ak_cz_file *f) {
	struct stat st;
	if (fstat(f->fd, &st) != 0) {
		ak_cz_fail_os(f->path);
		return -1;
	}
	return (int64_t)st.st_size;
}

bool ak_cz_truncate(ak_cz_file *f, int64_t len) {
	if (ftruncate(f->fd, (off_t)len) != 0) {
		ak_cz_fail_os(f->path);
		return false;
	}
	return ak_cz_seek(f, len);
}

void ak_cz_set_modified(ak_cz_file *f, int64_t unix_ns) {
	int64_t s = unix_ns / 1000000000, ns = unix_ns % 1000000000;
	if (ns < 0) s--, ns += 1000000000;
	struct timespec ts[2] = {{.tv_sec = 0, .tv_nsec = UTIME_OMIT}, {.tv_sec = (time_t)s, .tv_nsec = (long)ns}};
	(void)futimens(f->fd, ts);
}

bool ak_cz_close(ak_cz_file *f) {
	if (!f) return true;
	bool ok = close(f->fd) == 0;
	if (!ok) ak_cz_fail_os(f->path);
	ak_free(f);
	return ok;
}

void ak_cz_remove(const char *path) { (void)unlink(path); }

#endif

bool ak_cz_read_full(ak_cz_file *f, void *p, size_t n, const char *what) {
	uint8_t *b = p;
	while (n > 0) {
		int64_t got = ak_cz_read(f, b, n);
		if (got < 0) return false;
		if (got == 0) {
			ak_cz_fail(AK_CZ_CORRUPT, f->path, "%s ends too soon", what);
			return false;
		}
		b += got, n -= (size_t)got;
	}
	return true;
}

// ---- Sources ----

size_t ak_cz_src_read(ak_cz_src *s, uint8_t *p, size_t n) {
	if (!s->f) {
		size_t k = s->len - s->pos < n ? s->len - s->pos : n;
		memcpy(p, s->mem + s->pos, k);
		s->pos += k;
		return k;
	}
	if (s->left >= 0 && (int64_t)n > s->left) n = (size_t)s->left;
	if (n == 0) return 0;
	int64_t got = ak_cz_read(s->f, p, n);
	if (got <= 0) return 0;
	if (s->left > 0) s->left -= got;
	return (size_t)got;
}

// ---- Inflating ----

enum { IN_SIZE = 65536, DICT = TINFL_LZ_DICT_SIZE };
enum { Z_HEADER, Z_BODY, Z_TRAILER, Z_END, Z_FAILED };

struct ak_cz_inflate {
	tinfl_decompressor d;
	ak_cz_src *src;
	int format, state;
	int64_t limit;
	uint64_t total;   // bytes made
	uint32_t crc;     // gzip: the member's CRC-32 so far
	uint32_t member;  // gzip: the member's bytes (mod 2^32)
	bool eof;         // src has nothing more
	size_t in_pos, in_len;
	size_t dict_ofs;           // where tinfl writes next
	size_t pend_at, pend_len;  // what it wrote that isn't read yet
	uint8_t in[IN_SIZE];
	uint8_t dict[DICT];
};

static const char *where(const ak_cz_inflate *z) { return z->src->f ? z->src->f->path : ""; }

// z_fail fails z as corrupt (unless it failed already, reading); false.
static bool z_fail(ak_cz_inflate *z, const char *msg) {
	if (!ak_cz_failed()) ak_cz_fail(AK_CZ_CORRUPT, where(z), "%s", msg);
	z->state = Z_FAILED;
	return false;
}

static void refill(ak_cz_inflate *z) {
	if (z->in_pos < z->in_len || z->eof) return;
	z->in_pos = 0;
	z->in_len = ak_cz_src_read(z->src, z->in, IN_SIZE);
	if (z->in_len == 0) z->eof = true;
}

// in_byte is src's next byte; -1 at its end.
static int in_byte(ak_cz_inflate *z) {
	refill(z);
	if (z->in_pos == z->in_len) return -1;
	return z->in[z->in_pos++];
}

static const char *const too_soon = "the data ends too soon";

// gzip_header reads a member's header (RFC 1952 2.3), its magic read;
// false (failed) when it's broken.
static bool gzip_header(ak_cz_inflate *z) {
	uint8_t h[8];
	for (int i = 0; i < 8; i++) {
		int c = in_byte(z);
		if (c < 0) return z_fail(z, too_soon);
		h[i] = (uint8_t)c;
	}
	if (h[0] != 8) return z_fail(z, "not DEFLATE: gzip's method isn't 8");
	int flg = h[1];
	if (flg & 0xe0) return z_fail(z, "a gzip header's reserved flags are set");
	if (flg & 4) {  // FEXTRA
		int a = in_byte(z), b = in_byte(z);
		if (a < 0 || b < 0) return z_fail(z, too_soon);
		for (int n = a | b << 8; n > 0; n--)
			if (in_byte(z) < 0) return z_fail(z, too_soon);
	}
	for (int bit = 8; bit <= 16; bit <<= 1) {  // FNAME, FCOMMENT
		if (!(flg & bit)) continue;
		int c;
		while ((c = in_byte(z)) > 0) {}
		if (c < 0) return z_fail(z, too_soon);
	}
	if (flg & 2)  // FHCRC
		for (int i = 0; i < 2; i++)
			if (in_byte(z) < 0) return z_fail(z, too_soon);
	z->crc = 0;
	z->member = 0;
	tinfl_init(&z->d);
	z->state = Z_BODY;
	return true;
}

static bool magic(ak_cz_inflate *z) {
	int a = in_byte(z), b = in_byte(z);
	if (a == 0x1f && b == 0x8b) return true;
	return z_fail(z, a < 0 || b < 0 ? too_soon : "not gzip: its first bytes aren't 1f 8b");
}

// gzip_trailer checks a member's CRC-32 and size, then reads the next
// member if one follows (zeros after the last are padding).
static bool gzip_trailer(ak_cz_inflate *z) {
	uint8_t t[8];
	for (int i = 0; i < 8; i++) {
		int c = in_byte(z);
		if (c < 0) return z_fail(z, too_soon);
		t[i] = (uint8_t)c;
	}
	uint32_t crc = (uint32_t)t[0] | (uint32_t)t[1] << 8 | (uint32_t)t[2] << 16 | (uint32_t)t[3] << 24;
	uint32_t size = (uint32_t)t[4] | (uint32_t)t[5] << 8 | (uint32_t)t[6] << 16 | (uint32_t)t[7] << 24;
	if (crc != z->crc) return z_fail(z, "gzip's checksum doesn't match");
	if (size != z->member) return z_fail(z, "gzip's size doesn't match");
	int c = in_byte(z);
	if (c == 0x1f) {
		if (in_byte(z) != 0x8b) return z_fail(z, "data after the gzip stream");
		z->state = Z_HEADER;
		return true;
	}
	while (c == 0) c = in_byte(z);
	if (c >= 0) return z_fail(z, "data after the gzip stream");
	z->state = Z_END;
	return true;
}

ak_cz_inflate *ak_cz_inflate_new(ak_cz_src *src, int format, int64_t limit) {
	ak_cz_inflate *z = ak_alloc(sizeof *z);
	z->src = src;
	z->format = format;
	z->limit = limit;
	tinfl_init(&z->d);
	if (format == AK_CZ_GZIP) {
		if (magic(z)) z->state = Z_HEADER;
	} else {
		z->state = Z_BODY;
	}
	return z;
}

void ak_cz_inflate_free(ak_cz_inflate *z) { ak_free(z); }
bool ak_cz_inflate_done(const ak_cz_inflate *z) { return z->state == Z_END && z->pend_len == 0; }
uint64_t ak_cz_inflate_total(const ak_cz_inflate *z) { return z->total; }

static void body(ak_cz_inflate *z) {
	refill(z);
	if (ak_cz_failed()) {
		z_fail(z, "");
		return;
	}
	size_t in_n = z->in_len - z->in_pos, out_n = DICT - z->dict_ofs;
	mz_uint32 flags = z->eof ? 0 : TINFL_FLAG_HAS_MORE_INPUT;
	if (z->format == AK_CZ_ZLIB) flags |= TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_COMPUTE_ADLER32;
	tinfl_status st = tinfl_decompress(&z->d, z->in + z->in_pos, &in_n, z->dict, z->dict + z->dict_ofs, &out_n, flags);
	z->in_pos += in_n;
	z->pend_at = z->dict_ofs;
	z->pend_len = out_n;
	z->dict_ofs = (z->dict_ofs + out_n) & (DICT - 1);
	if (out_n) {
		z->total += out_n;
		if (z->limit >= 0 && z->total > (uint64_t)z->limit) {
			ak_cz_fail(AK_CZ_TOO_LARGE, where(z), "it decompresses to more than %lld bytes", (long long)z->limit);
			z->state = Z_FAILED;
			z->pend_len = 0;
			return;
		}
		if (z->format == AK_CZ_GZIP) {
			z->crc = ak_cz_crc32(z->crc, z->dict + z->pend_at, out_n);
			z->member += (uint32_t)out_n;
		}
	}
	switch (st) {
	case TINFL_STATUS_DONE:
		z->state = z->format == AK_CZ_GZIP ? Z_TRAILER : Z_END;
		break;
	case TINFL_STATUS_NEEDS_MORE_INPUT:
		if (z->eof) z_fail(z, too_soon);
		break;
	case TINFL_STATUS_HAS_MORE_OUTPUT:
		break;
	case TINFL_STATUS_ADLER32_MISMATCH:
		z_fail(z, "zlib's checksum doesn't match");
		break;
	case TINFL_STATUS_FAILED_CANNOT_MAKE_PROGRESS:
		z_fail(z, too_soon);
		break;
	default:
		z_fail(z, z->format == AK_CZ_ZLIB && z->total == 0 ? "not zlib, or not DEFLATE" : "not DEFLATE: a block is broken");
	}
}

size_t ak_cz_inflate_read(ak_cz_inflate *z, uint8_t *p, size_t n) {
	size_t k = 0;
	while (k < n) {
		if (z->pend_len) {
			size_t m = z->pend_len < n - k ? z->pend_len : n - k;
			memcpy(p + k, z->dict + z->pend_at, m);
			z->pend_at += m, z->pend_len -= m, k += m;
			continue;
		}
		if (z->state == Z_HEADER) gzip_header(z);
		else if (z->state == Z_BODY) body(z);
		else if (z->state == Z_TRAILER) gzip_trailer(z);
		else break;
	}
	return k;
}

// ---- Sinks ----

static const char *sink_path(const ak_cz_sink *s) { return s->f ? s->f->path : ""; }

bool ak_cz_sink_write(ak_cz_sink *s, const void *p, size_t n) {
	if (s->f) {
		if (!ak_cz_write(s->f, p, n)) return false;
	} else {
		if (s->mem.len + n > UINT32_MAX) {
			ak_cz_fail(AK_CZ_TOO_LARGE, "", "the result would be larger than 4 GB, more than bytes hold");
			return false;
		}
		ak_buf_put(&s->mem, p, n);
	}
	s->pos += (int64_t)n;
	return true;
}

bool ak_cz_sink_patch(ak_cz_sink *s, int64_t off, const void *p, size_t n) {
	if (!s->f) {
		memcpy(s->mem.data + off, p, n);
		return true;
	}
	return ak_cz_seek(s->f, off) && ak_cz_write(s->f, p, n) && ak_cz_seek(s->f, s->pos);
}

bool ak_cz_sink_truncate(ak_cz_sink *s, int64_t len) {
	if (s->f && !ak_cz_truncate(s->f, len)) return false;
	if (!s->f) s->mem.len = (size_t)len;
	s->pos = len;
	return true;
}

// ---- Deflating ----

struct ak_cz_deflate {
	tdefl_compressor c;
	ak_cz_sink *to;
	int format;
	uint32_t crc, size;
	bool ok;
};

static mz_bool put(const void *p, int len, void *user) {
	ak_cz_deflate *z = user;
	z->ok = z->ok && ak_cz_sink_write(z->to, p, (size_t)len);
	return z->ok;
}

ak_cz_deflate *ak_cz_deflate_new(ak_cz_sink *to, int format, int level) {
	if (level < 0) level = 0;
	if (level > 9) level = 9;
	ak_cz_deflate *z = ak_alloc(sizeof *z);
	z->to = to;
	z->format = format;
	z->ok = true;
	int flags = (int)tdefl_create_comp_flags_from_zip_params(level, format == AK_CZ_ZLIB ? 15 : -15, 0);
	tdefl_init(&z->c, put, z, flags);
	if (format == AK_CZ_GZIP) {
		// No name and no time (gzip -n's), so the same bytes make the
		// same file; XFL says the level, OS "unknown" (RFC 1952).
		uint8_t h[10] = {0x1f, 0x8b, 8, 0, 0, 0, 0, 0, level == 9 ? 2 : level == 1 ? 4 : 0, 255};
		z->ok = ak_cz_sink_write(to, h, sizeof h);
	}
	return z;
}

bool ak_cz_deflate_write(ak_cz_deflate *z, const void *p, size_t n) {
	if (!z->ok || n == 0) return z->ok;
	if (z->format == AK_CZ_GZIP) {
		z->crc = ak_cz_crc32(z->crc, p, n);
		z->size += (uint32_t)n;
	}
	const uint8_t *b = p;
	while (n > 0 && z->ok) {
		size_t chunk = n > (1u << 30) ? (1u << 30) : n;
		if (tdefl_compress_buffer(&z->c, b, chunk, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY) z->ok = false;
		b += chunk, n -= chunk;
	}
	if (!z->ok && !ak_cz_failed()) ak_cz_fail(AK_CZ_OTHER, sink_path(z->to), "compressing failed");
	return z->ok;
}

bool ak_cz_deflate_finish(ak_cz_deflate *z) {
	if (z->ok && tdefl_compress_buffer(&z->c, NULL, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE) z->ok = false;
	if (z->ok && z->format == AK_CZ_GZIP) {
		uint8_t t[8] = {(uint8_t)z->crc, (uint8_t)(z->crc >> 8), (uint8_t)(z->crc >> 16), (uint8_t)(z->crc >> 24),
		                (uint8_t)z->size, (uint8_t)(z->size >> 8), (uint8_t)(z->size >> 16), (uint8_t)(z->size >> 24)};
		z->ok = ak_cz_sink_write(z->to, t, sizeof t);
	}
	if (!z->ok && !ak_cz_failed()) ak_cz_fail(AK_CZ_OTHER, sink_path(z->to), "compressing failed");
	return z->ok;
}

void ak_cz_deflate_free(ak_cz_deflate *z) { ak_free(z); }

uint32_t ak_cz_crc32(uint32_t crc, const void *p, size_t n) { return (uint32_t)mz_crc32(crc, p, n); }

// ---- The builtins ----

static int format_of(int64_t f) { return f >= AK_CZ_GZIP && f <= AK_CZ_RAW ? (int)f : AK_CZ_GZIP; }

static ak_val encode_bytes(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	const ak_bytes *data = BYTES(0);
	ak_cz_sink s = {0};
	ak_cz_deflate *z = ak_cz_deflate_new(&s, format_of(args[1].i), (int)args[2].i);
	bool ok = ak_cz_deflate_write(z, data ? data->data : NULL, data ? data->len : 0) && ak_cz_deflate_finish(z);
	ak_cz_deflate_free(z);
	if (!ok) {
		ak_buf_free(&s.mem);
		ak_throw_msg("compress: %s", fail_reason);
		return (ak_val){.p = NULL};
	}
	ak_bytes *out = ak_bytes_new(s.mem.data, s.mem.len);
	ak_buf_free(&s.mem);
	return (ak_val){.p = out};
}

static int64_t mem_limit(int64_t limit) { return limit < 0 || limit > UINT32_MAX ? UINT32_MAX : limit; }

static ak_val decode_bytes(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	const ak_bytes *data = BYTES(0);
	ak_cz_src src = {.mem = data ? data->data : NULL, .len = data ? data->len : 0};
	ak_cz_inflate *z = ak_cz_inflate_new(&src, format_of(args[1].i), mem_limit(args[2].i));
	ak_buf b = {0};
	for (;;) {
		ak_buf_reserve(&b, 65536);
		size_t got = ak_cz_inflate_read(z, b.data + b.len, b.cap - b.len);
		if (got == 0) break;
		b.len += got;
	}
	bool ok = ak_cz_inflate_done(z);
	ak_cz_inflate_free(z);
	if (!ok) {
		ak_buf_free(&b);
		return (ak_val){.p = NULL};
	}
	ak_bytes *out = ak_bytes_new(b.data, b.len);
	ak_buf_free(&b);
	return (ak_val){.p = out};
}

enum { CHUNK = 1 << 16 };

static ak_val deflate_file(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	const char *from = STR(0)->data, *to = STR(1)->data;
	ak_cz_file *in = ak_cz_open(from);
	if (!in) return (ak_val){.i = 0};
	ak_cz_sink s = {.f = ak_cz_create(to, 0644, in)};
	if (!s.f) {
		ak_cz_close(in);
		return (ak_val){.i = 0};
	}
	ak_cz_deflate *z = ak_cz_deflate_new(&s, format_of(args[2].i), (int)args[3].i);
	uint8_t *buf = ak_alloc(CHUNK);
	bool ok = true;
	for (;;) {
		int64_t got = ak_cz_read(in, buf, CHUNK);
		if (got < 0) ok = false;
		if (got <= 0) break;
		if (!(ok = ak_cz_deflate_write(z, buf, (size_t)got))) break;
	}
	ok = ok && ak_cz_deflate_finish(z);
	ak_free(buf);
	ak_cz_deflate_free(z);
	ak_cz_close(in);
	ok = ak_cz_close(s.f) && ok;
	if (!ok) ak_cz_remove(to);
	return (ak_val){.i = ok};
}

static ak_val inflate_file(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_cz_reset();
	const char *from = STR(0)->data, *to = STR(1)->data;
	ak_cz_file *in = ak_cz_open(from);
	if (!in) return (ak_val){.i = 0};
	ak_cz_file *out = ak_cz_create(to, 0644, in);
	if (!out) {
		ak_cz_close(in);
		return (ak_val){.i = 0};
	}
	ak_cz_src src = {.f = in, .left = -1};
	ak_cz_inflate *z = ak_cz_inflate_new(&src, format_of(args[2].i), args[3].i < 0 ? -1 : args[3].i);
	uint8_t *buf = ak_alloc(CHUNK);
	bool ok = true;
	for (;;) {
		size_t got = ak_cz_inflate_read(z, buf, CHUNK);
		if (got == 0) break;
		if (!(ok = ak_cz_write(out, buf, got))) break;
	}
	ok = ok && ak_cz_inflate_done(z);
	ak_free(buf);
	ak_cz_inflate_free(z);
	ak_cz_close(in);
	ok = ak_cz_close(out) && ok;
	if (!ok) ak_cz_remove(to);
	return (ak_val){.i = ok};
}

const ak_lib_entry ak_std_compress[] = {
	{"compress.encodeBytes", encode_bytes},
	{"compress.decodeBytes", decode_bytes},
	{"compress.deflateFile", deflate_file},
	{"compress.inflateFile", inflate_file},
	{"compress.failureKind", ak_cz_failure_kind},
	{"compress.failureReason", ak_cz_failure_reason},
	{"compress.failurePath", ak_cz_failure_path},
	{NULL, NULL},
};
