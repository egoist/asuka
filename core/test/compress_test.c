// std/compress's and std/archive's C (core/src/std/compress.c,
// archive.c): what the Asuka tests (std/compress, std/archive) reach less
// easily: streams read a byte at a time, gzip's optional header fields,
// links already in a destination, the permissions extracting keeps.
#define _POSIX_C_SOURCE 200809L
#include "t.h"
#include "std/compress.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

static ak_lib_fn fn(const char *name) {
	for (const ak_lib_entry *const *t = ak_std_libs; *t; t++)
		for (const ak_lib_entry *e = *t; e->name; e++)
			if (strcmp(e->name, name) == 0) return e->fn;
	fprintf(stderr, "no builtin %s\n", name);
	abort();
}

static ak_val S(const char *s) { return (ak_val){.p = ak_str_c(s)}; }
static ak_val I(int64_t i) { return (ak_val){.i = i}; }
static ak_val B(const void *p, size_t n) { return (ak_val){.p = ak_bytes_new(p, n)}; }

// call calls a builtin with args, releasing the references among them
// (refs: a bit per argument).
static ak_val call(const char *name, ak_val *args, uint32_t n, uint32_t refs) {
	ak_val r = fn(name)(args, n);
	for (uint32_t i = 0; i < n; i++)
		if (refs >> i & 1) ak_release(args[i].p);
	return r;
}

static uint8_t *sample(size_t n) {
	uint8_t *p = malloc(n);
	uint32_t x = 1;
	for (size_t i = 0; i < n; i++) {
		x = x * 1103515245 + 12345;
		p[i] = i % 3000 < 2000 ? (uint8_t)('a' + i % 7) : (uint8_t)(x >> 16);
	}
	return p;
}

// Each format, read back a byte at a time and in odd pieces: what's
// pending in the window survives reads of any size.
TEST(inflate_in_pieces) {
	size_t n = 200000;
	uint8_t *data = sample(n);
	for (int format = AK_CZ_GZIP; format <= AK_CZ_RAW; format++) {
		ak_bytes *packed = call("compress.encodeBytes", (ak_val[]){B(data, n), I(format), I(6)}, 3, 1).p;
		REQUIRE(packed);
		for (size_t piece = 1; piece < 70000; piece = piece * 7 + 3) {
			ak_cz_reset();
			ak_cz_src src = {.mem = packed->data, .len = packed->len};
			ak_cz_inflate *z = ak_cz_inflate_new(&src, format, -1);
			uint8_t *out = malloc(n + piece);
			size_t k = 0, got;
			while ((got = ak_cz_inflate_read(z, out + k, piece)) > 0) k += got;
			CHECK(ak_cz_inflate_done(z));
			CHECK_EQ_INT(k, n);
			CHECK(memcmp(out, data, n) == 0);
			CHECK_EQ_INT(ak_cz_inflate_total(z), n);
			ak_cz_inflate_free(z);
			free(out);
		}
		ak_release(packed);
	}
	free(data);
}

// gzip's header with every optional field: extra, name, comment and its
// own CRC (RFC 1952 2.3.1), before a member that's stored.
TEST(gzip_header_fields) {
	uint8_t g[64] = {0x1f, 0x8b, 8, 2 | 4 | 8 | 16, 0, 0, 0, 0, 0, 3};
	size_t k = 10;
	g[k++] = 3, g[k++] = 0, g[k++] = 'x', g[k++] = 'y', g[k++] = 'z';  // FEXTRA
	memcpy(g + k, "a.txt", 6), k += 6;                                   // FNAME
	memcpy(g + k, "hi", 3), k += 3;                                      // FCOMMENT
	g[k++] = 0xab, g[k++] = 0xcd;                                        // FHCRC
	// A stored block: final, 2 bytes, "ok".
	g[k++] = 1, g[k++] = 2, g[k++] = 0, g[k++] = 0xfd, g[k++] = 0xff, g[k++] = 'o', g[k++] = 'k';
	uint32_t crc = ak_cz_crc32(0, "ok", 2);
	for (int i = 0; i < 4; i++) g[k++] = (uint8_t)(crc >> (8 * i));
	g[k++] = 2, g[k++] = 0, g[k++] = 0, g[k++] = 0;
	ak_bytes *out = call("compress.decodeBytes", (ak_val[]){B(g, k), I(AK_CZ_GZIP), I(100)}, 3, 1).p;
	REQUIRE(out);
	CHECK(out->len == 2 && memcmp(out->data, "ok", 2) == 0);
	ak_release(out);
	// Reserved flags set: corrupt.
	g[3] |= 0x20;
	CHECK(call("compress.decodeBytes", (ak_val[]){B(g, k), I(AK_CZ_GZIP), I(100)}, 3, 1).p == NULL);
	CHECK_EQ_INT(call("compress.failureKind", NULL, 0, 0).i, AK_CZ_CORRUPT);
}

// The CRC-32 of "123456789" is its check value (ISO 3309).
TEST(crc32_check) {
	CHECK_EQ_INT(ak_cz_crc32(0, "123456789", 9), 0xcbf43926);
	CHECK_EQ_INT(ak_cz_crc32(ak_cz_crc32(0, "1234", 4), "56789", 5), 0xcbf43926);
}

#if !defined(_WIN32)

static char *path_in(const char *dir, const char *name) {
	size_t n = strlen(dir) + strlen(name) + 2;
	char *p = malloc(n);
	snprintf(p, n, "%s/%s", dir, name);
	return p;
}

static void write_text(const char *path, const char *s) {
	FILE *f = fopen(path, "wb");
	fputs(s, f);
	fclose(f);
}

static char *read_text(const char *path) {
	static char buf[256];
	FILE *f = fopen(path, "rb");
	if (!f) return NULL;
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	return buf;
}

// encode makes an archive of (name, data, kind, mode) entries into path.
typedef struct {
	const char *name, *data;
	int kind, mode;
	const char *target;
} ent;

static bool encode_to(const char *path, int format, const ent *es, int n) {
	ak_arr *names = ak_str_arr(0), *targets = ak_str_arr(0), *datas = ak_arr_new(AK_K_REF, NULL, 0), *info = ak_arr_new(AK_K_INT, NULL, 0);
	for (int i = 0; i < n; i++) {
		ak_str_arr_push(&names, es[i].name, strlen(es[i].name));
		const char *t = es[i].target ? es[i].target : "";
		ak_str_arr_push(&targets, t, strlen(t));
		ak_arr_push(&datas, (ak_val){.p = ak_bytes_new(es[i].data, es[i].data ? strlen(es[i].data) : 0)});
		int64_t v[4] = {es[i].kind, es[i].mode, 1704209444000000000ll, 0x58220000 | (15 << 11)};
		for (int k = 0; k < 4; k++) ak_arr_push(&info, (ak_val){.i = v[k]});
	}
	ak_bytes *b = call("archive.encodeFiles", (ak_val[]){I(format), I(6), {.p = names}, {.p = targets}, {.p = datas}, {.p = info}}, 6, 0x3c).p;
	if (!b) return false;
	FILE *f = fopen(path, "wb");
	fwrite(b->data, 1, b->len, f);
	fclose(f);
	ak_release(b);
	return true;
}

static bool extract(const char *archive, const char *dir) {
	return call("archive.extractFile", (ak_val[]){S(archive), S(dir), I(1 << 20)}, 3, 3).i != 0;
}

// Extracting keeps an entry's permissions (not setuid's) and its time,
// and replaces a link that's where a file goes, never writing through
// it; a directory that's a link (made before) isn't gone into.
TEST(extract_over_links) {
	char *root = ak_fs_temp_dir();
	REQUIRE(root);
	char *out = path_in(root, "out"), *outside = path_in(root, "outside"), *dir = path_in(root, "dest");
	char *z = path_in(root, "a.zip"), *t = path_in(root, "a.tar");
	REQUIRE(ak_fs_make_dirs(out) && ak_fs_make_dirs(dir));
	char *victim = path_in(outside, "victim.txt");
	REQUIRE(ak_fs_make_dirs(outside));
	write_text(victim, "safe");
	ent es[] = {{"run.sh", "#!/bin/sh\n", 0, 04755, NULL}, {"victim.txt", "evil", 0, 0644, NULL}};
	for (int format = 0; format < 3; format++) {
		const char *archive = format == 0 ? z : t;
		REQUIRE(encode_to(archive, format, es, 2));
		// victim.txt in dest is a link to the file outside.
		char *link = path_in(dir, "victim.txt");
		unlink(link);
		REQUIRE(symlink(victim, link) == 0);
		REQUIRE(extract(archive, dir));
		CHECK_EQ_STR(read_text(victim), "safe");
		struct stat st;
		REQUIRE(lstat(link, &st) == 0);
		CHECK(S_ISREG(st.st_mode));
		CHECK_EQ_STR(read_text(link), "evil");
		char *run = path_in(dir, "run.sh");
		REQUIRE(stat(run, &st) == 0);
		mode_t mask = umask(0);
		umask(mask);
		CHECK_EQ_INT(st.st_mode & 07777, 0755 & ~mask);
		CHECK_EQ_INT(st.st_mtime, 1704209444);
		free(run);
		free(link);
	}
	// A directory of dest that's a link outside: an entry under it is refused.
	char *sub = path_in(dir, "sub");
	REQUIRE(symlink(outside, sub) == 0);
	ent under[] = {{"sub/x.txt", "x", 0, 0, NULL}};
	REQUIRE(encode_to(z, 0, under, 1));
	CHECK(!extract(z, dir));
	CHECK_EQ_INT(call("archive.failureKind", NULL, 0, 0).i, AK_CZ_UNSAFE);
	char *x = path_in(outside, "x.txt");
	CHECK(access(x, F_OK) != 0);
	// A file where a directory goes: not a directory.
	char *file = path_in(dir, "file");
	write_text(file, "f");
	free(file);
	ent in_file[] = {{"file/x.txt", "x", 0, 0, NULL}};
	REQUIRE(encode_to(z, 0, in_file, 1));
	CHECK(!extract(z, dir));
	CHECK_EQ_INT(call("archive.failureKind", NULL, 0, 0).i, AK_CZ_NOT_DIR);
	free(x);
	free(sub);
	free(victim);
	CHECK(ak_fs_remove_tree(root));
	ak_free(root);
	free(out), free(outside), free(dir), free(z), free(t);
}

// A link an entry makes stays where it points, inside: one to a sibling
// is made, and what follows through it is refused.
TEST(extract_links_made) {
	char *root = ak_fs_temp_dir();
	REQUIRE(root);
	char *dir = path_in(root, "dest"), *t = path_in(root, "l.tar");
	ent es[] = {{"d/a.txt", "a", 0, 0, NULL}, {"d/l", NULL, 2, 0, "a.txt"}, {"up", NULL, 2, 0, "d/../d"}};
	REQUIRE(encode_to(t, 1, es, 3));
	REQUIRE(extract(t, dir));
	char *l = path_in(dir, "d/l");
	char target[64] = {0};
	CHECK(readlink(l, target, sizeof target - 1) == 5 && strcmp(target, "a.txt") == 0);
	CHECK_EQ_STR(read_text(l), "a");
	free(l);
	CHECK(ak_fs_remove_tree(root));
	ak_free(root);
	free(dir), free(t);
}

// A sink writes, patches what it wrote and drops its end, in a file as in
// memory.
TEST(sink_patch_truncate) {
	char *root = ak_fs_temp_dir();
	REQUIRE(root);
	char *p = path_in(root, "s.bin");
	for (int file = 0; file < 2; file++) {
		ak_cz_reset();
		ak_cz_sink s = {.f = file ? ak_cz_create(p, 0644, NULL) : NULL};
		CHECK(ak_cz_sink_write(&s, "hello world", 11));
		CHECK(ak_cz_sink_patch(&s, 0, "J", 1));
		CHECK(ak_cz_sink_truncate(&s, 5));
		CHECK(ak_cz_sink_write(&s, "!", 1));
		CHECK_EQ_INT(s.pos, 6);
		if (file) {
			CHECK(ak_cz_close(s.f));
			CHECK_EQ_STR(read_text(p), "Jello!");
		} else {
			CHECK(s.mem.len == 6 && memcmp(s.mem.data, "Jello!", 6) == 0);
			ak_buf_free(&s.mem);
		}
	}
	free(p);
	CHECK(ak_fs_remove_tree(root));
	ak_free(root);
}
#endif

T_MAIN()
