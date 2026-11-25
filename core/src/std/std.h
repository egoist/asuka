// std.h: the standard library's packages written in C: the builtins
// their `native fn`s are (lang/LOWERING.md, "The standard library's
// native fns"), named <package>.<fn>. Each package's .asuka (std/<name>)
// declares them and writes the rest of its API on top of them in Asuka.
//
// Like rt/lib.h's, a builtin borrows its arguments and returns an owned
// result, throws with ak_throw, and may run on spawn's worker threads:
// nothing here keeps state that isn't per thread or immutable.
#ifndef AK_STD_H
#define AK_STD_H

#include "rt/lib.h"
#include "rt/unicode.h"

#include <stdio.h>

extern const ak_lib_entry ak_std_time[];
extern const ak_lib_entry ak_std_url[];
extern const ak_lib_entry ak_std_regex[];
extern const ak_lib_entry ak_std_encoding[];
extern const ak_lib_entry ak_std_crypto[];
extern const ak_lib_entry ak_std_crypto_aead[];      // aead.c
extern const ak_lib_entry ak_std_crypto_password[];  // password.c
extern const ak_lib_entry ak_std_crypto_pk[];        // pubkey.c
extern const ak_lib_entry ak_std_crypto_keyfile[];   // keyfile.c
extern const ak_lib_entry ak_std_crypto_keydecrypt[]; // keyfile.c
extern const ak_lib_entry ak_std_crypto_keyenc[];    // keyfile.c
extern const ak_lib_entry ak_std_html[];
extern const ak_lib_entry ak_std_random[];
extern const ak_lib_entry ak_std_fs[];
extern const ak_lib_entry ak_std_crash[];
extern const ak_lib_entry ak_std_update[];
extern const ak_lib_entry ak_std_markdown[];
extern const ak_lib_entry ak_std_os[];
extern const ak_lib_entry ak_std_flag[];
extern const ak_lib_entry ak_std_command[];
extern const ak_lib_entry ak_std_term[];
extern const ak_lib_entry ak_std_signals[];
extern const ak_lib_entry ak_std_toml[];
extern const ak_lib_entry ak_std_csv[];
extern const ak_lib_entry ak_std_compress[];
extern const ak_lib_entry ak_std_compress_checksum[];  // checksum.c
extern const ak_lib_entry ak_std_archive[];
extern const ak_lib_entry ak_std_http[];  // httpserver.c: std/http's server
extern const ak_lib_entry ak_std_log[];
extern const ak_lib_entry ak_std_bigint[];
extern const ak_lib_entry ak_std_net[];
extern const ak_lib_entry ak_std_tls[];  // tls.c: std/net's TLS
extern const ak_lib_entry ak_std_collections[];

// Every package's table, NULL-terminated.
extern const ak_lib_entry *const ak_std_libs[];

// ---- Shared helpers ----

// ak_utf8_decode and ak_utf8_encode: rt/unicode.h.

// ak_os_random fills p with n bytes of the system's randomness (any
// thread); false if the system gave none.
bool ak_os_random(void *p, size_t n);

// An array of strings (the elements' flag set, so arrays compare them).
ak_arr *ak_str_arr(uint32_t cap);
// Pushes a new string of s[0:n] onto *a.
void ak_str_arr_push(ak_arr **a, const char *s, size_t n);

// ak_sha256, ak_sha512 and ak_sha1 hash p into out (32, 64 and 20
// bytes).
void ak_sha256(const void *p, size_t n, uint8_t out[32]);
void ak_sha512(const void *p, size_t n, uint8_t out[64]);
void ak_sha1(const void *p, size_t n, uint8_t out[20]);

// The same over what comes in pieces (a file, streamed): init, update
// with each piece, final.
typedef struct {
	uint32_t h[8];
	uint8_t buf[64];
	size_t n;
	uint64_t len;
} ak_sha256_ctx;
void ak_sha256_init(ak_sha256_ctx *c);
void ak_sha256_update(ak_sha256_ctx *c, const void *p, size_t n);
void ak_sha256_final(ak_sha256_ctx *c, uint8_t out[32]);

typedef struct {
	uint64_t h[8];
	uint8_t buf[128];
	size_t n;
	uint64_t len;
} ak_sha512_ctx;
void ak_sha512_init(ak_sha512_ctx *c);
void ak_sha512_update(ak_sha512_ctx *c, const void *p, size_t n);
void ak_sha512_final(ak_sha512_ctx *c, uint8_t out[64]);

typedef struct {
	uint32_t h[5];
	uint8_t buf[64];
	size_t n;
	uint64_t len;
} ak_sha1_ctx;
void ak_sha1_init(ak_sha1_ctx *c);
void ak_sha1_update(ak_sha1_ctx *c, const void *p, size_t n);
void ak_sha1_final(ak_sha1_ctx *c, uint8_t out[20]);
// SHA-384 is SHA-512's context from its own start: final's first 48 bytes.
void ak_sha384_init(ak_sha512_ctx *c);
void ak_sha384(const void *p, size_t n, uint8_t out[48]);
void ak_md5(const void *p, size_t n, uint8_t out[16]);

// Hashes by kind (std/crypto's Hash, its code), HMAC over them, HKDF's
// expand and PBKDF2 (crypto.c); ak_hash_size is 0 for no kind.
enum { AK_HASH_SHA1 = 1, AK_HASH_SHA256, AK_HASH_SHA384, AK_HASH_SHA512 };
size_t ak_hash_size(int kind);
void ak_hash(int kind, const void *p, size_t n, uint8_t *out);
void ak_hmac(int kind, const uint8_t *key, size_t kn, const uint8_t *m, size_t mn, uint8_t *out);
void ak_hkdf_expand(int kind, const uint8_t *prk, size_t pn, const uint8_t *info, size_t in, uint8_t *out, size_t n);
void ak_pbkdf2(int kind, const uint8_t *pw, size_t pn, const uint8_t *salt, size_t sn, uint64_t iterations, uint8_t *out, size_t n);
// CRC-32 (IEEE's, gzip's) and CRC-32C (Castagnoli's), continuing crc
// (0 to start), as zlib's crc32 (checksum.c). Any thread.
uint32_t ak_crc32(uint32_t crc, const void *p, size_t n);
uint32_t ak_crc32c(uint32_t crc, const void *p, size_t n);
uint32_t ak_crc32_bytewise(uint32_t crc, const void *p, size_t n, bool castagnoli);
extern bool ak_crc_hw_off;  // the tables alone (the tests check both)

// ak_wipe zeroes a secret where the compiler can't leave it out.
void ak_wipe(void *p, size_t n);

// Ed25519 (RFC 8032, ed25519.c): a key pair is a 32-byte seed (the
// private key; Go's and the CLI's 64-byte private keys are the seed and
// the public key) and its 32-byte public key. Verifying is strict as Go's
// crypto/ed25519 is: S below the group's order, A a point on the curve.
void ak_ed25519_public(const uint8_t seed[32], uint8_t pub[32]);
void ak_ed25519_sign(const uint8_t seed[32], const uint8_t pub[32], const uint8_t *m, size_t n, uint8_t sig[64]);
bool ak_ed25519_verify(const uint8_t pub[32], const uint8_t *m, size_t n, const uint8_t sig[64]);

// The app's directories: its files (fs.appDir), its settings (configDir),
// its caches (cacheDir) and its logs (logDir), ASUKA_DIR_*'s order. Each
// is what the host set with asuka_set_dir (asuka_set_app_dir: the app's),
// else ASUKA_DATA, ASUKA_CONFIG, ASUKA_CACHE or ASUKA_LOGS, else the
// platform's place for it.
enum { AK_DIR_APP, AK_DIR_CONFIG, AK_DIR_CACHE, AK_DIR_LOGS, AK_DIR_COUNT };
void ak_fs_set_dir(int kind, const char *path);
void ak_fs_set_app_dir(const char *path);
// ak_fs_dir is kind's path (ak_free it); ak_fs_app_dir the app's.
char *ak_fs_dir(int kind);
char *ak_fs_app_dir(void);
// ak_fs_make_dirs makes path and the directories above it (a UTF-8
// path); false if it can't.
bool ak_fs_make_dirs(const char *path);

// DEFLATE (RFC 1951) and gzip (RFC 1952) read as a stream (inflate.c):
// a section of a file (offset, len bytes; len -1: to its end), or a gzip
// file. ak_inflate_read gives up to n bytes of what it decompresses to:
// 0 at its end, or once it failed (ak_inflate_error says why, NULL when
// it didn't); ak_inflate_done reports whether it ended whole (a gzip
// file's checksum checked).
typedef struct ak_inflate ak_inflate;
ak_inflate *ak_inflate_open(const char *path, int64_t offset, int64_t len);
ak_inflate *ak_gunzip_open(const char *path);
size_t ak_inflate_read(ak_inflate *z, uint8_t *out, size_t n);
const char *ak_inflate_error(const ak_inflate *z);
bool ak_inflate_done(const ak_inflate *z);
uint64_t ak_inflate_total(const ak_inflate *z);
void ak_inflate_close(ak_inflate *z);
// ak_update_fopen is fopen of a UTF-8 path (UTF-16 on Windows).
FILE *ak_update_fopen(const char *path, const char *mode);
// ak_update_valid_path reports whether p is a path inside a tree (Go's
// fs.ValidPath, without "." and backslashes).
bool ak_update_valid_path(const char *p, size_t n);

// platform/crash's record (crash.c): the app's lifecycle (AK_LIFECYCLE_*),
// for its reports' context; and whether the app compiled into the build
// reads its reports (lang/cgen sets it).
void ak_crash_lifecycle(int state);
extern bool ak_app_crash;
// std/os (os.c): the program's arguments (argv[0] the program), which
// a host that doesn't set them reads from the system (a desktop app's);
// a test's process (the runner's tests: exit ends the test, not the
// process; args and stdin are empty unless os.fake gives them), cleared
// with ak_os_reset after each; whether a test called exit, and its code.
void ak_os_set_args(int argc, char **argv);
void ak_os_testing(bool on);
void ak_os_reset(void);
bool ak_os_test_exit(int64_t *code);
// ak_os_getenv is an environment variable's value as os.env sees it
// (os.fake's env: in a test that gave one), NULL when it isn't set
// (ak_free it). In a test without a fake environment, real says whether
// the real one counts: false for what makes a test's output vary
// (FORCE_COLOR, COLUMNS).
char *ak_os_getenv(const char *name, bool real);
// ak_os_test_process reports whether the process is a test's (the
// runner's tests, os.fake): its streams are never terminals.
bool ak_os_test_process(void);
// std/term (term.c): the color choices setColors made, forgotten after
// each test (ak_os_reset calls it).
void ak_term_reset(void);
// ak_term_colors reports whether styled text shows on fd (1 stdout, 2
// stderr), as term.colors decides it.
bool ak_term_colors(int fd);
// ak_os_write writes to stdout (1) or stderr (2) as os.writeTo does:
// kept by FakeOS.run while it runs main.
void ak_os_write(int fd, const void *p, size_t n);
// The user's home directory, as fs.home says it (os.user's too).
ak_str *ak_fs_home(void);
// std/log (log.c): its level, format and where lines go, forgotten after
// each test (ak_os_reset calls it).
void ak_log_reset(void);
// Signals (signals.c). ak_os_signals_program makes the process's
// signals a program's: end(128 + the signal's number) ends it for one no
// os.signals stream reads, as its main's cancellation when main_is_task,
// else at once. ak_os_signals_ended tells Windows' close handler the
// program has ended. ak_os_signals_reset drops a test's streams.
// ak_os_fake_end ends the main FakeOS.run runs with code (os.c).
void ak_os_signals_program(void (*end)(int code), bool main_is_task);
void ak_os_signals_ended(void);
void ak_os_signals_reset(void);
void ak_os_fake_end(int code);
// std/toml (toml.c): text's TOML as the toml-test suite's JSON (its
// tables' keys sorted, its scalars tagged and normalized: core/test's
// toml_test.c), or false and why in err (line:column: why).
bool ak_toml_tagged(const char *text, size_t n, ak_buf *out, char *err, size_t errn);

// std/bigint (bigint.c), for json.c: whether a struct's type is
// std/bigint's BigInt, which JSON and print write as an integer of any
// size (its digits), and reads from one or from a string of one; a
// BigInt's bytes written in decimal; and decimal digits (an optional
// '-', no underscores) as a BigInt's bytes (NULL: not such digits, or
// an error pending). json.c reaches them through ak_link_bigint
// (link.c), NULL in an app without std/bigint, so their C stays out of
// its link.
typedef struct ak_bigint_json {
	bool (*is)(const char *name, size_t n);
	void (*write)(ak_strb *b, const ak_bytes *data);
	ak_bytes *(*read)(const char *s, size_t n);
} ak_bigint_json;
extern const ak_bigint_json ak_bigint_json_impl;
extern const ak_bigint_json *const ak_link_bigint;

// For tests (core/runner): a new empty directory in the system's
// temporary one (ak_free the path; NULL when it can't be made), and
// removing a directory and what's in it.
char *ak_fs_temp_dir(void);
bool ak_fs_remove_tree(const char *path);

#endif
