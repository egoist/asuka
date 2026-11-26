#include "ops/ops.h"

#include <stdio.h>
#include <stdlib.h>

static const char *const op_names[AK_OP_COUNT] = {NULL, "Create", "Destroy", "Insert", "Remove", "SetProp", "SetFrame", "Listen", "Unlisten", "Command", "SetRoot"};

const char *ak_op_name(uint8_t code) { return code < AK_OP_COUNT ? op_names[code] : NULL; }

const char *ak_err_string(int err) {
	switch (err) {
	case AK_OK: return "ok";
	case AK_ERR_SHORT: return "ops: truncated batch";
	case AK_ERR_BAD_CODE: return "ops: bad op code";
	case AK_ERR_BAD_TYPE: return "ops: bad value type";
	case AK_ERR_TRAILING: return "ops: bytes after the end";
	case AK_ERR_BAD_EVENT: return "ops: unknown event kind";
	}
	return "ops: unknown error";
}

// Colors.

bool ak_color_semantic(ak_color c, uint16_t *out) {
	if (c >> 32 != 1) return false;
	if (out) *out = (uint16_t)c;
	return true;
}

bool ak_color_channels(ak_color c, uint8_t *r, uint8_t *g, uint8_t *b, uint8_t *a) {
	if (c >> 32 != 2) return false;
	uint32_t v = (uint32_t)c;
	*r = (uint8_t)(v >> 24);
	*g = (uint8_t)(v >> 16);
	*b = (uint8_t)(v >> 8);
	*a = (uint8_t)v;
	return true;
}

uint8_t ak_color_alpha(ak_color c) {
	switch (c >> 32) {
	case 1: return (uint8_t)(255 - (uint8_t)(c >> 24));
	case 2: return (uint8_t)c;
	}
	return 255;
}

ak_color ak_color_opacity(ak_color c, float opacity) {
	if (c == 0) return 0;
	float o = opacity > 0 ? opacity : 0;  // NaN too
	if (o > 1) o = 1;
	uint8_t a = (uint8_t)((float)ak_color_alpha(c) * o + 0.5f);
	switch (c >> 32) {
	case 1: return AK_COLOR_SEMANTIC | (uint64_t)(255 - a) << 24 | (uint16_t)c;
	case 2: return (c & ~(uint64_t)0xff) | a;
	}
	return c;
}

// The batch.

void ak_batch_reset(ak_batch *b) {
	b->buf.len = 0;
	b->count = 0;
	b->mark = 0;
}

void ak_batch_free(ak_batch *b) {
	ak_buf_free(&b->buf);
	b->count = 0;
	b->mark = 0;
}

static inline void op(ak_batch *b, uint8_t code, ak_id id) {
	AK_ASSERT(b->mark == 0);  // not between a _begin and its _end
	if (b->buf.len == 0) ak_buf_u32(&b->buf, 0);  // the count, written by encode
	ak_buf_reserve(&b->buf, 1 + 4 + 2 + 1 + 16);
	ak_buf_u8(&b->buf, code);
	ak_buf_u32(&b->buf, id);
	b->count++;
}

const uint8_t *ak_batch_encode(ak_batch *b, size_t *len) {
	AK_ASSERT(b->mark == 0);
	if (b->buf.len == 0) ak_buf_u32(&b->buf, 0);
	uint32_t n = b->count;
	uint8_t *p = b->buf.data;
	p[0] = (uint8_t)n;
	p[1] = (uint8_t)(n >> 8);
	p[2] = (uint8_t)(n >> 16);
	p[3] = (uint8_t)(n >> 24);
	*len = b->buf.len;
	return b->buf.data;
}

void ak_batch_create(ak_batch *b, ak_id id, ak_view_kind k) {
	op(b, AK_OP_CREATE, id);
	ak_buf_u16(&b->buf, k);
}

void ak_batch_destroy(ak_batch *b, ak_id id) { op(b, AK_OP_DESTROY, id); }

void ak_batch_insert(ak_batch *b, ak_id parent, ak_id child, uint32_t index) {
	op(b, AK_OP_INSERT, parent);
	ak_buf_u32(&b->buf, child);
	ak_buf_u32(&b->buf, index);
}

void ak_batch_remove(ak_batch *b, ak_id parent, ak_id child) {
	op(b, AK_OP_REMOVE, parent);
	ak_buf_u32(&b->buf, child);
}

void ak_batch_set_root(ak_batch *b, ak_id id) { op(b, AK_OP_SET_ROOT, id); }

void ak_batch_listen(ak_batch *b, ak_id id, ak_event_kind e) {
	op(b, AK_OP_LISTEN, id);
	ak_buf_u16(&b->buf, e);
}

void ak_batch_unlisten(ak_batch *b, ak_id id, ak_event_kind e) {
	op(b, AK_OP_UNLISTEN, id);
	ak_buf_u16(&b->buf, e);
}

void ak_batch_set_frame(ak_batch *b, ak_id id, float x, float y, float w, float h) {
	op(b, AK_OP_SET_FRAME, id);
	ak_buf_f32(&b->buf, x);
	ak_buf_f32(&b->buf, y);
	ak_buf_f32(&b->buf, w);
	ak_buf_f32(&b->buf, h);
}

static inline void prop(ak_batch *b, ak_id id, ak_prop p, uint8_t type) {
	op(b, AK_OP_SET_PROP, id);
	ak_buf_u16(&b->buf, p);
	ak_buf_u8(&b->buf, type);
}

void ak_batch_set_bool(ak_batch *b, ak_id id, ak_prop p, bool v) {
	prop(b, id, p, AK_T_BOOL);
	ak_buf_u8(&b->buf, v ? 1 : 0);
}

void ak_batch_set_int(ak_batch *b, ak_id id, ak_prop p, int64_t v) {
	prop(b, id, p, AK_T_INT);
	ak_buf_u64(&b->buf, (uint64_t)v);
}

void ak_batch_set_float(ak_batch *b, ak_id id, ak_prop p, double v) {
	prop(b, id, p, AK_T_FLOAT);
	ak_buf_f64(&b->buf, v);
}

void ak_batch_set_string(ak_batch *b, ak_id id, ak_prop p, const char *s, size_t n) {
	prop(b, id, p, AK_T_STRING);
	ak_buf_str(&b->buf, s, n);
}

void ak_batch_set_color(ak_batch *b, ak_id id, ak_prop p, ak_color c) {
	prop(b, id, p, AK_T_COLOR);
	ak_buf_u64(&b->buf, c);
}

void ak_batch_set_enum(ak_batch *b, ak_id id, ak_prop p, int32_t v) {
	prop(b, id, p, AK_T_ENUM);
	ak_buf_u32(&b->buf, (uint32_t)v);
}

void ak_batch_set_edges(ak_batch *b, ak_id id, ak_prop p, float top, float right, float bottom, float left) {
	prop(b, id, p, AK_T_EDGES);
	ak_buf_f32(&b->buf, top);
	ak_buf_f32(&b->buf, right);
	ak_buf_f32(&b->buf, bottom);
	ak_buf_f32(&b->buf, left);
}

void ak_batch_set_bytes(ak_batch *b, ak_id id, ak_prop p, const void *v, size_t n) {
	prop(b, id, p, AK_T_BYTES);
	ak_buf_str(&b->buf, v, n);
}

void ak_batch_command(ak_batch *b, ak_id id, ak_cmd c, const void *args, size_t n) {
	op(b, AK_OP_COMMAND, id);
	ak_buf_u16(&b->buf, c);
	ak_buf_str(&b->buf, args, n);
}

static ak_buf *open_length(ak_batch *b) {
	AK_ASSERT(b->mark == 0);
	b->mark = b->buf.len;
	ak_buf_u32(&b->buf, 0);
	return &b->buf;
}

static void close_length(ak_batch *b) {
	AK_ASSERT(b->mark != 0);
	uint32_t n = (uint32_t)(b->buf.len - b->mark - 4);
	uint8_t *p = b->buf.data + b->mark;
	p[0] = (uint8_t)n;
	p[1] = (uint8_t)(n >> 8);
	p[2] = (uint8_t)(n >> 16);
	p[3] = (uint8_t)(n >> 24);
	b->mark = 0;
}

ak_buf *ak_batch_set_bytes_begin(ak_batch *b, ak_id id, ak_prop p) {
	prop(b, id, p, AK_T_BYTES);
	return open_length(b);
}

void ak_batch_set_bytes_end(ak_batch *b) { close_length(b); }

ak_buf *ak_batch_command_begin(ak_batch *b, ak_id id, ak_cmd c) {
	op(b, AK_OP_COMMAND, id);
	ak_buf_u16(&b->buf, c);
	return open_length(b);
}

void ak_batch_command_end(ak_batch *b) { close_length(b); }

// Decoding.

void ak_batch_reader_init(ak_batch_reader *br, const uint8_t *data, size_t len) {
	br->r = (ak_reader){data, len, 0, false};
	br->left = ak_read_u32(&br->r);
	br->err = br->r.err ? AK_ERR_SHORT : AK_OK;
	if (br->err) br->left = 0;
}

static bool read_value(ak_reader *r, uint8_t type, ak_value *v) {
	memset(v, 0, sizeof *v);
	v->type = type;
	switch (type) {
	case AK_T_BOOL: v->i = ak_read_u8(r); break;
	case AK_T_INT:
	case AK_T_COLOR: v->i = (int64_t)ak_read_u64(r); break;
	case AK_T_FLOAT: v->f = ak_read_f64(r); break;
	case AK_T_STRING:
	case AK_T_BYTES: v->s = ak_read_str(r); break;
	case AK_T_EDGES:
		for (int i = 0; i < 4; i++) v->edges[i] = ak_read_f32(r);
		break;
	case AK_T_ENUM: v->i = (int32_t)ak_read_u32(r); break;
	default: return false;
	}
	return true;
}

bool ak_batch_next(ak_batch_reader *br, ak_op *op) {
	if (br->err) return false;
	ak_reader *r = &br->r;
	if (br->left == 0) {
		if (r->off != r->len) br->err = AK_ERR_TRAILING;
		return false;
	}
	memset(op, 0, sizeof *op);
	op->code = ak_read_u8(r);
	op->id = ak_read_u32(r);
	switch (op->code) {
	case AK_OP_CREATE: op->kind = ak_read_u16(r); break;
	case AK_OP_DESTROY:
	case AK_OP_SET_ROOT: break;
	case AK_OP_INSERT:
		op->child = ak_read_u32(r);
		op->index = ak_read_u32(r);
		break;
	case AK_OP_REMOVE: op->child = ak_read_u32(r); break;
	case AK_OP_SET_PROP: {
		op->prop = ak_read_u16(r);
		uint8_t t = ak_read_u8(r);
		if (!r->err && !read_value(r, t, &op->value)) {
			br->err = AK_ERR_BAD_TYPE;
			return false;
		}
		break;
	}
	case AK_OP_SET_FRAME:
		for (int i = 0; i < 4; i++) op->frame[i] = ak_read_f32(r);
		break;
	case AK_OP_LISTEN:
	case AK_OP_UNLISTEN: op->event = ak_read_u16(r); break;
	case AK_OP_COMMAND:
		op->cmd = ak_read_u16(r);
		op->value.type = AK_T_BYTES;
		op->value.s = ak_read_str(r);
		break;
	default:
		br->err = r->err ? AK_ERR_SHORT : AK_ERR_BAD_CODE;
		return false;
	}
	if (r->err) {
		br->err = AK_ERR_SHORT;
		return false;
	}
	br->left--;
	return true;
}

// Text forms.

// shortest finds the fewest significant digits (1..17) that read back as
// f, and writes f with them in %e form into tmp.
static int shortest(char *tmp, size_t n, double f, int bits) {
	int max = bits == 32 ? 9 : 17;
	for (int p = 1; p <= max; p++) {
		snprintf(tmp, n, "%.*e", p - 1, f);
		double back = strtod(tmp, NULL);
		if (bits == 32 ? (float)back == (float)f : back == f) return p;
	}
	snprintf(tmp, n, "%.*e", max - 1, f);
	return max;
}

void ak_fmt_float(ak_buf *out, double f, int bits) {
	if (f != f) {
		ak_buf_put(out, "NaN", 3);
		return;
	}
	if (isinf(f)) {
		ak_buf_printf(out, "%cInf", f > 0 ? '+' : '-');
		return;
	}
	// strconv's 'g' with the shortest precision: %e when the exponent is
	// under -4 or at least 6 (eprec is 6 for the shortest), else %f with
	// as many decimals as the digits need.
	char tmp[64];
	int digits = shortest(tmp, sizeof tmp, f, bits);
	const char *e = strchr(tmp, 'e');
	int exp = e ? atoi(e + 1) : 0;
	if (exp < -4 || exp >= 6) {
		ak_buf_put(out, tmp, strlen(tmp));
		return;
	}
	int prec = digits - (exp + 1);
	if (prec < 0) prec = 0;
	ak_buf_printf(out, "%.*f", prec, f);
}

// utf8 decodes one rune of s at i, returning its length (0: invalid).
static int utf8(const uint8_t *s, uint32_t n, uint32_t i, uint32_t *r) {
	uint8_t c = s[i];
	if (c < 0x80) {
		*r = c;
		return 1;
	}
	int len;
	uint32_t v, min;
	if ((c & 0xe0) == 0xc0) len = 2, v = c & 0x1f, min = 0x80;
	else if ((c & 0xf0) == 0xe0) len = 3, v = c & 0x0f, min = 0x800;
	else if ((c & 0xf8) == 0xf0) len = 4, v = c & 0x07, min = 0x10000;
	else return 0;
	if (i + (uint32_t)len > n) return 0;
	for (int k = 1; k < len; k++) {
		uint8_t d = s[i + (uint32_t)k];
		if ((d & 0xc0) != 0x80) return 0;
		v = v << 6 | (d & 0x3f);
	}
	if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff)) return 0;
	*r = v;
	return len;
}

// printable approximates Go's unicode.IsPrint for what UI text holds: the
// C1 controls, the soft hyphen, the line and paragraph separators,
// formatting characters, private use and noncharacters are not.
static bool printable(uint32_t r) {
	if (r < 0x20 || r == 0x7f) return false;
	if (r >= 0x80 && r < 0xa0) return false;
	if (r == 0xad) return false;
	if ((r >= 0x200b && r <= 0x200f) || (r >= 0x2028 && r <= 0x202e) || (r >= 0x2060 && r <= 0x206f)) return false;
	if (r == 0xfeff || (r >= 0xfff9 && r <= 0xfffb)) return false;
	if (r >= 0xe000 && r <= 0xf8ff) return false;
	if (r >= 0xf0000) return false;
	if ((r & 0xfffe) == 0xfffe) return false;
	return true;
}

void ak_fmt_quote(ak_buf *out, ak_span s) {
	const uint8_t *p = (const uint8_t *)s.p;
	ak_buf_u8(out, '"');
	for (uint32_t i = 0; i < s.len;) {
		uint32_t r;
		int n = utf8(p, s.len, i, &r);
		if (n == 0) {
			ak_buf_printf(out, "\\x%02x", p[i]);
			i++;
			continue;
		}
		i += (uint32_t)n;
		switch (r) {
		case '"': ak_buf_put(out, "\\\"", 2); continue;
		case '\\': ak_buf_put(out, "\\\\", 2); continue;
		case '\a': ak_buf_put(out, "\\a", 2); continue;
		case '\b': ak_buf_put(out, "\\b", 2); continue;
		case '\f': ak_buf_put(out, "\\f", 2); continue;
		case '\n': ak_buf_put(out, "\\n", 2); continue;
		case '\r': ak_buf_put(out, "\\r", 2); continue;
		case '\t': ak_buf_put(out, "\\t", 2); continue;
		case '\v': ak_buf_put(out, "\\v", 2); continue;
		}
		if (printable(r)) ak_buf_put(out, p + i - (uint32_t)n, (size_t)n);
		else if (r < 0x20 || r == 0x7f) ak_buf_printf(out, "\\x%02x", r);
		else if (r < 0x10000) ak_buf_printf(out, "\\u%04x", r);
		else ak_buf_printf(out, "\\U%08x", r);
	}
	ak_buf_u8(out, '"');
}

static void hex(ak_buf *out, ak_span s) {
	for (uint32_t i = 0; i < s.len; i++) ak_buf_printf(out, "%02x", (uint8_t)s.p[i]);
}

void ak_value_format(ak_buf *out, const ak_value *v) {
	switch (v->type) {
	case AK_T_BOOL: ak_buf_printf(out, "%s", v->i ? "true" : "false"); return;
	case AK_T_INT:
	case AK_T_ENUM: ak_buf_printf(out, "%lld", (long long)v->i); return;
	case AK_T_FLOAT: ak_fmt_float(out, v->f, 64); return;
	case AK_T_STRING: ak_fmt_quote(out, v->s); return;
	case AK_T_COLOR: ak_buf_printf(out, "#%llx", (unsigned long long)(uint64_t)v->i); return;
	case AK_T_EDGES:
		ak_buf_u8(out, '[');
		for (int i = 0; i < 4; i++) {
			if (i) ak_buf_u8(out, ' ');
			ak_fmt_float(out, v->edges[i], 32);
		}
		ak_buf_u8(out, ']');
		return;
	case AK_T_BYTES: hex(out, v->s); return;
	}
	ak_buf_u8(out, '?');
}

static void kind_name(ak_buf *out, ak_view_kind k) {
	const char *n = ak_kind_name(k);
	if (n) ak_buf_printf(out, "%s", n);
	else ak_buf_printf(out, "Kind(%u)", k);
}

static void event_name(ak_buf *out, ak_event_kind e) {
	const char *n = ak_event_name(e);
	if (n) ak_buf_printf(out, "%s", n);
	else ak_buf_printf(out, "Event(%u)", e);
}

static void format(ak_buf *out, const ak_op *op, bool by_kind, ak_view_kind k) {
	const char *code = ak_op_name(op->code);
	switch (op->code) {
	case AK_OP_CREATE:
		ak_buf_printf(out, "Create %u ", op->id);
		kind_name(out, op->kind);
		return;
	case AK_OP_DESTROY:
	case AK_OP_SET_ROOT: ak_buf_printf(out, "%s %u", code, op->id); return;
	case AK_OP_INSERT: ak_buf_printf(out, "Insert %u %u @%u", op->id, op->child, op->index); return;
	case AK_OP_REMOVE: ak_buf_printf(out, "Remove %u %u", op->id, op->child); return;
	case AK_OP_SET_PROP: {
		const ak_prop_info *info = ak_prop_of(by_kind ? k : 0, op->prop);
		if (info) ak_buf_printf(out, "SetProp %u %s=", op->id, info->name);
		else ak_buf_printf(out, "SetProp %u #%u=", op->id, op->prop);
		ak_value_format(out, &op->value);
		return;
	}
	case AK_OP_SET_FRAME:
		ak_buf_printf(out, "SetFrame %u ", op->id);
		ak_fmt_float(out, op->frame[0], 32);
		ak_buf_u8(out, ',');
		ak_fmt_float(out, op->frame[1], 32);
		ak_buf_u8(out, ' ');
		ak_fmt_float(out, op->frame[2], 32);
		ak_buf_u8(out, 'x');
		ak_fmt_float(out, op->frame[3], 32);
		return;
	case AK_OP_LISTEN:
	case AK_OP_UNLISTEN:
		ak_buf_printf(out, "%s %u ", code, op->id);
		event_name(out, op->event);
		return;
	case AK_OP_COMMAND: {
		ak_buf_printf(out, "Command %u ", op->id);
		const char *n = ak_cmd_name(op->cmd);
		if (n) ak_buf_printf(out, "%s ", n);
		else ak_buf_printf(out, "Command(%u) ", op->cmd);
		hex(out, op->value.s);
		return;
	}
	}
	ak_buf_printf(out, "Code(%u)", op->code);
}

void ak_op_format(ak_buf *out, const ak_op *op) { format(out, op, false, 0); }
void ak_op_format_kind(ak_buf *out, const ak_op *op, ak_view_kind k) { format(out, op, true, k); }

static void bits32(ak_buf *out, float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	ak_buf_printf(out, "%08x", u);
}

void ak_op_dump(ak_buf *out, const ak_op *op) {
	switch (op->code) {
	case AK_OP_CREATE: ak_buf_printf(out, "create %u %u", op->id, op->kind); return;
	case AK_OP_DESTROY: ak_buf_printf(out, "destroy %u", op->id); return;
	case AK_OP_INSERT: ak_buf_printf(out, "insert %u %u %u", op->id, op->child, op->index); return;
	case AK_OP_REMOVE: ak_buf_printf(out, "remove %u %u", op->id, op->child); return;
	case AK_OP_SET_PROP: {
		const ak_value *v = &op->value;
		ak_buf_printf(out, "set %u %u %u ", op->id, op->prop, v->type);
		switch (v->type) {
		case AK_T_BOOL: ak_buf_printf(out, "%s", v->i ? "true" : "false"); break;
		case AK_T_INT:
		case AK_T_ENUM: ak_buf_printf(out, "%lld", (long long)v->i); break;
		case AK_T_FLOAT: {
			uint64_t u;
			memcpy(&u, &v->f, 8);
			ak_buf_printf(out, "%016llx", (unsigned long long)u);
			break;
		}
		case AK_T_COLOR: ak_buf_printf(out, "%016llx", (unsigned long long)(uint64_t)v->i); break;
		case AK_T_EDGES:
			for (int i = 0; i < 4; i++) {
				if (i) ak_buf_u8(out, ' ');
				bits32(out, v->edges[i]);
			}
			break;
		case AK_T_STRING:
		case AK_T_BYTES: hex(out, v->s); break;
		}
		return;
	}
	case AK_OP_SET_FRAME:
		ak_buf_printf(out, "frame %u", op->id);
		for (int i = 0; i < 4; i++) {
			ak_buf_u8(out, ' ');
			bits32(out, op->frame[i]);
		}
		return;
	case AK_OP_LISTEN: ak_buf_printf(out, "listen %u %u", op->id, op->event); return;
	case AK_OP_UNLISTEN: ak_buf_printf(out, "unlisten %u %u", op->id, op->event); return;
	case AK_OP_COMMAND:
		ak_buf_printf(out, "command %u %u ", op->id, op->cmd);
		hex(out, op->value.s);
		return;
	case AK_OP_SET_ROOT: ak_buf_printf(out, "root %u", op->id); return;
	}
}

// Events.

const char *ak_platform_name(uint8_t p) {
	switch (p) {
	case AK_PLATFORM_IOS: return "iOS";
	case AK_PLATFORM_ANDROID: return "Android";
	case AK_PLATFORM_MACOS: return "macOS";
	case AK_PLATFORM_WINDOWS: return "Windows";
	case AK_PLATFORM_WEB: return "web";
	case AK_PLATFORM_LINUX: return "Linux";
	}
	return "unknown";
}

const char *ak_size_class_name(uint8_t c) {
	switch (c) {
	case AK_SIZE_CLASS_COMPACT: return "compact";
	case AK_SIZE_CLASS_MEDIUM: return "medium";
	case AK_SIZE_CLASS_EXPANDED: return "expanded";
	}
	return "unspecified";
}

uint8_t ak_width_class_of(float w) {
	if (w < 600) return AK_SIZE_CLASS_COMPACT;
	if (w < 840) return AK_SIZE_CLASS_MEDIUM;
	return AK_SIZE_CLASS_EXPANDED;
}

uint8_t ak_height_class_of(float h) {
	if (h < 480) return AK_SIZE_CLASS_COMPACT;
	if (h < 900) return AK_SIZE_CLASS_MEDIUM;
	return AK_SIZE_CLASS_EXPANDED;
}

void ak_event_append(ak_buf *b, const ak_event *e) {
	ak_buf_u32(b, e->id);
	ak_buf_u16(b, e->kind);
	switch (ak_event_payload(e->kind)) {
	case AK_P_BOOL: ak_buf_u8(b, e->b ? 1 : 0); break;
	case AK_P_INT: ak_buf_u64(b, (uint64_t)e->i); break;
	case AK_P_FLOAT: ak_buf_f64(b, e->f); break;
	case AK_P_STRING:
	case AK_P_BYTES: ak_buf_wstr(b, e->s); break;
	case AK_P_POINT:
		ak_buf_f64(b, e->x);
		ak_buf_f64(b, e->y);
		break;
	case AK_P_ENVIRONMENT: {
		const ak_environment *v = &e->env;
		const float fs[] = {v->width, v->height, v->safe_top, v->safe_right, v->safe_bottom, v->safe_left, v->keyboard, v->scale, v->font_scale};
		for (size_t i = 0; i < AK_LEN(fs); i++) ak_buf_f32(b, fs[i]);
		uint8_t d = (uint8_t)((v->platform & 7) << 2);
		if (v->dark) d |= 1;
		if (v->reduce_motion) d |= 2;
		if (v->screen_reader) d |= 32;
		if (v->tv) d |= 64;
		ak_buf_u8(b, d);
		ak_buf_u8(b, (uint8_t)((v->width_class & 3) | (v->height_class & 3) << 2));
		ak_buf_wstr(b, v->locale);
		ak_buf_wstr(b, v->time_zone);
		ak_buf_wstr(b, v->languages);
		ak_buf_u8(b, v->first_weekday);
		ak_buf_u8(b, v->hour_cycle);
		break;
	}
	case AK_P_GESTURE: {
		const ak_gesture *g = &e->g;
		ak_buf_u8(b, g->state);
		const float fs[] = {g->x, g->y, g->translation_x, g->translation_y, g->velocity_x, g->velocity_y, g->scale, g->rotation};
		for (size_t i = 0; i < AK_LEN(fs); i++) ak_buf_f32(b, fs[i]);
		break;
	}
	}
}

int ak_event_decode(const uint8_t *data, size_t len, ak_event *e) {
	ak_reader r = {data, len, 0, false};
	memset(e, 0, sizeof *e);
	e->id = ak_read_u32(&r);
	e->kind = ak_read_u16(&r);
	if (r.err) return AK_ERR_SHORT;
	switch (ak_event_payload(e->kind)) {
	case AK_P_NONE: break;
	case AK_P_BOOL: e->b = ak_read_u8(&r) != 0; break;
	case AK_P_INT: e->i = (int64_t)ak_read_u64(&r); break;
	case AK_P_FLOAT: e->f = ak_read_f64(&r); break;
	case AK_P_STRING:
	case AK_P_BYTES: e->s = ak_read_str(&r); break;
	case AK_P_POINT:
		e->x = ak_read_f64(&r);
		e->y = ak_read_f64(&r);
		break;
	case AK_P_ENVIRONMENT: {
		ak_environment *v = &e->env;
		float *fs[] = {&v->width, &v->height, &v->safe_top, &v->safe_right, &v->safe_bottom, &v->safe_left, &v->keyboard, &v->scale, &v->font_scale};
		for (size_t i = 0; i < AK_LEN(fs); i++) *fs[i] = ak_read_f32(&r);
		uint8_t f = ak_read_u8(&r);
		v->dark = f & 1;
		v->reduce_motion = f & 2;
		v->screen_reader = f & 32;
		v->tv = f & 64;
		v->platform = f >> 2 & 7;
		uint8_t c = ak_read_u8(&r);
		v->width_class = c & 3;
		v->height_class = c >> 2 & 3;
		v->locale = ak_read_str(&r);
		v->time_zone = ak_read_str(&r);
		v->languages = ak_read_str(&r);
		v->first_weekday = ak_read_u8(&r);
		v->hour_cycle = ak_read_u8(&r);
		break;
	}
	case AK_P_GESTURE: {
		ak_gesture *g = &e->g;
		g->state = ak_read_u8(&r);
		float *fs[] = {&g->x, &g->y, &g->translation_x, &g->translation_y, &g->velocity_x, &g->velocity_y, &g->scale, &g->rotation};
		for (size_t i = 0; i < AK_LEN(fs); i++) *fs[i] = ak_read_f32(&r);
		break;
	}
	default: return AK_ERR_BAD_EVENT;
	}
	if (r.err) return AK_ERR_SHORT;
	if (r.off != r.len) return AK_ERR_TRAILING;
	return AK_OK;
}
