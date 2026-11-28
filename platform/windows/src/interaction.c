// interaction.c: interaction states and drag and drop (DESIGN.md,
// "Interaction states, drag and drop"):
//
//   - Pressed: a view's press with the mouse or Space (views.c), reported
//     as it changes (wh_set_pressed).
//   - Hover: tracked from the message loop. Each mouse move finds the
//     views under the pointer by their rectangles, in the app's window
//     under it (views that let the mouse through count), and
//     TrackMouseEvent's WM_MOUSELEAVE, or a timer where it can't watch,
//     clears them as the pointer leaves the app's windows. HoverMove: each
//     move, where the pointer is in each view under it that listens.
//   - Drop targets: each top-level window of the app is an OLE drop target
//     (an IDropTarget of the host's), which finds the innermost view under
//     the drag that takes it.
//   - Drag sources: a mouse drag of a view with DragItems runs an OLE drag
//     (DoDragDrop, with an IDropSource of the host's) of the shell's data
//     object, with the view's image (the shell's drag image helper).
//   - Paste: a field's PasteTypes take the clipboard's files and images
//     (copied into the app's caches) instead of its text.
#include "nav.h"

#include <shlobj.h>

static const GUID iid_unknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID iid_data_object = {0x0000010E, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID iid_drop_source = {0x00000121, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID iid_drop_target = {0x00000122, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
static const GUID clsid_drag_drop_helper = {0x4657278A, 0x411B, 0x11D2, {0x83, 0x9A, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0xD0}};
static const GUID iid_drop_target_helper = {0x4657278B, 0x411B, 0x11D2, {0x83, 0x9A, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0xD0}};
static const GUID iid_drag_source_helper = {0xDE5BF786, 0x477A, 0x11D2, {0x83, 0x9D, 0x00, 0xC0, 0x4F, 0xD9, 0x18, 0xD0}};

// ---- Hover ----

static int hover_views;           // views listening for Hover or HoverMove
static node **hovered;            // those the pointer is over, innermost first
static uint32_t nhovered;
static HWND hover_win;            // where the pointer was last
static POINT hover_pt;
static bool hovering, hover_again, hover_queued;
static int hover_refreshes;
static HWND tracked;              // the window watched for the pointer leaving
static node **chain;              // views_at's
static uint32_t nchain, capchain;

void wh_hover_views(int delta) { hover_views += delta; }

// in_region reports whether pt (on the screen) is in a window's region,
// if it has one (an overlay's: its children's rectangles).
static bool in_region(HWND w, RECT r, POINT pt) {
	HRGN rg = CreateRectRgn(0, 0, 0, 0);
	bool in = GetWindowRgn(w, rg) == ERROR || PtInRegion(rg, pt.x - r.left, pt.y - r.top);
	DeleteObject(rg);
	return in;
}

// window_at is the deepest visible window at pt (on the screen) in top,
// found by the windows' rectangles (and regions), not by hit-testing:
// views that let the mouse through count.
static HWND window_at(HWND top, POINT pt) {
	HWND w = top;
	for (;;) {
		HWND next = NULL;
		for (HWND c = GetWindow(w, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
			if (!(GetWindowLongPtrW(c, GWL_STYLE) & WS_VISIBLE)) continue;
			RECT r;
			GetWindowRect(c, &r);
			if (pt.x >= r.left && pt.x < r.right && pt.y >= r.top && pt.y < r.bottom && in_region(c, r, pt)) {
				next = c;
				break;
			}
		}
		if (!next) return w;
		w = next;
	}
}

// views_at are the views at pt in top, innermost first: the window there
// and its ancestors (in chain, reused by the next call).
static void views_at(HWND top, POINT pt) {
	nchain = 0;
	for (HWND w = window_at(top, pt); w; w = GetParent(w)) {
		node *n = wh_node_of(w);
		if (n && n->hwnd == w && wh_node(n->id) == n) {
			if (nchain == capchain) {
				capchain = capchain ? capchain * 2 : 16;
				chain = wh_realloc(chain, capchain * sizeof *chain);
			}
			chain[nchain++] = n;
		}
		if (w == top) break;
	}
}

static bool among(node **list, uint32_t n, const node *x) {
	for (uint32_t i = 0; i < n; i++)
		if (list[i] == x) return true;
	return false;
}

static void hover_once(void) {
	if (!hover_views && !nhovered) return;
	HWND top = hover_win;
	POINT pt = hover_pt;
	node **now = NULL;
	uint32_t nnow = 0;
	if (top && IsWindow(top) && wh_key_window(top) == top) {
		views_at(top, pt);
		now = wh_alloc((nchain ? nchain : 1) * sizeof *now);
		for (uint32_t i = 0; i < nchain; i++)
			if (node_listening(chain[i], AK_EVENT_HOVER)) now[nnow++] = chain[i];
	}
	if (nnow == nhovered && (!nnow || !memcmp(now, hovered, nnow * sizeof *now))) {
		free(now);
		return;
	}
	node **was = hovered;
	uint32_t nwas = nhovered;
	hovered = now, nhovered = nnow;
	// Reported from a copy: a report runs the core, whose flush may
	// destroy views, taken out of hovered as they go (their nodes stay
	// until the message returns, dead: wh_emit skips them).
	node **seen = wh_alloc((nnow ? nnow : 1) * sizeof *seen);
	memcpy(seen, now, nnow * sizeof *seen);
	for (uint32_t i = 0; i < nwas; i++)
		if (!among(seen, nnow, was[i])) wh_emit_bool(was[i], AK_EVENT_HOVER, false);
	for (uint32_t i = nnow; i-- > 0;)
		if (!among(was, nwas, seen[i])) wh_emit_bool(seen[i], AK_EVENT_HOVER, true);
	free(seen);
	free(was);
}

// wh_hover_at finds the views listening for Hover under the pointer, at
// pt in top (NULL: not over the app's windows), and reports the changes:
// those it left (innermost first), then those it came over (outermost
// first). Called again while it reports (a report's flush moved views
// under the pointer), it goes again once they're reported.
void wh_hover_at(HWND top, POINT pt) {
	hover_win = top, hover_pt = pt;
	if (hovering) {
		hover_again = true;
		return;
	}
	hovering = true;
	// A style that moves a view off the pointer as it's hovered, and back
	// as it isn't, would go on.
	for (int i = 0; i < 8; i++) {
		hover_again = false;
		hover_once();
		if (!hover_again) break;
	}
	hovering = false;
}

// wh_hover_moves reports where the pointer is (pt, on the screen, in top) to
// the views under it that listen for HoverMove, innermost first; the
// same place again (the timer's look) isn't a move.
void wh_hover_moves(HWND top, POINT pt) {
	static POINT last;
	static bool had;
	if (had && pt.x == last.x && pt.y == last.y) return;
	last = pt, had = true;
	if (!top || !IsWindow(top) || wh_key_window(top) != top) return;
	views_at(top, pt);
	// By ID: a report's flush may destroy views, and views_at's chain is
	// the next call's.
	uint32_t ids[32], n = 0;
	for (uint32_t i = 0; i < nchain && n < WH_LEN(ids); i++)
		if (node_listening(chain[i], AK_EVENT_HOVER_MOVE)) ids[n++] = chain[i]->id;
	for (uint32_t i = 0; i < n; i++) {
		node *v = wh_node(ids[i]);
		if (!v || !v->hwnd) continue;
		POINT c = pt;
		ScreenToClient(v->hwnd, &c);
		ak_event e = {.kind = AK_EVENT_HOVER_MOVE, .x = wh_pt(v, c.x), .y = wh_pt(v, c.y)};
		wh_emit(v, &e);
	}
}

// wh_pointer_moved follows the pointer at pt (on the screen; hwnd the
// window of the app's the move was for) for Hover. With track, the window
// under it is watched for the pointer leaving (TrackMouseEvent); where it
// can't be (the mouse captured, another thread's window: a web view's), a
// timer checks now and then.
void wh_pointer_moved(HWND hwnd, POINT pt, bool track) {
	hover_refreshes = 0;
	if (!hover_views && !nhovered) {
		hover_win = wh_key_window(hwnd), hover_pt = pt;
		return;
	}
	HWND under = WindowFromPoint(pt);
	wh_hover_at(wh_key_window(under), pt);
	wh_hover_moves(wh_key_window(under), pt);
	if (!nhovered) {
		KillTimer(H.msg_win, TIMER_HOVER);
		return;
	}
	if (track && !GetCapture() && under && GetWindowThreadProcessId(under, NULL) == GetCurrentThreadId()) {
		if (under != tracked) {
			TRACKMOUSEEVENT t = {sizeof t, TME_LEAVE, under, 0};
			TrackMouseEvent(&t);
			tracked = under;
		}
		KillTimer(H.msg_win, TIMER_HOVER);
		return;
	}
	SetTimer(H.msg_win, TIMER_HOVER, 100, NULL);
}

// wh_mouse_left is a WM_MOUSELEAVE: the pointer left a window of the
// app's (for another of them, or another app's).
void wh_mouse_left(HWND hwnd) {
	if (tracked && hwnd != tracked) return;  // a window left before the one watched now
	tracked = NULL;
	POINT p;
	GetCursorPos(&p);
	wh_pointer_moved(hwnd, p, false);
}

static void refresh_now(void *ctx) {
	(void)ctx;
	hover_queued = false;
	hover_refreshes++;
	wh_hover_at(hover_win, hover_pt);
}

// wh_refresh_hover finds the views under the pointer again once the core
// has returned: views shown, hidden, moved or made under it. A few times
// between moves of the pointer: a style that moves a view off the pointer
// as it's hovered, and back as it isn't, would go on.
void wh_refresh_hover(void) {
	if (hover_queued || (!hover_views && !nhovered) || hover_refreshes >= 8) return;
	hover_queued = true;
	wh_after_core(refresh_now, NULL);
}

// ---- What a drag carries ----

static UINT cf_source, cf_url_w, cf_url, cf_group, cf_contents, cf_png;

static void formats(void) {
	if (cf_source) return;
	cf_source = RegisterClipboardFormatW(L"Asuka Drag Source");
	cf_url_w = RegisterClipboardFormatW(L"UniformResourceLocatorW");
	cf_url = RegisterClipboardFormatW(L"UniformResourceLocator");
	cf_group = RegisterClipboardFormatW(L"FileGroupDescriptorW");
	cf_contents = RegisterClipboardFormatW(L"FileContents");
	cf_png = RegisterClipboardFormatW(L"PNG");
}

static FORMATETC format_of(UINT cf, LONG index, DWORD tymed) { return (FORMATETC){(CLIPFORMAT)cf, NULL, DVASPECT_CONTENT, index, tymed}; }

// has_data reports whether a data object has format cf in global memory.
static bool has_data(IDataObject *d, UINT cf) {
	FORMATETC fe = format_of(cf, -1, TYMED_HGLOBAL);
	return IDataObject_QueryGetData(d, &fe) == S_OK;
}

// get_medium gets format cf (its item index, -1 for all) from a data
// object, in one of the media tymed; release it (ReleaseStgMedium).
static bool get_medium(IDataObject *d, UINT cf, LONG index, DWORD tymed, STGMEDIUM *sm) {
	FORMATETC fe = format_of(cf, index, tymed);
	memset(sm, 0, sizeof *sm);
	if (FAILED(IDataObject_GetData(d, &fe, sm))) return false;
	if (!(sm->tymed & tymed)) {
		ReleaseStgMedium(sm);
		return false;
	}
	return true;
}

// global_bytes copies global memory, at most limit bytes (-1: all).
static uint8_t *global_bytes(HGLOBAL g, int64_t limit, size_t *len) {
	size_t size = GlobalSize(g);
	if (limit >= 0 && (uint64_t)limit < size) size = (size_t)limit;
	*len = 0;
	void *p = GlobalLock(g);
	if (!p) return NULL;
	uint8_t *b = wh_alloc(size + 2);
	memcpy(b, p, size);
	GlobalUnlock(g);
	*len = size;
	return b;
}

// get_data copies format cf out of a data object (free it).
static uint8_t *get_data(IDataObject *d, UINT cf, size_t *len) {
	STGMEDIUM sm;
	*len = 0;
	if (!get_medium(d, cf, -1, TYMED_HGLOBAL, &sm)) return NULL;
	uint8_t *b = global_bytes(sm.hGlobal, -1, len);
	ReleaseStgMedium(&sm);
	return b;
}

// get_text is a data object's text in format cf, UTF-16 (CF_UNICODETEXT,
// UniformResourceLocatorW) or, ansi, 8-bit (UniformResourceLocator), as
// UTF-8 (free it); NULL: none.
static char *get_text(IDataObject *d, UINT cf, bool ansi) {
	size_t len;
	uint8_t *b = get_data(d, cf, &len);
	if (!b) return NULL;
	char *s;
	if (ansi) {
		s = wh_strndup((const char *)b, strnlen((const char *)b, len));
	} else {
		const wchar_t *w = (const wchar_t *)b;
		size_t n = 0;
		while (n < len / 2 && w[n]) n++;
		int l;
		s = wh_utf8(w, (int)n, &l);
	}
	free(b);
	return s;
}

// dropped_files are the paths in a data object's CF_HDROP (UTF-8); free
// them with wh_free_cstrings.
static uint32_t dropped_files(IDataObject *d, char ***out) {
	*out = NULL;
	STGMEDIUM sm;
	if (!get_medium(d, CF_HDROP, -1, TYMED_HGLOBAL, &sm)) return 0;
	HDROP h = (HDROP)sm.hGlobal;
	UINT n = DragQueryFileW(h, 0xFFFFFFFF, NULL, 0);
	char **paths = wh_alloc((n ? n : 1) * sizeof *paths);
	uint32_t k = 0;
	for (UINT i = 0; i < n; i++) {
		UINT l = DragQueryFileW(h, i, NULL, 0);
		if (!l) continue;
		wchar_t *buf = wh_alloc(((size_t)l + 1) * sizeof *buf);
		DragQueryFileW(h, i, buf, l + 1);
		int ul;
		paths[k++] = wh_utf8(buf, (int)l, &ul);
		free(buf);
	}
	ReleaseStgMedium(&sm);
	*out = paths;
	return k;
}

// A virtual file: one a drag carries as its contents, not as a path
// (FileGroupDescriptorW and FileContents: Outlook's attachments, a
// browser's images).
typedef struct {
	char *name;    // its path in the drag, \-separated (UTF-8)
	int64_t size;  // -1: unknown
	bool dir;
} vfile;

static uint32_t virtual_files(IDataObject *d, vfile **out) {
	*out = NULL;
	size_t len;
	uint8_t *b = get_data(d, cf_group, &len);
	if (!b) return 0;
	if (len < 4) {
		free(b);
		return 0;
	}
	uint32_t n;
	memcpy(&n, b, 4);
	// Another app's count: no more than its descriptors there are.
	const size_t size = sizeof(FILEDESCRIPTORW);
	if (n > (len - 4) / size) n = (uint32_t)((len - 4) / size);
	vfile *v = wh_alloc((n ? n : 1) * sizeof *v);
	uint32_t k = 0;
	for (uint32_t i = 0; i < n; i++) {
		FILEDESCRIPTORW fd;
		memcpy(&fd, b + 4 + i * size, size);
		fd.cFileName[MAX_PATH - 1] = 0;
		int l;
		v[k].name = wh_utf8(fd.cFileName, (int)wcslen(fd.cFileName), &l);
		v[k].size = (fd.dwFlags & FD_FILESIZE) ? (int64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow : -1;
		v[k].dir = (fd.dwFlags & FD_ATTRIBUTES) && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
		k++;
	}
	free(b);
	*out = v;
	return k;
}

static void free_vfiles(vfile *v, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) free(v[i].name);
	free(v);
}

// save_virtual_file writes a data object's virtual file i (whose size is
// size, -1 if unknown) to path, and returns its size; -1: it failed.
static int64_t save_virtual_file(IDataObject *d, uint32_t i, int64_t size, const char *path) {
	STGMEDIUM sm;
	if (!get_medium(d, cf_contents, (LONG)i, TYMED_HGLOBAL | TYMED_ISTREAM, &sm)) return -1;
	wchar_t *wp = wh_wide(path, (int)strlen(path));
	HANDLE f = CreateFileW(wp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	free(wp);
	if (f == INVALID_HANDLE_VALUE) {
		ReleaseStgMedium(&sm);
		return -1;
	}
	int64_t n = 0;
	bool ok = true;
	if (sm.tymed == TYMED_HGLOBAL) {
		size_t len;
		uint8_t *b = global_bytes(sm.hGlobal, size, &len);
		DWORD w = 0;
		ok = b && WriteFile(f, b, (DWORD)len, &w, NULL) && w == len;
		n = (int64_t)w;
		free(b);
	} else {
		// IStream::Read until it ends.
		static uint8_t buf[64 << 10];
		for (;;) {
			ULONG got = 0;
			HRESULT hr = IStream_Read(sm.pstm, buf, sizeof buf, &got);
			if (got > 0) {
				DWORD w = 0;
				if (!WriteFile(f, buf, got, &w, NULL) || w != got) {
					ok = false;
					break;
				}
				n += got;
			}
			if (FAILED(hr)) {
				ok = false;
				break;
			}
			if (got == 0 || hr != S_OK) break;  // S_FALSE: the end
		}
	}
	ok = CloseHandle(f) && ok;
	ReleaseStgMedium(&sm);
	return ok ? n : -1;
}

// ---- MIME types ----

// file_mime is a file's MIME type from its name, as Go's
// mime.TypeByExtension has it on Windows (the registry's Content Type,
// then the common types), without parameters (free it).
static char *file_mime(const char *path) {
	const char *slash = strrchr(path, '\\'), *fwd = strrchr(path, '/');
	if (fwd > slash) slash = fwd;
	const char *dot = strrchr(path, '.');
	if (!dot || (slash && dot < slash)) return wh_strdup("application/octet-stream");
	char ext[32];
	size_t el = strlen(dot);
	if (el >= sizeof ext) return wh_strdup("application/octet-stream");
	for (size_t i = 0; i <= el; i++) ext[i] = (char)tolower((unsigned char)dot[i]);
	wchar_t *wext = wh_wide(ext, (int)el);
	wchar_t buf[128];
	DWORD size = sizeof buf;
	char *found = NULL;
	if (RegGetValueW(HKEY_CLASSES_ROOT, wext, L"Content Type", RRF_RT_REG_SZ, NULL, buf, &size) == ERROR_SUCCESS) {
		int l;
		found = wh_utf8(buf, (int)wcslen(buf), &l);
		// Some registries have .js as text/plain: not what it is.
		if (!strcmp(ext, ".js") && !strncmp(found, "text/plain", 10)) {
			free(found);
			found = NULL;
		}
	}
	free(wext);
	if (!found) {
		static const char *const types[][2] = {
			{".avif", "image/avif"}, {".css", "text/css"}, {".gif", "image/gif"}, {".htm", "text/html"},
			{".html", "text/html"}, {".jpeg", "image/jpeg"}, {".jpg", "image/jpeg"}, {".js", "text/javascript"},
			{".json", "application/json"}, {".mjs", "text/javascript"}, {".pdf", "application/pdf"}, {".png", "image/png"},
			{".svg", "image/svg+xml"}, {".wasm", "application/wasm"}, {".webp", "image/webp"}, {".xml", "text/xml"},
		};
		for (size_t i = 0; i < WH_LEN(types); i++)
			if (!strcmp(ext, types[i][0])) found = wh_strdup(types[i][1]);
	}
	if (!found) return wh_strdup("application/octet-stream");
	char *semi = strchr(found, ';');
	if (semi) *semi = 0;
	size_t l = strlen(found);
	while (l && found[l - 1] == ' ') found[--l] = 0;
	char *s = found;
	while (*s == ' ') s++;
	if (s != found) memmove(found, s, strlen(s) + 1);
	return found;
}

// file_kind is the ShareKind of a file of a MIME type.
static uint8_t file_kind(const char *m) {
	if (!strncmp(m, "image/", 6)) return AK_SHARE_KIND_IMAGE;
	if (!strncmp(m, "video/", 6)) return AK_SHARE_KIND_VIDEO;
	if (!strncmp(m, "audio/", 6)) return AK_SHARE_KIND_AUDIO;
	return AK_SHARE_KIND_FILE;
}

// patterns are a view's MIME patterns (DropAccept, PasteTypes) as spans.
typedef struct {
	ak_span *p;
	uint32_t n;
} patterns;

static patterns patterns_of(char **s, uint32_t n) {
	patterns p = {wh_alloc((n ? n : 1) * sizeof *p.p), n};
	for (uint32_t i = 0; i < n; i++) p.p[i] = ak_cstr(s[i]);
	return p;
}

static bool matches(patterns a, const char *mime) { return ak_match_mime(a.p, a.n, ak_cstr(mime)); }

// Items, with the strings they point to.
typedef struct {
	ak_item *v;
	uint32_t n, cap;
	char **strs;  // what the items' spans point into
	uint32_t nstrs, capstrs;
} items;

static ak_span keep(items *it, char *s) {
	if (it->nstrs == it->capstrs) {
		it->capstrs = it->capstrs ? it->capstrs * 2 : 16;
		it->strs = wh_realloc(it->strs, it->capstrs * sizeof *it->strs);
	}
	it->strs[it->nstrs++] = s;
	return ak_cstr(s);
}

static void add_item(items *it, ak_item x) {
	if (it->n == it->cap) {
		it->cap = it->cap ? it->cap * 2 : 8;
		it->v = wh_realloc(it->v, it->cap * sizeof *it->v);
	}
	it->v[it->n++] = x;
}

static void free_items(items *it) {
	for (uint32_t i = 0; i < it->nstrs; i++) free(it->strs[i]);
	free(it->strs);
	free(it->v);
	memset(it, 0, sizeof *it);
}

static bool has_files(const items *it) {
	for (uint32_t i = 0; i < it->n; i++)
		if (it->v[i].path.len) return true;
	return false;
}

static const char *base_name(const char *path) {
	const char *b = path;
	for (const char *p = path; *p; p++)
		if (*p == '\\' || *p == '/') b = p + 1;
	return b;
}

static char *join(const char *dir, const char *name) {
	size_t a = strlen(dir), b = strlen(name);
	char *s = wh_alloc(a + b + 2);
	memcpy(s, dir, a);
	s[a] = '\\';
	memcpy(s + a + 1, name, b);
	return s;
}

static bool exists(const char *path) {
	wchar_t *w = wh_wide(path, (int)strlen(path));
	bool e = GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
	free(w);
	return e;
}

static int64_t file_size(const char *path) {
	wchar_t *w = wh_wide(path, (int)strlen(path));
	WIN32_FILE_ATTRIBUTE_DATA a;
	int64_t size = -1;
	if (GetFileAttributesExW(w, GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
		size = (int64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow;
	free(w);
	return size;
}

// drop_dir is a new folder in the app's caches for a drop's or a
// paste's copies (free it), or NULL.
static char *drop_dir(void) {
	wchar_t base[MAX_PATH];
	DWORD l = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
	if (!l || l >= MAX_PATH) return NULL;
	int ul;
	char *b = wh_utf8(base, (int)l, &ul);
	char *app = join(b, H.app_name ? H.app_name : "Asuka");
	char *drops = join(app, "Drops");
	free(b);
	free(app);
	if (!dsk_mkdirs(drops)) {
		free(drops);
		return NULL;
	}
	for (int i = 0; i < 100; i++) {
		char name[32];
		snprintf(name, sizeof name, "drop%u", (unsigned)(GetTickCount() * 2654435761u + (unsigned)i * 7919u) % 1000000000u);
		char *dir = join(drops, name);
		wchar_t *w = wh_wide(dir, (int)strlen(dir));
		BOOL made = CreateDirectoryW(w, NULL);
		free(w);
		if (made) {
			free(drops);
			return dir;
		}
		free(dir);
		if (GetLastError() != ERROR_ALREADY_EXISTS) break;
	}
	free(drops);
	return NULL;
}

// dropped_items loads the items of another app's drop (or the
// clipboard's) that a target accepting accept takes: its files (the files
// themselves; virtual files copied into the app's caches, *copied then
// true), its text and its link.
static void dropped_items(IDataObject *d, patterns accept, items *out, bool *copied) {
	formats();
	*copied = false;
	char **files;
	uint32_t nfiles = dropped_files(d, &files);
	for (uint32_t i = 0; i < nfiles; i++) {
		char *m = file_mime(files[i]);
		if (!matches(accept, m)) {
			free(m);
			continue;
		}
		ak_item it = {.kind = file_kind(m), .size = file_size(files[i])};
		it.path = keep(out, wh_strdup(files[i]));
		it.name = keep(out, wh_strdup(base_name(files[i])));
		it.mime = keep(out, m);
		add_item(out, it);
	}
	if (!nfiles) {
		vfile *v;
		uint32_t nv = virtual_files(d, &v);
		char *dir = NULL;
		for (uint32_t i = 0; i < nv; i++) {
			char *m = file_mime(v[i].name);
			if (v[i].dir || !matches(accept, m)) {
				free(m);
				continue;
			}
			if (!dir && !(dir = drop_dir())) {
				free(m);
				break;
			}
			for (char *p = v[i].name; *p; p++)
				if (*p == '/') *p = '\\';
			const char *name = base_name(v[i].name);
			char *path = join(dir, name);
			if (exists(path)) {
				free(path);
				char numbered[MAX_PATH * 3];
				snprintf(numbered, sizeof numbered, "%u %s", i, name);
				path = join(dir, numbered);
			}
			int64_t size = save_virtual_file(d, i, v[i].size, path);
			if (size < 0) {
				wchar_t *w = wh_wide(path, (int)strlen(path));
				DeleteFileW(w);
				free(w);
				free(path);
				free(m);
				continue;
			}
			ak_item it = {.kind = file_kind(m), .size = size};
			it.path = keep(out, path);
			it.name = keep(out, wh_strdup(name));
			it.mime = keep(out, m);
			add_item(out, it);
			*copied = true;
		}
		free(dir);
		free_vfiles(v, nv);
	}
	wh_free_cstrings(files, nfiles);
	char *link = NULL;
	if (matches(accept, "text/uri-list")) {
		char *u = get_text(d, cf_url_w, false);
		if (!u) u = get_text(d, cf_url, true);
		if (u) {
			char *s = u;
			while (*s && isspace((unsigned char)*s)) s++;
			size_t l = strlen(s);
			while (l && isspace((unsigned char)s[l - 1])) l--;
			if (l) {
				link = wh_strndup(s, l);
				ak_item it = {.kind = AK_SHARE_KIND_LINK, .size = -1};
				it.text = keep(out, wh_strdup(link));
				it.mime = ak_cstr("text/uri-list");
				add_item(out, it);
			}
			free(u);
		}
	}
	if (matches(accept, "text/plain")) {
		// A browser's link comes as its text too: once.
		char *t = get_text(d, CF_UNICODETEXT, false);
		if (t && *t) {
			char *s = t;
			while (*s && isspace((unsigned char)*s)) s++;
			size_t l = strlen(s);
			while (l && isspace((unsigned char)s[l - 1])) l--;
			if (!link || strlen(link) != l || strncmp(link, s, l)) {
				// \r\n to \n.
				size_t k = 0;
				for (size_t i = 0; t[i]; i++)
					if (!(t[i] == '\r' && t[i + 1] == '\n')) t[k++] = t[i];
				t[k] = 0;
				ak_item it = {.kind = AK_SHARE_KIND_TEXT, .size = -1};
				it.text = keep(out, t);
				it.mime = ak_cstr("text/plain");
				add_item(out, it);
				t = NULL;
			}
		}
		free(t);
	}
	free(link);
	*copied = *copied && out->n > 0;
}

// ---- Drop targets ----

// What a drag carries, as far as taking it goes.
typedef struct {
	uint32_t source;  // a drag from the app: the view it's from
	char **mimes;     // its items' MIME types
	uint32_t nmimes;
} drag_info;

static void drag_info_free(drag_info *d) {
	wh_free_cstrings(d->mimes, d->nmimes);
	memset(d, 0, sizeof *d);
}

static void add_mime(drag_info *d, const char *m, size_t len) {
	d->mimes = wh_realloc(d->mimes, (d->nmimes + 1) * sizeof *d->mimes);
	d->mimes[d->nmimes++] = wh_strndup(m, len);
}

// A top-level window's IDropTarget.
typedef struct {
	IDropTarget iface;
	LONG refs;
	HWND win;
	IDataObject *data;  // the drag over the window's data object (a reference)
	drag_info drag;
	node *target;       // the view it's over that takes it
	float at[2];        // its place in target, in points, last reported
} drop_target;

static drop_target **targets;  // the app's windows', revoked as each is destroyed
static uint32_t ntargets;

// The drag the app runs: the view it's from, and its items as it began.
static uint32_t drag_source;
static uint8_t *drag_snapshot;
static uint32_t drag_snapshot_len;

static IDropTargetHelper *drop_helper;
static bool drop_helper_made;

// helper is the shell's drop target helper, which shows a drag's image
// over the app's windows.
static IDropTargetHelper *helper(void) {
	if (!drop_helper_made) {
		drop_helper_made = true;
		if (FAILED(CoCreateInstance(&clsid_drag_drop_helper, NULL, CLSCTX_INPROC_SERVER, &iid_drop_target_helper, (void **)&drop_helper)))
			drop_helper = NULL;
	}
	return drop_helper;
}

// local_source is the view a drag of the app's is from: the private
// format, with this process's ID; 0: another app's.
static uint32_t local_source(IDataObject *d) {
	formats();
	size_t len;
	uint8_t *b = get_data(d, cf_source, &len);
	uint32_t src = 0;
	if (b && len >= 8) {
		uint32_t pid;
		memcpy(&pid, b, 4);
		if (pid == GetCurrentProcessId()) memcpy(&src, b + 4, 4);
	}
	free(b);
	return src;
}

// describe_drag finds what a drag's data carries: the items of the view
// it's from, for the app's own drag; another app's files (CF_HDROP, or
// virtual files), text and link.
static drag_info describe_drag(IDataObject *d) {
	drag_info di = {0};
	uint32_t src = local_source(d);
	if (src) {
		di.source = src;
		const uint8_t *b = drag_snapshot;
		uint32_t len = drag_snapshot_len;
		if (src != drag_source) {
			node *n = wh_node(src);
			b = n ? n->drag_items : NULL;
			len = n ? n->drag_items_len : 0;
		}
		ak_item_vec v = {0};
		if (b) ak_decode_drag_items(b, len, &v);
		for (uint32_t i = 0; i < v.len; i++) add_mime(&di, v.data[i].mime.p, v.data[i].mime.len);
		ak_vec_free(&v);
		return di;
	}
	char **files;
	uint32_t nfiles = dropped_files(d, &files);
	for (uint32_t i = 0; i < nfiles; i++) {
		char *m = file_mime(files[i]);
		add_mime(&di, m, strlen(m));
		free(m);
	}
	if (!nfiles) {
		vfile *v;
		uint32_t nv = virtual_files(d, &v);
		for (uint32_t i = 0; i < nv; i++) {
			if (v[i].dir) continue;
			char *m = file_mime(v[i].name);
			add_mime(&di, m, strlen(m));
			free(m);
		}
		free_vfiles(v, nv);
	}
	wh_free_cstrings(files, nfiles);
	if (has_data(d, CF_UNICODETEXT)) add_mime(&di, "text/plain", 10);
	if (has_data(d, cf_url_w) || has_data(d, cf_url)) add_mime(&di, "text/uri-list", 13);
	return di;
}

// point_in is a point on the screen in n, in points.
static void point_in(node *n, POINT pt, float out[2]) {
	ScreenToClient(n->hwnd, &pt);
	out[0] = wh_pt(n, pt.x), out[1] = wh_pt(n, pt.y);
}

// takes reports whether a view takes any of a drag's MIME types.
static bool takes(node *n, const drag_info *d) {
	patterns p = patterns_of(n->drop_accept, n->ndrop_accept);
	bool ok = false;
	for (uint32_t i = 0; i < d->nmimes && !ok; i++) ok = matches(p, d->mimes[i]);
	free(p.p);
	return ok;
}

// drop_target_at is the innermost view at pt in top that takes a drag.
static node *drop_target_at(HWND top, POINT pt, const drag_info *d) {
	if (!d->nmimes) return NULL;
	views_at(top, pt);
	for (uint32_t i = 0; i < nchain; i++)
		if (chain[i]->ndrop_accept && takes(chain[i], d)) return chain[i];
	return NULL;
}

static void emit_over(node *n, int state, const float at[2]) {
	ak_event e = {.kind = AK_EVENT_DRAG_OVER};
	e.g.state = (uint8_t)state;
	e.g.x = at[0], e.g.y = at[1], e.g.scale = 1;
	wh_emit(n, &e);
}

// set_target reports the drag over n (NULL: none) at pt: Cancelled for
// the view it leaves, Began for the one it comes over, Changed as it
// moves.
static void set_target(drop_target *t, node *n, POINT pt) {
	if (n != t->target) {
		node *old = t->target;
		if (old) {
			t->target = NULL;
			emit_over(old, AK_GESTURE_STATE_CANCELLED, t->at);
		}
		// Reporting the view it left ran the core: n may be gone.
		if (!n || n->dead || !n->hwnd) return;
		t->target = n;
		point_in(n, pt, t->at);
		emit_over(n, AK_GESTURE_STATE_BEGAN, t->at);
		return;
	}
	if (!n || !n->hwnd) return;
	float at[2];
	point_in(n, pt, at);
	if (at[0] != t->at[0] || at[1] != t->at[1]) {
		t->at[0] = at[0], t->at[1] = at[1];
		emit_over(n, AK_GESTURE_STATE_CHANGED, at);
	}
}

// over finds the view the drag at pt is over, reports it, and returns the
// effect of a drop there.
static DWORD over(drop_target *t, POINT pt, DWORD allowed) {
	set_target(t, drop_target_at(t->win, pt, &t->drag), pt);
	if (!t->target) return DROPEFFECT_NONE;
	// A move for the app's own drags, a copy for other apps'; else what
	// the source allows.
	DWORD prefer[3] = {DROPEFFECT_COPY, DROPEFFECT_MOVE, DROPEFFECT_LINK};
	if (t->drag.source) prefer[0] = DROPEFFECT_MOVE, prefer[1] = DROPEFFECT_COPY;
	for (int i = 0; i < 3; i++)
		if (allowed & prefer[i]) return prefer[i];
	return DROPEFFECT_NONE;
}

// end ends the drag over the window: Cancelled for the view it was over,
// and its data let go.
static void end(drop_target *t) {
	set_target(t, NULL, (POINT){0, 0});
	if (t->data) {
		IDataObject_Release(t->data);
		t->data = NULL;
	}
	drag_info_free(&t->drag);
}

static HRESULT STDMETHODCALLTYPE dt_query(IDropTarget *t, REFIID riid, void **out) {
	if (!out) return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &iid_unknown) && !IsEqualIID(riid, &iid_drop_target)) return E_NOINTERFACE;
	*out = t;
	t->lpVtbl->AddRef(t);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE dt_add_ref(IDropTarget *t) { return (ULONG)InterlockedIncrement(&((drop_target *)t)->refs); }

static ULONG STDMETHODCALLTYPE dt_release(IDropTarget *t) {
	drop_target *x = (drop_target *)t;
	LONG r = InterlockedDecrement(&x->refs);
	if (r == 0) {
		end(x);
		free(x);
	}
	return (ULONG)r;
}

static HRESULT STDMETHODCALLTYPE dt_enter(IDropTarget *it, IDataObject *data, DWORD keys, POINTL ptl, DWORD *effect) {
	(void)keys;
	drop_target *t = (drop_target *)it;
	if (!effect) return E_UNEXPECTED;
	end(t);
	IDataObject_AddRef(data);
	t->data = data;
	t->drag = describe_drag(data);
	POINT pt = {ptl.x, ptl.y};
	DWORD e = over(t, pt, *effect);
	if (helper()) IDropTargetHelper_DragEnter(drop_helper, t->win, data, &pt, e);
	*effect = e;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_over(IDropTarget *it, DWORD keys, POINTL ptl, DWORD *effect) {
	(void)keys;
	drop_target *t = (drop_target *)it;
	if (!effect) return E_UNEXPECTED;
	POINT pt = {ptl.x, ptl.y};
	DWORD e = over(t, pt, *effect);
	if (helper()) IDropTargetHelper_DragOver(drop_helper, &pt, e);
	*effect = e;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_leave(IDropTarget *it) {
	drop_target *t = (drop_target *)it;
	if (helper()) IDropTargetHelper_DragLeave(drop_helper);
	end(t);
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE dt_drop(IDropTarget *it, IDataObject *data, DWORD keys, POINTL ptl, DWORD *effect) {
	(void)keys;
	drop_target *t = (drop_target *)it;
	if (!effect) return E_UNEXPECTED;
	if (!t->data) {
		// A drop without DragEnter: what it carries, now.
		IDataObject_AddRef(data);
		t->data = data;
		t->drag = describe_drag(data);
	}
	POINT pt = {ptl.x, ptl.y};
	DWORD e = over(t, pt, *effect);
	if (helper()) IDropTargetHelper_Drop(drop_helper, data, &pt, e);
	node *n = t->target;
	if (n && e != DROPEFFECT_NONE) {
		ak_drop d = {.x = t->at[0], .y = t->at[1], .source = t->drag.source};
		items it = {0};
		if (!d.source) {
			patterns p = patterns_of(n->drop_accept, n->ndrop_accept);
			dropped_items(data, p, &it, &d.copied);
			free(p.p);
		}
		ak_buf b = {0};
		ak_append_drop(&b, &d, it.v, it.n);
		wh_emit_bytes(n, AK_EVENT_DROP, b.data, b.len);
		ak_buf_free(&b);
		free_items(&it);
	}
	end(t);
	*effect = e;
	return S_OK;
}

static IDropTargetVtbl drop_target_vtbl = {dt_query, dt_add_ref, dt_release, dt_enter, dt_over, dt_leave, dt_drop};

static drop_target *target_of(HWND win) {
	for (uint32_t i = 0; i < ntargets; i++)
		if (targets[i]->win == win) return targets[i];
	return NULL;
}

// wh_accept_drops makes a top-level window of the app's take drops.
void wh_accept_drops(HWND win) {
	if (target_of(win)) return;
	drop_target *t = wh_alloc(sizeof *t);
	t->iface.lpVtbl = &drop_target_vtbl;
	t->refs = 1;
	t->win = win;
	if (SUCCEEDED(RegisterDragDrop(win, &t->iface))) {
		targets = wh_realloc(targets, (ntargets + 1) * sizeof *targets);
		targets[ntargets++] = t;
	}
	dt_release(&t->iface);  // RegisterDragDrop keeps its own
}

// wh_window_destroyed revokes a window's drop target, and stops watching
// it for the pointer.
void wh_window_destroyed(HWND hwnd) {
	if (tracked == hwnd) tracked = NULL;
	for (uint32_t i = 0; i < ntargets; i++) {
		drop_target *t = targets[i];
		if (t->win != hwnd) continue;
		memmove(targets + i, targets + i + 1, (ntargets - i - 1) * sizeof *targets);
		ntargets--;
		dt_add_ref(&t->iface);
		end(t);
		RevokeDragDrop(hwnd);
		dt_release(&t->iface);
		return;
	}
}

// wh_forget_interaction drops a destroyed view from the states the host
// follows (the core reports leaving them itself).
void wh_forget_interaction(node *n) {
	if (node_listening(n, AK_EVENT_HOVER)) hover_views--;
	if (node_listening(n, AK_EVENT_HOVER_MOVE)) hover_views--;
	for (uint32_t i = 0; i < nhovered; i++)
		if (hovered[i] == n) {
			// hover_once reports from its own copies.
			memmove(hovered + i, hovered + i + 1, (nhovered - i - 1) * sizeof *hovered);
			nhovered--;
			break;
		}
	for (uint32_t i = 0; i < ntargets; i++)
		if (targets[i]->target == n) targets[i]->target = NULL;
	free(n->two_finger);
	n->two_finger = NULL;
}

// ---- Drag sources ----

typedef struct {
	IDropSource iface;
	LONG refs;
} drop_source;

static HRESULT STDMETHODCALLTYPE ds_query(IDropSource *t, REFIID riid, void **out) {
	if (!out) return E_POINTER;
	*out = NULL;
	if (!IsEqualIID(riid, &iid_unknown) && !IsEqualIID(riid, &iid_drop_source)) return E_NOINTERFACE;
	*out = t;
	t->lpVtbl->AddRef(t);
	return S_OK;
}

static ULONG STDMETHODCALLTYPE ds_add_ref(IDropSource *t) { return (ULONG)InterlockedIncrement(&((drop_source *)t)->refs); }

static ULONG STDMETHODCALLTYPE ds_release(IDropSource *t) {
	LONG r = InterlockedDecrement(&((drop_source *)t)->refs);
	if (r == 0) free(t);
	return (ULONG)r;
}

// Escape, or another button, cancels; letting go of the left button
// drops; the system's cursors show the effect.
static HRESULT STDMETHODCALLTYPE ds_continue(IDropSource *t, BOOL escape, DWORD keys) {
	(void)t;
	if (escape || (keys & (MK_RBUTTON | MK_MBUTTON))) return DRAGDROP_S_CANCEL;
	if (!(keys & MK_LBUTTON)) return DRAGDROP_S_DROP;
	return S_OK;
}

static HRESULT STDMETHODCALLTYPE ds_feedback(IDropSource *t, DWORD effect) {
	(void)t, (void)effect;
	return DRAGDROP_S_USEDEFAULTCURSORS;
}

static IDropSourceVtbl drop_source_vtbl = {ds_query, ds_add_ref, ds_release, ds_continue, ds_feedback};

// real_drag_drop runs a drag of data with the left button (a modal loop,
// until it's dropped or cancelled), allowing effects.
static HRESULT real_drag_drop(IDataObject *data, DWORD allowed, DWORD *effect) {
	drop_source *s = wh_alloc(sizeof *s);
	s->iface.lpVtbl = &drop_source_vtbl;
	s->refs = 1;
	*effect = DROPEFFECT_NONE;
	HRESULT hr = DoDragDrop(data, &s->iface, allowed, effect);
	ds_release(&s->iface);
	return hr;
}

// do_drag_drop is DoDragDrop's, unless the automation scripts the drag.
static HRESULT (*do_drag_drop)(IDataObject *, DWORD, DWORD *) = real_drag_drop;

// set_data puts b in a data object as format cf, in global memory the
// data object then owns.
static bool set_data(IDataObject *d, UINT cf, const void *b, size_t len) {
	HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, len ? len : 1);
	if (!g) return false;
	if (len) {
		void *p = GlobalLock(g);
		if (!p) {
			GlobalFree(g);
			return false;
		}
		memcpy(p, b, len);
		GlobalUnlock(g);
	}
	FORMATETC fe = format_of(cf, -1, TYMED_HGLOBAL);
	STGMEDIUM sm = {.tymed = TYMED_HGLOBAL, .hGlobal = g};
	if (FAILED(IDataObject_SetData(d, &fe, &sm, TRUE))) {  // fRelease: it's the data object's
		GlobalFree(g);
		return false;
	}
	return true;
}

// set_wide puts UTF-8 text in a data object as UTF-16, NUL-terminated.
static void set_wide(IDataObject *d, UINT cf, const char *s, size_t len) {
	wchar_t *w = wh_wide(s, (int)len);
	set_data(d, cf, w, (wcslen(w) + 1) * sizeof *w);
	free(w);
}

// hdrop is CF_HDROP's data for paths (UTF-8): a DROPFILES of wide
// strings, each NUL-terminated, the list too.
static void set_hdrop(IDataObject *d, char **paths, uint32_t n) {
	ak_buf b = {0};
	DROPFILES df = {.pFiles = sizeof df, .fWide = TRUE};
	ak_buf_put(&b, &df, sizeof df);
	for (uint32_t i = 0; i < n; i++) {
		wchar_t *w = wh_wide(paths[i], (int)strlen(paths[i]));
		ak_buf_put(&b, w, (wcslen(w) + 1) * sizeof *w);
		free(w);
	}
	ak_buf_put(&b, L"", sizeof(wchar_t));
	set_data(d, CF_HDROP, b.data, b.len);
	ak_buf_free(&b);
}

static IDataObject *new_data(void) {
	IDataObject *d = NULL;
	if (FAILED(SHCreateDataObject(NULL, 0, NULL, NULL, &iid_data_object, (void **)&d))) return NULL;
	return d;
}

// drag_data is the data object of a drag of view id's items: its files
// (CF_HDROP), its texts (CF_UNICODETEXT; its links' URLs if it has no
// text), its first link (UniformResourceLocatorW), and the private format
// with the process's ID and the view's, which the app's own drops know it
// by. An item that's only a value goes nowhere else.
static IDataObject *drag_data(uint32_t id, const uint8_t *b, uint32_t len, bool *files) {
	formats();
	*files = false;
	IDataObject *d = new_data();
	if (!d) return NULL;
	ak_item_vec v = {0};
	ak_decode_drag_items(b, len, &v);
	char **paths = wh_alloc((v.len ? v.len : 1) * sizeof *paths);
	uint32_t npaths = 0;
	ak_buf texts = {0}, links = {0};
	ak_span first_link = {0};
	for (uint32_t i = 0; i < v.len; i++) {
		ak_item *it = &v.data[i];
		if (it->path.len) {
			paths[npaths++] = wh_strndup(it->path.p, it->path.len);
		} else if (it->kind == AK_SHARE_KIND_LINK && it->text.len) {
			if (links.len) ak_buf_u8(&links, '\n');
			ak_buf_put(&links, it->text.p, it->text.len);
			if (!first_link.len) first_link = it->text;
		} else if (it->text.len) {
			if (texts.len) ak_buf_u8(&texts, '\n');
			ak_buf_put(&texts, it->text.p, it->text.len);
		}
	}
	if (npaths) {
		set_hdrop(d, paths, npaths);
		*files = true;
	}
	ak_buf *text = texts.len ? &texts : &links;
	if (text->len) {
		// \n (and \r\n) to \r\n.
		ak_buf crlf = {0};
		for (size_t i = 0; i < text->len; i++) {
			char c = (char)text->data[i];
			if (c == '\r' && i + 1 < text->len && text->data[i + 1] == '\n') continue;
			if (c == '\n') ak_buf_u8(&crlf, '\r');
			ak_buf_u8(&crlf, (uint8_t)c);
		}
		set_wide(d, CF_UNICODETEXT, (const char *)crlf.data, crlf.len);
		ak_buf_free(&crlf);
	}
	if (first_link.len) {
		set_wide(d, cf_url_w, first_link.p, first_link.len);
		char *a = wh_strndup(first_link.p, first_link.len);
		set_data(d, cf_url, a, first_link.len + 1);
		free(a);
	}
	uint32_t src[2] = {GetCurrentProcessId(), id};
	set_data(d, cf_source, src, sizeof src);
	wh_free_cstrings(paths, npaths);
	ak_buf_free(&texts);
	ak_buf_free(&links);
	ak_vec_free(&v);
	return d;
}

// corner_alpha is the coverage of the pixel centered at x, y by a w by h
// rectangle with corners of radius r.
static uint8_t corner_alpha(float x, float y, float w, float h, float r) {
	if (r <= 0) return 255;
	float cx = x < r ? r : x > w - r ? w - r : x;
	float cy = y < r ? r : y > h - r ? h - r : y;
	float dx = x - cx, dy = y - cy;
	if (dx == 0 && dy == 0) return 255;
	float d = sqrtf(dx * dx + dy * dy) - r;  // past the edge, in pixels
	if (d <= -0.5f) return 255;
	if (d >= 0.5f) return 0;
	return (uint8_t)((0.5f - d) * 255);
}

// drag_image gives a drag the view's image (as it draws, its rounded
// corners left out), grabbed at at.
static void drag_image(node *n, IDataObject *data, POINT at) {
	RECT r;
	GetClientRect(n->hwnd, &r);
	int w = r.right, h = r.bottom;
	if (w <= 0 || h <= 0) return;
	BITMAPINFO bi = {.bmiHeader = {sizeof bi.bmiHeader, w, -h, 1, 32, BI_RGB, 0, 0, 0, 0, 0}};
	uint8_t *px = NULL;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void **)&px, NULL, 0);
	if (!bmp || !px) {
		if (bmp) DeleteObject(bmp);
		DeleteDC(dc);
		return;
	}
	HGDIOBJ old = SelectObject(dc, bmp);
	wh_paint(n, n->hwnd, dc, r);
	HWND kids[256];
	int nkids = 0;
	for (HWND c = GetWindow(n->hwnd, GW_CHILD); c && nkids < (int)WH_LEN(kids); c = GetWindow(c, GW_HWNDNEXT))
		if (IsWindowVisible(c)) kids[nkids++] = c;
	for (int i = nkids; i-- > 0;) wh_paint_tree(kids[i], n->hwnd, dc);
	GdiFlush();
	SelectObject(dc, old);
	DeleteDC(dc);
	// Opaque (GDI's text leaves alpha 0), but for the rounded corners;
	// premultiplied.
	float radius = wh_minf(n->corner_radius * wh_scale_of(n), wh_minf((float)w / 2, (float)h / 2));
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			uint8_t *p = px + ((size_t)y * (size_t)w + (size_t)x) * 4;
			uint8_t a = corner_alpha((float)x + 0.5f, (float)y + 0.5f, (float)w, (float)h, radius);
			if (a < 255) {
				p[0] = (uint8_t)(p[0] * a / 255), p[1] = (uint8_t)(p[1] * a / 255), p[2] = (uint8_t)(p[2] * a / 255);
				p[3] = a;
			} else {
				p[3] = 255;
			}
		}
	IDragSourceHelper *helper = NULL;
	bool taken = false;
	if (SUCCEEDED(CoCreateInstance(&clsid_drag_drop_helper, NULL, CLSCTX_INPROC_SERVER, &iid_drag_source_helper, (void **)&helper)) && helper) {
		SHDRAGIMAGE img = {{w, h}, at, bmp, CLR_NONE};
		taken = SUCCEEDED(IDragSourceHelper_InitializeFromBitmap(helper, &img, data));
		IDragSourceHelper_Release(helper);
	}
	if (!taken) DeleteObject(bmp);  // the helper owns it if it took it
}

typedef struct {
	uint32_t id;
	POINT at;
} drag_job;

// drag runs a drag of view id, grabbed at at (pixels, in it): Dragging
// Began, the drag (DoDragDrop), then Dropped or Cancelled.
static void drag_now(void *ctx) {
	drag_job *j = ctx;
	uint32_t id = j->id;
	POINT at = j->at;
	free(j);
	node *n = wh_node(id);
	if (!n || !n->hwnd || !wh_draggable(n)) return;
	uint32_t len = n->drag_items_len;
	uint8_t *items = wh_alloc(len ? len : 1);
	memcpy(items, n->drag_items, len);
	bool files;
	IDataObject *data = drag_data(id, items, len, &files);
	if (!data) {
		free(items);
		return;
	}
	drag_image(n, data, at);
	// The files the app shares aren't moved away from it.
	DWORD allowed = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
	if (files) allowed = DROPEFFECT_COPY | DROPEFFECT_LINK;
	drag_source = id, drag_snapshot = items, drag_snapshot_len = len;
	wh_emit_int(n, AK_EVENT_DRAGGING, AK_DRAG_PHASE_BEGAN);
	H.modal++;
	DWORD effect = DROPEFFECT_NONE;
	HRESULT hr = do_drag_drop(data, allowed, &effect);
	H.modal--;
	drag_source = 0, drag_snapshot = NULL, drag_snapshot_len = 0;
	free(items);
	int phase = hr == DRAGDROP_S_DROP && effect != DROPEFFECT_NONE ? AK_DRAG_PHASE_DROPPED : AK_DRAG_PHASE_CANCELLED;
	if ((n = wh_node(id))) wh_emit_int(n, AK_EVENT_DRAGGING, phase);  // the drop may have removed it
	IDataObject_Release(data);
	POINT p;
	GetCursorPos(&p);
	wh_pointer_moved(WindowFromPoint(p), p, true);
}

// wh_start_drag starts dragging n, the mouse having moved past the
// system's drag distance with the button down: the press is cancelled,
// and the drag runs once the core has returned.
void wh_start_drag(node *n, HWND hwnd) {
	wh_cancel_press(n);
	n->panning = false;
	if (GetCapture() == hwnd) {
		n->releasing = true;
		ReleaseCapture();  // DoDragDrop captures the mouse itself
		n->releasing = false;
	}
	drag_job *j = wh_alloc(sizeof *j);
	j->id = n->id, j->at = n->press_at;
	wh_after_core(drag_now, j);
}

// ---- Paste ----

// bmp_file makes a packed DIB a .bmp file: a BITMAPFILEHEADER before it,
// its pixels after the header and the color table.
static uint8_t *bmp_file(const uint8_t *dib, size_t len, size_t *out_len) {
	uint32_t header, compression, colors;
	uint16_t bits;
	memcpy(&header, dib, 4);
	memcpy(&bits, dib + 14, 2);
	memcpy(&compression, dib + 16, 4);
	memcpy(&colors, dib + 32, 4);
	if (!colors && bits <= 8) colors = 1u << bits;
	uint32_t masks = header == 40 && compression == BI_BITFIELDS ? 12 : 0;
	uint32_t offset = 14 + header + masks + colors * 4;
	uint8_t *out = wh_alloc(14 + len);
	out[0] = 'B', out[1] = 'M';
	uint32_t size = (uint32_t)(14 + len);
	memcpy(out + 2, &size, 4);
	memcpy(out + 10, &offset, 4);
	memcpy(out + 14, dib, len);
	*out_len = 14 + len;
	return out;
}

// clipboard_image is an image on the clipboard: a PNG (as browsers and
// Office put one), else the bitmap as a .bmp file (free it).
static uint8_t *clipboard_image(IDataObject *d, size_t *len, const char **name, const char **mime) {
	uint8_t *b = get_data(d, cf_png, len);
	if (b && *len > 8) {
		*name = "Image.png", *mime = "image/png";
		return b;
	}
	free(b);
	UINT cfs[2] = {CF_DIBV5, CF_DIB};
	for (int i = 0; i < 2; i++) {
		size_t l;
		b = get_data(d, cfs[i], &l);
		if (!b || l < 40) {
			free(b);
			continue;
		}
		uint8_t *f = bmp_file(b, l, len);
		free(b);
		*name = "Image.bmp", *mime = "image/bmp";
		return f;
	}
	return NULL;
}

static bool write_file(const char *path, const uint8_t *b, size_t len) {
	wchar_t *w = wh_wide(path, (int)strlen(path));
	HANDLE f = CreateFileW(w, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	free(w);
	if (f == INVALID_HANDLE_VALUE) return false;
	DWORD n = 0;
	bool ok = WriteFile(f, b, (DWORD)len, &n, NULL) && n == len;
	return CloseHandle(f) && ok;
}

// paste_place is where a paste's file named name goes: the paste's
// folder in the app's caches, made with its first (*dir), or NULL.
static char *paste_place(char **dir, const char *name) {
	if (!*dir && !(*dir = drop_dir())) return NULL;
	return join(*dir, name);
}

// wh_paste_content takes the clipboard's content of the field's types,
// if it has any (files, PNG and bitmap images, copied into the app's
// caches), and reports it with Pasted; false: none, the EDIT pastes its
// text as it would.
bool wh_paste_content(node *n) {
	if (!n->npaste_types || !node_listening(n, AK_EVENT_PASTED)) return false;
	IDataObject *data = NULL;
	if (FAILED(OleGetClipboard(&data)) || !data) return false;
	formats();
	patterns accept = patterns_of(n->paste_types, n->npaste_types);
	items got = {0}, out = {0};
	bool copied;
	char *dir = NULL;
	dropped_items(data, accept, &got, &copied);
	for (uint32_t i = 0; i < got.n; i++) {
		ak_item it = got.v[i];
		if (it.path.len && !copied) {
			// Another app's file: the app's own copy, as a paste's are.
			char *src = wh_strndup(it.path.p, it.path.len);
			char *p = paste_place(&dir, base_name(src));
			wchar_t *ws = wh_wide(src, (int)strlen(src)), *wp = p ? wh_wide(p, (int)strlen(p)) : NULL;
			bool ok = p && CopyFileW(ws, wp, FALSE);
			free(ws);
			free(wp);
			free(src);
			if (!ok) {
				free(p);
				continue;
			}
			it.path = keep(&out, p);
		}
		add_item(&out, it);
	}
	if (!has_files(&out)) {
		size_t len;
		const char *name, *mime;
		uint8_t *img = clipboard_image(data, &len, &name, &mime);
		if (img && matches(accept, mime)) {
			char *p = paste_place(&dir, name);
			if (p && write_file(p, img, len)) {
				ak_item it = {.kind = AK_SHARE_KIND_IMAGE, .size = (int64_t)len};
				it.path = keep(&out, p);
				it.name = ak_cstr(name);
				it.mime = ak_cstr(mime);
				add_item(&out, it);
			} else {
				free(p);
			}
		}
		free(img);
	}
	// Text alone the EDIT pastes itself, unless the app takes text.
	bool took = out.n && (has_files(&out) || matches(accept, "text/plain"));
	if (took) {
		ak_buf b = {0};
		ak_append_items(&b, out.v, out.n);
		wh_emit_bytes(n, AK_EVENT_PASTED, b.data, b.len);
		ak_buf_free(&b);
	}
	free_items(&out);
	free_items(&got);
	free(dir);
	free(accept.p);
	IDataObject_Release(data);
	return took;
}

// ---- The automation's drags and drops (ASUKA_SCRIPT) ----

static bool shows_text(node *n, const wchar_t *t) {
	const wstr *s = n->kind == AK_KIND_TEXT_FIELD ? &n->placeholder : n->kind == AK_KIND_BUTTON ? &n->title : &n->text;
	return s->n && !wcscmp(s->p, t);
}

// showing is the view on screen showing text t (the last made, if many).
static node *showing(const wchar_t *t) {
	node *found = NULL;
	uint32_t i = 0;
	void *v;
	while (wh_map_next(&H.nodes, &i, NULL, &v)) {
		node *n = v;
		if (n->hwnd && wh_on_screen(n) && shows_text(n, t) && (!found || n->id > found->id)) found = n;
	}
	return found;
}

static POINT center(node *n) {
	RECT c;
	GetClientRect(n->hwnd, &c);
	POINT p = {c.right / 2, c.bottom / 2};
	ClientToScreen(n->hwnd, &p);
	return p;
}

// drop_at drops data on the view showing text, through its window's
// drop target, as OLE calls it: over it, moved a little, dropped.
static bool drop_at(node *on, IDataObject *data, DWORD allowed, DWORD *effect) {
	drop_target *t = target_of(GetAncestor(on->hwnd, GA_ROOT));
	if (!t) return false;
	POINT p = center(on);
	POINTL pl = {p.x, p.y};
	IDropTarget *it = &t->iface;
	dt_add_ref(it);
	DWORD e = allowed;
	it->lpVtbl->DragEnter(it, data, MK_LBUTTON, pl, &e);
	pl.x += 4;
	e = allowed;
	it->lpVtbl->DragOver(it, MK_LBUTTON, pl, &e);
	e = allowed;
	it->lpVtbl->Drop(it, data, 0, pl, &e);
	dt_release(it);
	*effect = e;
	return true;
}

static uint32_t scripted_target;  // the view a scripted drag drops on

// scripted_drag_drop is the drag a "drag" step runs: dropped on its
// target, as OLE would run it over the app's own drop target.
static HRESULT scripted_drag_drop(IDataObject *data, DWORD allowed, DWORD *effect) {
	do_drag_drop = real_drag_drop;
	*effect = DROPEFFECT_NONE;
	node *t = wh_node(scripted_target);
	scripted_target = 0;
	if (!t || !t->hwnd || !drop_at(t, data, allowed, effect)) return DRAGDROP_S_CANCEL;
	return *effect != DROPEFFECT_NONE ? DRAGDROP_S_DROP : DRAGDROP_S_CANCEL;
}

static wchar_t *next_part(wchar_t **s) {
	wchar_t *p = *s, *bar = wcschr(p, L'|');
	if (bar) *bar = 0, *s = bar + 1;
	else *s = p + wcslen(p);
	return p;
}

// wh_interaction_step runs a script's step of drags, drops, pastes and
// pinches; false: not one of them.
//
//   - drop:<view's text>|file:<path>|text:<text>|link:<url>: another
//     app's drag of those items, dropped on the view.
//   - drag:<view's text>|<target view's text>: a mouse drag of the view,
//     dropped on the target (cancelled if there's none).
//   - paste:<field's placeholder>|file:<path>|text:<text>: the clipboard
//     set to those, and pasted into the field.
//   - pinch:<view's text>|<notches>: Ctrl and the wheel over the view,
//     then the wheel resting.
bool wh_interaction_step(const char *verb, const wchar_t *arg) {
	bool drop = !strcmp(verb, "drop"), paste = !strcmp(verb, "paste");
	if (!drop && !paste && strcmp(verb, "drag") && strcmp(verb, "pinch")) return false;
	formats();
	size_t al = wcslen(arg);
	wchar_t *buf = wh_alloc((al + 1) * sizeof *buf), *rest = buf;
	memcpy(buf, arg, al * sizeof *buf);
	wchar_t *first = next_part(&rest);
	node *n = showing(first);
	if (!n) {
		int l;
		char *s = wh_utf8(first, (int)wcslen(first), &l);
		wh_log("automation: nothing shows \"%s\"", s);
		free(s);
		free(buf);
		return true;
	}
	if (!strcmp(verb, "pinch")) {
		int notches = *rest ? _wtoi(rest) : 1;
		POINT p = center(n);
		HWND w = n->hwnd;
		for (int i = 0; i < notches; i++) SendMessageW(w, WM_MOUSEWHEEL, MAKEWPARAM(MK_CONTROL, WHEEL_DELTA), MAKELPARAM(p.x, p.y));
		SendMessageW(w, WM_TIMER, TIMER_PINCH, 0);
	} else if (!strcmp(verb, "drag")) {
		node *t = *rest ? showing(rest) : NULL;
		node *src = n;
		while (src && !wh_draggable(src)) src = src->parent;
		if (src && src->hwnd) {
			scripted_target = t ? t->id : 0;
			do_drag_drop = scripted_drag_drop;
			RECT c;
			GetClientRect(src->hwnd, &c);
			int x = c.right / 2, y = c.bottom / 2, away = wh_system_metric(SM_CXDRAG, wh_dpi_of(src)) * 4 + 8;
			SendMessageW(src->hwnd, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(x, y));
			SendMessageW(src->hwnd, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(x + away, y));
			SendMessageW(src->hwnd, WM_LBUTTONUP, 0, MAKELPARAM(x + away, y));
		} else {
			wh_log("automation: nothing to drag there");
		}
	} else {
		IDataObject *d = new_data();
		char **paths = NULL;
		uint32_t npaths = 0;
		while (d && *rest) {
			wchar_t *part = next_part(&rest);
			int l;
			char *s = wh_utf8(part, (int)wcslen(part), &l);
			if (!strncmp(s, "file:", 5)) {
				paths = wh_realloc(paths, (npaths + 1) * sizeof *paths);
				paths[npaths++] = wh_strdup(s + 5);
			} else if (!strncmp(s, "text:", 5)) {
				set_wide(d, CF_UNICODETEXT, s + 5, strlen(s + 5));
			} else if (!strncmp(s, "link:", 5)) {
				set_wide(d, cf_url_w, s + 5, strlen(s + 5));
			}
			free(s);
		}
		if (d && npaths) set_hdrop(d, paths, npaths);
		wh_free_cstrings(paths, npaths);
		if (d && drop) {
			DWORD e;
			if (!drop_at(n, d, DROPEFFECT_COPY | DROPEFFECT_MOVE, &e)) wh_log("automation: that window takes no drops");
		} else if (d) {
			if (SUCCEEDED(OleSetClipboard(d))) {
				OleFlushClipboard();
				if (n->ctl) SendMessageW(n->ctl, WM_PASTE, 0, 0);
			}
		}
		if (d) IDataObject_Release(d);
	}
	free(buf);
	return true;
}
