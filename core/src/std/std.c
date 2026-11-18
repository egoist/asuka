// std.c: the packages' tables, and what several of them share.
#include "std/std.h"

const ak_lib_entry *const ak_std_libs[] = {
	ak_std_time,
	ak_std_url,
	ak_std_regex,
	ak_std_encoding,
	ak_std_crypto,
	ak_std_html,
	ak_std_random,
	ak_std_fs,
	ak_std_crash,
	ak_std_update,
	ak_std_markdown,
	ak_std_os,
	ak_std_flag,
	ak_std_command,
	ak_std_term,
	ak_std_compress,
	ak_std_archive,
	NULL,
};

uint32_t ak_utf8_decode(const char *str, size_t n, size_t *i) {
	const uint8_t *p = (const uint8_t *)str + *i;
	size_t left = n - *i;
	uint8_t c = p[0];
	if (c < 0x80) {
		*i += 1;
		return c;
	}
	size_t len;
	uint32_t min;
	if ((c & 0xe0) == 0xc0) len = 2, min = 0x80;
	else if ((c & 0xf0) == 0xe0) len = 3, min = 0x800;
	else if ((c & 0xf8) == 0xf0) len = 4, min = 0x10000;
	else goto bad;
	if (left < len) goto bad;
	uint32_t cp = c & (0x7f >> len);
	for (size_t k = 1; k < len; k++) {
		if ((p[k] & 0xc0) != 0x80) goto bad;
		cp = cp << 6 | (p[k] & 0x3f);
	}
	if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) goto bad;
	*i += len;
	return cp;
bad:
	*i += 1;
	return 0xfffd;
}

size_t ak_utf8_encode(uint32_t c, char *out) {
	if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) c = 0xfffd;
	if (c < 0x80) {
		out[0] = (char)c;
		return 1;
	}
	if (c < 0x800) {
		out[0] = (char)(0xc0 | c >> 6);
		out[1] = (char)(0x80 | (c & 0x3f));
		return 2;
	}
	if (c < 0x10000) {
		out[0] = (char)(0xe0 | c >> 12);
		out[1] = (char)(0x80 | (c >> 6 & 0x3f));
		out[2] = (char)(0x80 | (c & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | c >> 18);
	out[1] = (char)(0x80 | (c >> 12 & 0x3f));
	out[2] = (char)(0x80 | (c >> 6 & 0x3f));
	out[3] = (char)(0x80 | (c & 0x3f));
	return 4;
}

ak_arr *ak_str_arr(uint32_t cap) {
	ak_arr *a = ak_arr_new(AK_K_REF, NULL, cap);
	a->h.flags = 1;
	return a;
}

void ak_str_arr_push(ak_arr **a, const char *s, size_t n) {
	ak_arr_push(a, (ak_val){.p = ak_str_new(s, n)});
}
