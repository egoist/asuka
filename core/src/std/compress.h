// compress.h: what std/compress (compress.c) and std/archive (archive.c)
// share: their failures, files read and written with 64-bit offsets, and
// DEFLATE streams (miniz's, compiled into compress.c) read and written a
// piece at a time, so a file's archive never needs all of it in memory.
// Everything here is safe on any thread: the builtins run in spawn { }.
#ifndef AK_STD_COMPRESS_H
#define AK_STD_COMPRESS_H

#include "std/std.h"

// ---- Failures ----
//
// A builtin that fails returns its failure value (none, false) and leaves
// why in this thread's failure: its kind (std/fs's FAIL_* numbers for
// what the file system said, then the packages' own), what it says, and
// the path (or the entry's name) it concerns. The Asuka side throws an
// fs.FsError or the package's own error of it. The first failure since
// ak_cz_reset is kept: what fails after it (closing, removing what was
// half made) doesn't hide it.
enum {
	AK_CZ_OTHER = 1,
	AK_CZ_NOT_FOUND,
	AK_CZ_EXISTS,
	AK_CZ_DENIED,
	AK_CZ_NOT_DIR,
	AK_CZ_IS_DIR,
	AK_CZ_NOT_EMPTY,
	AK_CZ_CORRUPT = 10,  // not the format it should be
	AK_CZ_TOO_LARGE,     // past the caller's limit
	AK_CZ_UNSAFE,        // an entry's path outside the destination
	AK_CZ_UNSUPPORTED,   // a format's feature not read or written here
};

void ak_cz_reset(void);
void ak_cz_fail(int kind, const char *path, const char *fmt, ...) AK_PRINTF(3, 4);
// ak_cz_fail_os fails with what the system said of its last call
// (errno, GetLastError).
void ak_cz_fail_os(const char *path);
bool ak_cz_failed(void);
// The builtins <package>.failureKind() -> int, failureReason() -> string
// and failurePath() -> string, in both packages' tables.
ak_val ak_cz_failure_kind(ak_val *args, uint32_t n);
ak_val ak_cz_failure_reason(ak_val *args, uint32_t n);
ak_val ak_cz_failure_path(ak_val *args, uint32_t n);

// ---- Files ----

typedef struct ak_cz_file ak_cz_file;

// ak_cz_open opens the file at path (UTF-8) to read; ak_cz_create makes
// it (or empties it) to write, with mode's permissions where the system
// has them. NULL when it can't (failed). Unless same is NULL, a file
// that's same (the one being read) isn't emptied: that fails instead.
ak_cz_file *ak_cz_open(const char *path);
ak_cz_file *ak_cz_create(const char *path, int mode, const ak_cz_file *same);
// Up to n bytes; 0 at the end, -1 failed.
int64_t ak_cz_read(ak_cz_file *f, void *p, size_t n);
// ak_cz_read_full reads n bytes or fails (a file that ends first is
// corrupt, what says so).
bool ak_cz_read_full(ak_cz_file *f, void *p, size_t n, const char *what);
bool ak_cz_write(ak_cz_file *f, const void *p, size_t n);
bool ak_cz_seek(ak_cz_file *f, int64_t off);
int64_t ak_cz_size(ak_cz_file *f);  // -1 failed
bool ak_cz_truncate(ak_cz_file *f, int64_t len);
// ak_cz_set_modified sets the file's modification time (ns since 1970);
// where the system can't, it's left as it is.
void ak_cz_set_modified(ak_cz_file *f, int64_t unix_ns);
// ak_cz_close closes f (NULL is fine); false if what was written didn't
// reach the file.
bool ak_cz_close(ak_cz_file *f);
const char *ak_cz_path(const ak_cz_file *f);
// ak_cz_remove removes the file at path, if it can (what a failure made
// half of).
void ak_cz_remove(const char *path);

// ---- Reading: a source, and what it inflates to ----

// A source is bytes in memory or a section of a file (left -1: to its
// end), read from its start on.
typedef struct {
	const uint8_t *mem;
	size_t len, pos;
	ak_cz_file *f;
	int64_t left;
} ak_cz_src;

// Up to n bytes; 0 at its end, or once it failed (ak_cz_failed).
size_t ak_cz_src_read(ak_cz_src *s, uint8_t *p, size_t n);

// The formats, numbered as std/compress's Format.
enum { AK_CZ_GZIP, AK_CZ_ZLIB, AK_CZ_RAW };

typedef struct ak_cz_inflate ak_cz_inflate;

// ak_cz_inflate_new reads what src decompresses to, in format (gzip: one
// member or several in a row). Past limit bytes (-1: none) it fails, too
// large; what isn't the format is corrupt.
ak_cz_inflate *ak_cz_inflate_new(ak_cz_src *src, int format, int64_t limit);
// Up to n bytes of what it makes; 0 at its end (ak_cz_inflate_done), or
// once it failed.
size_t ak_cz_inflate_read(ak_cz_inflate *z, uint8_t *p, size_t n);
bool ak_cz_inflate_done(const ak_cz_inflate *z);
uint64_t ak_cz_inflate_total(const ak_cz_inflate *z);
void ak_cz_inflate_free(ak_cz_inflate *z);

// ---- Writing: a sink, and a deflater into one ----

// A sink is a file, or bytes in memory (mem, no more than 4 GB: what
// bytes hold); pos is where the next byte goes.
typedef struct {
	ak_cz_file *f;
	ak_buf mem;
	int64_t pos;
} ak_cz_sink;

bool ak_cz_sink_write(ak_cz_sink *s, const void *p, size_t n);
// ak_cz_sink_patch writes p over what's at off, before pos.
bool ak_cz_sink_patch(ak_cz_sink *s, int64_t off, const void *p, size_t n);
// ak_cz_sink_truncate drops what's past len; pos is len.
bool ak_cz_sink_truncate(ak_cz_sink *s, int64_t len);

typedef struct ak_cz_deflate ak_cz_deflate;

// ak_cz_deflate_new compresses what's written to it into to, in format,
// at level (0 to 9). ak_cz_deflate_finish ends the stream (gzip's and
// zlib's checksums). Both false when to failed.
ak_cz_deflate *ak_cz_deflate_new(ak_cz_sink *to, int format, int level);
bool ak_cz_deflate_write(ak_cz_deflate *z, const void *p, size_t n);
bool ak_cz_deflate_finish(ak_cz_deflate *z);
void ak_cz_deflate_free(ak_cz_deflate *z);

// CRC-32 (ISO 3309, gzip's and zip's), continued from crc (0 to start).
uint32_t ak_cz_crc32(uint32_t crc, const void *p, size_t n);

#endif
