// symbols.c: SF Symbol names drawn as the Segoe Fluent Icons glyphs for
// them (Segoe MDL2 Assets before Windows 11), a curated set (".fill"
// variants take the filled glyph where there's one). Sorted by name: found
// by bsearch.
#include "host.h"
#include "parts.h"

typedef struct {
	const char *name;
	uint16_t glyph;
} symbol;

static const symbol symbols[] = {
	{"archivebox", 0xE7B8},
	{"arrow.clockwise", 0xE72C},
	{"arrow.counterclockwise", 0xE7A7},
	{"arrow.down", 0xE74B},
	{"arrow.left", 0xE72B},
	{"arrow.right", 0xE72A},
	{"arrow.triangle.2.circlepath", 0xE895},
	{"arrow.up", 0xE74A},
	{"arrow.up.arrow.down", 0xE8CB},
	{"arrow.up.right.square", 0xE8A7},
	{"arrow.uturn.backward", 0xE7A7},
	{"arrow.uturn.forward", 0xE7A6},
	{"arrowshape.turn.up.left", 0xE8CA},
	{"arrowshape.turn.up.left.2", 0xE8C2},
	{"backward.fill", 0xEB9E},
	{"bag", 0xE7BF},
	{"battery.100", 0xE83F},
	{"bell", 0xEA8F},
	{"bell.fill", 0xEA8F},
	{"bold", 0xE8DD},
	{"bolt", 0xE945},
	{"bolt.fill", 0xE945},
	{"book", 0xE82D},
	{"bookmark", 0xE8A4},
	{"bookmark.fill", 0xE8A4},
	{"briefcase", 0xE821},
	{"bubble.left", 0xE8BD},
	{"bubble.left.and.bubble.right", 0xE8F2},
	{"calendar", 0xE787},
	{"camera", 0xE722},
	{"camera.fill", 0xE722},
	{"cart", 0xE7BF},
	{"cart.fill", 0xE7BF},
	{"chart.bar", 0xE9D2},
	{"chart.bar.fill", 0xE9D2},
	{"checkmark", 0xE73E},
	{"checkmark.circle", 0xE73E},
	{"checkmark.circle.fill", 0xEC61},
	{"chevron.down", 0xE70D},
	{"chevron.left", 0xE76B},
	{"chevron.left.forwardslash.chevron.right", 0xE943},
	{"chevron.right", 0xE76C},
	{"chevron.up", 0xE70E},
	{"circle", 0xEA3A},
	{"clock", 0xE823},
	{"clock.fill", 0xE823},
	{"cloud", 0xE753},
	{"creditcard", 0xE8C7},
	{"curlybraces", 0xE943},
	{"doc", 0xE8A5},
	{"doc.fill", 0xE8A5},
	{"doc.on.clipboard", 0xE77F},
	{"doc.on.doc", 0xE8C8},
	{"doc.text", 0xE8A5},
	{"ellipsis", 0xE712},
	{"ellipsis.bubble", 0xE8BD},
	{"ellipsis.circle", 0xE712},
	{"envelope", 0xE715},
	{"envelope.fill", 0xE715},
	{"exclamationmark.circle", 0xE783},
	{"exclamationmark.triangle", 0xE7BA},
	{"exclamationmark.triangle.fill", 0xE7BA},
	{"eye", 0xE7B3},
	{"eye.slash", 0xED1A},
	{"film", 0xE8B2},
	{"flag", 0xE7C1},
	{"flag.fill", 0xE7C1},
	{"folder", 0xE8B7},
	{"folder.fill", 0xE8D5},
	{"forward.fill", 0xEB9D},
	{"gear", 0xE713},
	{"gearshape", 0xE713},
	{"gearshape.fill", 0xE713},
	{"gift", 0xE88C},
	{"globe", 0xE774},
	{"hand.thumbsdown", 0xE8E0},
	{"hand.thumbsup", 0xE8E1},
	{"headphones", 0xE7F6},
	{"heart", 0xEB51},
	{"heart.fill", 0xEB52},
	{"house", 0xE80F},
	{"house.circle", 0xE80F},
	{"house.fill", 0xEA8A},
	{"icloud", 0xE753},
	{"info.circle", 0xE946},
	{"info.circle.fill", 0xE946},
	{"italic", 0xE8DB},
	{"key", 0xE8D7},
	{"keyboard", 0xE765},
	{"lightbulb", 0xEA80},
	{"line.3.horizontal", 0xE700},
	{"line.3.horizontal.decrease", 0xE71C},
	{"link", 0xE71B},
	{"list.bullet", 0xE8FD},
	{"location", 0xE81D},
	{"location.fill", 0xE81D},
	{"lock", 0xE72E},
	{"lock.fill", 0xE72E},
	{"lock.open", 0xE785},
	{"magnifyingglass", 0xE721},
	{"map", 0xE707},
	{"mappin", 0xE707},
	{"message", 0xE8BD},
	{"message.fill", 0xE8BD},
	{"mic", 0xE720},
	{"mic.fill", 0xE720},
	{"minus", 0xE738},
	{"minus.circle", 0xE949},
	{"moon", 0xE708},
	{"moon.fill", 0xE708},
	{"music.note", 0xE8D6},
	{"paintbrush", 0xE771},
	{"paintpalette", 0xE790},
	{"paperclip", 0xE723},
	{"paperplane", 0xE724},
	{"paperplane.fill", 0xE724},
	{"pause", 0xE769},
	{"pause.fill", 0xE769},
	{"pencil", 0xE70F},
	{"person", 0xE77B},
	{"person.2", 0xE716},
	{"person.2.fill", 0xE716},
	{"person.circle", 0xE77B},
	{"person.crop.circle", 0xE77B},
	{"person.crop.circle.fill", 0xEA8C},
	{"person.fill", 0xEA8C},
	{"person.fill.checkmark", 0xE8FB},
	{"person.fill.xmark", 0xE8F8},
	{"phone", 0xE717},
	{"phone.fill", 0xE717},
	{"photo", 0xE91B},
	{"photo.fill", 0xE91B},
	{"photo.on.rectangle", 0xE91B},
	{"play", 0xE768},
	{"play.fill", 0xE768},
	{"plus", 0xE710},
	{"plus.circle", 0xE948},
	{"plus.circle.fill", 0xE948},
	{"printer", 0xE749},
	{"qrcode", 0xED14},
	{"questionmark.bubble", 0xE897},
	{"questionmark.circle", 0xE897},
	{"rectangle.grid.2x2", 0xF0E2},
	{"safari", 0xE774},
	{"scissors", 0xE8C6},
	{"shield", 0xEA18},
	{"slider.horizontal.3", 0xE9E9},
	{"speaker.slash", 0xE74F},
	{"speaker.wave.2", 0xE767},
	{"speaker.wave.2.fill", 0xE767},
	{"square.and.arrow.down", 0xE896},
	{"square.and.arrow.up", 0xE72D},
	{"square.and.arrow.up.fill", 0xE72D},
	{"square.and.pencil", 0xE70F},
	{"square.grid.2x2", 0xF0E2},
	{"square.stack", 0xE8F1},
	{"star", 0xE734},
	{"star.fill", 0xE735},
	{"stop.fill", 0xE71A},
	{"sun.max", 0xE706},
	{"tag", 0xE8EC},
	{"textformat", 0xE8D2},
	{"timer", 0xE916},
	{"trash", 0xE74D},
	{"trash.fill", 0xE74D},
	{"tray", 0xE7B8},
	{"underline", 0xE8DC},
	{"video", 0xE714},
	{"video.fill", 0xE714},
	{"wifi", 0xE701},
	{"wifi.exclamationmark", 0xF384},
	{"xmark", 0xE711},
	{"xmark.circle", 0xF13D},
	{"xmark.circle.fill", 0xEB90},
};

static int cmp(const void *k, const void *e) { return strcmp(k, ((const symbol *)e)->name); }

static uint16_t lookup(const char *name) {
	const symbol *s = bsearch(name, symbols, WH_LEN(symbols), sizeof *symbols, cmp);
	return s ? s->glyph : 0;
}

static uint16_t plain_glyph(const char *name) {
	if (!name || !*name) return 0;
	uint16_t g = lookup(name);
	if (g) return g;
	size_t n = strlen(name);
	if (n > 5 && strcmp(name + n - 5, ".fill") == 0 && n < 128) {
		char base[128];
		memcpy(base, name, n - 5);
		base[n - 5] = 0;
		return lookup(base);
	}
	return 0;
}

uint16_t wh_symbol_glyph(const char *name) { return wh_directed_glyph(name, false); }

// replace_parts writes name with each part from swapped for to.
static void swap_parts(char *out, size_t cap, const char *name, const char *a, const char *b, const char *ra, const char *rb) {
	size_t o = 0;
	const char *p = name;
	while (*p && o + 1 < cap) {
		const char *dot = strchr(p, '.');
		size_t len = dot ? (size_t)(dot - p) : strlen(p);
		const char *part = p;
		size_t pl = len;
		if (len == strlen(a) && strncmp(p, a, len) == 0) part = ra, pl = strlen(ra);
		else if (len == strlen(b) && strncmp(p, b, len) == 0) part = rb, pl = strlen(rb);
		if (o + pl + 2 >= cap) break;
		memcpy(out + o, part, pl);
		o += pl;
		if (dot) out[o++] = '.';
		p = dot ? dot + 1 : p + len;
	}
	out[o] = 0;
}

static bool has_part(const char *name, const char *part) {
	size_t n = strlen(part);
	for (const char *p = name; p; p = strchr(p, '.')) {
		if (*p == '.') p++;
		if (strncmp(p, part, n) == 0 && (p[n] == '.' || p[n] == 0)) return true;
	}
	return false;
}

// wh_directed_glyph is the glyph of a symbol in a direction. A name that
// says forward or backward takes its glyph if there's one (or the other
// way round right to left), else that of the side it means
// (chevron.forward: chevron.right, left to right).
uint16_t wh_directed_glyph(const char *name, bool rtl) {
	if (!name || (!has_part(name, "forward") && !has_part(name, "backward"))) return plain_glyph(name);
	char buf[160];
	if (rtl) swap_parts(buf, sizeof buf, name, "forward", "backward", "backward", "forward");
	else snprintf(buf, sizeof buf, "%s", name);
	uint16_t g = plain_glyph(buf);
	if (g) return g;
	char side[160];
	swap_parts(side, sizeof side, buf, "forward", "backward", "right", "left");
	return plain_glyph(side);
}

static wh_map warned;

// wh_warn_symbol tells, once, of a symbol the host has no glyph for: it
// shows nothing.
void wh_warn_symbol(const char *name) {
	uint64_t h = 1469598103934665603ULL;
	for (const char *p = name; *p; p++) h = (h ^ (uint8_t)*p) * 1099511628211ULL;
	if (wh_map_get(&warned, h | 1)) return;
	wh_map_put(&warned, h | 1, (void *)1);
	wh_log("the Windows host has no glyph for the symbol \"%s\"", name);
}
