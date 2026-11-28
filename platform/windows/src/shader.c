// shader.c: a Shader on Windows: its Kage program, compiled to HLSL with
// the app (internal/shadercode), drawn on the GPU with Direct3D 11 on a
// thread of its own. One device (the GPU's, or WARP where there's none)
// draws every shader's frames, one at a time: its immediate context serves
// one thread. A frame is a triangle over a texture of the view's size in
// pixels, drawn by the shader's pixel shader (compiled by Windows's
// compiler, once per program), copied to a staging texture and read back:
// the render thread waits for the GPU, never the main thread. The frame
// crosses back with wh_post: the main thread swaps it in and paints it over
// the view, as GDI+ paints the views around it. Inputs that change while a
// frame renders render again once it's done. Animated, a timer starts a
// frame whenever none is rendering, so the frame rate is what the GPU and
// the copy afford, up to the timer's. A view larger than a texture can be
// renders at a fraction of its pixels, which the program's positions are
// still.
#include "host.h"
#include "parts.h"
#include "draw.h"

#include <d3d11.h>
#include <d3dcompiler.h>

// ---- The shader's code (internal/shadercode's Encode) ----

typedef struct {
	char *hlsl;  // the pixel shader: asuka_ps
	uint32_t lanes, size, scale;
	int32_t time;  // -1: no float Time
	uint32_t cb_size;
	uint32_t *offsets;  // per lane, in cbuffer b0
} code;

static void code_free(code *c) {
	if (!c) return;
	free(c->hlsl);
	free(c->offsets);
	free(c);
}

// decode_code reads Encode's bytes: NULL, with why, if they're bad or
// carry no HLSL.
static code *decode_code(const uint8_t *b, size_t n, const char **why) {
	size_t i = 0;
	bool bad = false;
#define U32() (i + 4 <= n ? (i += 4, wh_u32(b + i - 4)) : (bad = true, 0u))
#define SKIP_STR() \
	do { \
		uint32_t l_ = U32(); \
		if (bad || l_ > n - i) bad = true; \
		else i += l_; \
	} while (0)
	SKIP_STR();  // Kage
	SKIP_STR();  // Metal
	SKIP_STR();  // AGSL
	code *c = wh_alloc(sizeof *c);
	c->lanes = U32();
	c->size = U32();
	c->scale = U32();
	c->time = (int32_t)U32() - 1;
	U32();  // the Metal buffer's size
	if (!bad && c->lanes > (1u << 16)) bad = true;
	for (uint32_t l = 0; l < c->lanes && !bad; l++) U32();  // Metal's offsets
	uint32_t nu = U32();
	for (uint32_t u = 0; u < nu && !bad; u++) {
		SKIP_STR();
		U32();
		U32();
		if (i < n) i++;
		else bad = true;
	}
	if (!bad && i < n) SKIP_STR();  // GLSL
	if (bad || i >= n) {
		*why = bad ? "bad code" : "its code has no HLSL: build the app again";
		code_free(c);
		return NULL;
	}
	uint32_t hl = U32();
	if (bad || hl > n - i) {
		*why = "bad code";
		code_free(c);
		return NULL;
	}
	c->hlsl = wh_strndup((const char *)b + i, hl);
	i += hl;
	c->cb_size = U32();
	c->offsets = wh_alloc((c->lanes ? c->lanes : 1) * sizeof *c->offsets);
	for (uint32_t l = 0; l < c->lanes; l++) c->offsets[l] = U32();
	if (bad || c->size + 1 >= c->lanes || c->scale >= c->lanes || (c->time >= 0 && (uint32_t)c->time >= c->lanes) || c->cb_size > 65536) {
		*why = "bad code";
		code_free(c);
		return NULL;
	}
	for (uint32_t l = 0; l < c->lanes; l++)
		if (c->offsets[l] + 4 > c->cb_size) {
			*why = "bad code";
			code_free(c);
			return NULL;
		}
#undef U32
#undef SKIP_STR
	return c;
}

// ---- The GPU's side: the render thread's alone ----

// A shader's own on the GPU: the texture its frames draw into, the one
// they're copied to for the CPU, and its constant buffers.
typedef struct {
	ID3D11Device *device;  // the device it was made on
	int w, h;
	ID3D11Texture2D *tex, *staging;
	ID3D11RenderTargetView *rtv;
	ID3D11Buffer *cb, *step_cb;
	uint32_t cb_size;
	uint8_t *cbuf;
} target;

typedef struct pipeline {
	char *hlsl;
	ID3D11PixelShader *ps;
	char *err;
	struct pipeline *next;
} pipeline;

static struct {
	ID3D11Device *device;
	ID3D11DeviceContext *ctx;
	D3D_FEATURE_LEVEL level;
	ID3D11VertexShader *vs;
	char *err;  // why there's no device
	pipeline *pipelines;
} d3d;

typedef HRESULT(WINAPI *compile_fn)(const void *, SIZE_T, const char *, const D3D_SHADER_MACRO *, ID3DInclude *, const char *, const char *, UINT,
	UINT, ID3DBlob **, ID3DBlob **);
static compile_fn compiler;

static char *errorf(const char *fmt, ...) {
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return wh_strdup(buf);
}

// compile compiles the function entry of an HLSL source for target (ps_5_0,
// vs_4_0...), optimized: its bytecode blob, or why not.
static ID3DBlob *compile(const char *src, const char *entry, const char *tgt, char **err) {
	if (!compiler) {
		HMODULE m = LoadLibraryExW(L"d3dcompiler_47.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (m) compiler = (compile_fn)(void *)GetProcAddress(m, "D3DCompile");
		if (!compiler) {
			*err = errorf("d3d11: no shader compiler (d3dcompiler_47.dll)");
			return NULL;
		}
	}
	ID3DBlob *blob = NULL, *errs = NULL;
	HRESULT hr = compiler(src, strlen(src), "shader.hlsl", NULL, NULL, entry, tgt, D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_IEEE_STRICTNESS, 0,
		&blob, &errs);
	if (FAILED(hr) || !blob) {
		if (errs) *err = errorf("d3d11: the shader doesn't compile: %.*s", (int)ID3D10Blob_GetBufferSize(errs), (const char *)ID3D10Blob_GetBufferPointer(errs));
		else *err = errorf("d3d11: the shader doesn't compile: %#lx", (unsigned long)hr);
		if (blob) ID3D10Blob_Release(blob);
		blob = NULL;
	}
	if (errs) ID3D10Blob_Release(errs);
	return blob;
}

// shader_model is the shader model of the device's feature level: 5 on
// 11_0 and later, 4 before.
static const char *shader_model(const char *stage) {
	static char buf[8];
	snprintf(buf, sizeof buf, "%s_%d_0", stage, d3d.level >= D3D_FEATURE_LEVEL_11_0 ? 5 : 4);
	return buf;
}

static const char vertex_src[] =
	"float4 asuka_vs(uint id : SV_VertexID) : SV_Position {\n"
	"\tfloat2 p = float2((id << 1) & 2, id & 2);\n"
	"\treturn float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
	"}\n";

#define RELEASE(p) \
	do { \
		if (p) (p)->lpVtbl->Release(p); \
		(p) = NULL; \
	} while (0)

// make_device makes the device and the vertex shader the first time.
static bool make_device(char **err) {
	if (d3d.device) return true;
	if (d3d.err) {
		*err = wh_strdup(d3d.err);
		return false;
	}
	static const D3D_FEATURE_LEVEL all[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
	HRESULT hr = E_FAIL;
	static const D3D_DRIVER_TYPE drivers[] = {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP};
	for (size_t d = 0; d < 2 && !d3d.device; d++) {
		const D3D_FEATURE_LEVEL *levels = all;
		UINT nl = WH_LEN(all);
		while (nl) {
			hr = D3D11CreateDevice(NULL, drivers[d], NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, nl, D3D11_SDK_VERSION, &d3d.device, &d3d.level,
				&d3d.ctx);
			if (SUCCEEDED(hr)) break;
			d3d.device = NULL, d3d.ctx = NULL;
			// Direct3D 11.0's runtime refuses 11_1 (E_INVALIDARG): without it.
			if (hr != E_INVALIDARG || levels[0] != D3D_FEATURE_LEVEL_11_1) break;
			levels++, nl--;
		}
	}
	if (!d3d.device) {
		d3d.err = errorf("d3d11: no device: %#lx", (unsigned long)hr);
		*err = wh_strdup(d3d.err);
		return false;
	}
	ID3DBlob *code = compile(vertex_src, "asuka_vs", shader_model("vs"), err);
	if (code) {
		hr = ID3D11Device_CreateVertexShader(d3d.device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &d3d.vs);
		ID3D10Blob_Release(code);
		if (FAILED(hr)) *err = errorf("d3d11: no vertex shader: %#lx", (unsigned long)hr);
	}
	if (!d3d.vs) {
		RELEASE(d3d.ctx);
		RELEASE(d3d.device);
		d3d.err = wh_strdup(*err);
		return false;
	}
	return true;
}

// reset lets go of a lost device and what was made on it; targets make
// theirs again on the next.
static void reset(void) {
	while (d3d.pipelines) {
		pipeline *p = d3d.pipelines;
		d3d.pipelines = p->next;
		RELEASE(p->ps);
		free(p->hlsl);
		free(p->err);
		free(p);
	}
	RELEASE(d3d.vs);
	RELEASE(d3d.ctx);
	RELEASE(d3d.device);
}

// pipeline_of is a program's pixel shader, compiled the first time.
static pipeline *pipeline_of(const char *hlsl) {
	for (pipeline *p = d3d.pipelines; p; p = p->next)
		if (!strcmp(p->hlsl, hlsl)) return p;
	pipeline *p = wh_alloc(sizeof *p);
	p->hlsl = wh_strdup(hlsl);
	p->next = d3d.pipelines;
	d3d.pipelines = p;
	ID3DBlob *code = compile(hlsl, "asuka_ps", shader_model("ps"), &p->err);
	if (code) {
		HRESULT hr = ID3D11Device_CreatePixelShader(d3d.device, ID3D10Blob_GetBufferPointer(code), ID3D10Blob_GetBufferSize(code), NULL, &p->ps);
		ID3D10Blob_Release(code);
		if (FAILED(hr)) p->err = errorf("d3d11: no pixel shader: %#lx", (unsigned long)hr);
	}
	return p;
}

static void target_free(target *t) {
	RELEASE(t->rtv);
	RELEASE(t->tex);
	RELEASE(t->staging);
	RELEASE(t->cb);
	RELEASE(t->step_cb);
	free(t->cbuf);
	t->cbuf = NULL;
	t->device = NULL;
	t->w = t->h = 0;
	t->cb_size = 0;
}

static ID3D11Buffer *constant_buffer(uint32_t size, char **err) {
	D3D11_BUFFER_DESC desc = {size, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
	ID3D11Buffer *b = NULL;
	HRESULT hr = ID3D11Device_CreateBuffer(d3d.device, &desc, NULL, &b);
	if (FAILED(hr)) *err = errorf("d3d11: no constant buffer: %#lx", (unsigned long)hr);
	return b;
}

// prepare makes the target's textures for frames of w by h pixels, and
// its constant buffers, on the current device.
static bool prepare(target *t, int w, int h, uint32_t cb_size, char **err) {
	if (t->device != d3d.device) {
		target_free(t);
		t->device = d3d.device;
	}
	if (!t->step_cb && !(t->step_cb = constant_buffer(16, err))) return false;
	if (t->cb_size != cb_size) {
		RELEASE(t->cb);
		free(t->cbuf);
		t->cbuf = NULL, t->cb_size = 0;
		if (cb_size) {
			if (!(t->cb = constant_buffer(cb_size, err))) return false;
			t->cb_size = cb_size;
			t->cbuf = wh_alloc(cb_size);
		}
	}
	if (t->tex && t->w == w && t->h == h) return true;
	RELEASE(t->rtv);
	RELEASE(t->tex);
	RELEASE(t->staging);
	t->w = t->h = 0;
	D3D11_TEXTURE2D_DESC desc = {(UINT)w, (UINT)h, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
	HRESULT hr = ID3D11Device_CreateTexture2D(d3d.device, &desc, NULL, &t->tex);
	if (FAILED(hr)) {
		*err = errorf("d3d11: no texture of %dx%d: %#lx", w, h, (unsigned long)hr);
		return false;
	}
	hr = ID3D11Device_CreateRenderTargetView(d3d.device, (ID3D11Resource *)t->tex, NULL, &t->rtv);
	if (FAILED(hr)) {
		*err = errorf("d3d11: no render target: %#lx", (unsigned long)hr);
		return false;
	}
	desc.Usage = D3D11_USAGE_STAGING, desc.BindFlags = 0, desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	hr = ID3D11Device_CreateTexture2D(d3d.device, &desc, NULL, &t->staging);
	if (FAILED(hr)) {
		*err = errorf("d3d11: no staging texture: %#lx", (unsigned long)hr);
		return false;
	}
	t->w = w, t->h = h;
	return true;
}

// A frame to render, and what it gave.
typedef struct job {
	target *t;
	bool release;  // let the target go instead
	uint32_t id, gen;
	code *c;  // the render thread's own copy
	uint32_t *in;
	int w, h;
	uint8_t *buf;
	size_t buf_cap;
	// What it gave: premultiplied BGRA, pw by ph, or why not.
	uint8_t *pix;
	int pw, ph;
	char *err;
	struct job *next;
} job;

// draw draws the frame on the device, then reads it back.
static void draw(job *j) {
	pipeline *p = pipeline_of(j->c->hlsl);
	if (p->err) {
		j->err = wh_strdup(p->err);
		return;
	}
	// A view larger than a texture can be renders at a fraction of its
	// pixels.
	int m = d3d.level >= D3D_FEATURE_LEVEL_11_0 ? 16384 : 8192;
	int pw = j->w, ph = j->h;
	if (pw > m || ph > m) {
		double f = fmin((double)m / pw, (double)m / ph);
		pw = wh_maxi(1, wh_mini(m, (int)(pw * f)));
		ph = wh_maxi(1, wh_mini(m, (int)(ph * f)));
	}
	target *t = j->t;
	if (!prepare(t, pw, ph, j->c->cb_size, &j->err)) return;
	ID3D11DeviceContext *ctx = d3d.ctx;
	if (t->cb) {
		for (uint32_t i = 0; i < j->c->lanes; i++) memcpy(t->cbuf + j->c->offsets[i], &j->in[i], 4);
		ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t->cb, 0, NULL, t->cbuf, 0, 0);
	}
	float step[4] = {(float)j->w / (float)pw, (float)j->h / (float)ph, 0, 0};
	ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)t->step_cb, 0, NULL, step, 0, 0);
	ID3D11DeviceContext_ClearState(ctx);
	ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &t->rtv, NULL);
	D3D11_VIEWPORT vp = {0, 0, (float)pw, (float)ph, 0, 1};
	ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
	ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	ID3D11DeviceContext_VSSetShader(ctx, d3d.vs, NULL, 0);
	ID3D11DeviceContext_PSSetShader(ctx, p->ps, NULL, 0);
	ID3D11Buffer *cbs[2] = {t->cb, t->step_cb};
	ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 2, cbs);
	ID3D11DeviceContext_Draw(ctx, 3, 0);
	ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)t->staging, (ID3D11Resource *)t->tex);
	D3D11_MAPPED_SUBRESOURCE mp;
	HRESULT hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)t->staging, 0, D3D11_MAP_READ, 0, &mp);
	if (FAILED(hr)) {
		j->err = errorf("d3d11: the frame doesn't map: %#lx", (unsigned long)hr);
		return;
	}
	size_t row = (size_t)pw * 4, size = row * (size_t)ph;
	uint8_t *pix = j->buf && j->buf_cap >= size ? j->buf : NULL;
	if (!pix) {
		free(j->buf);
		pix = wh_alloc(size);
	}
	j->buf = NULL;
	for (int y = 0; y < ph; y++) memcpy(pix + (size_t)y * row, (const uint8_t *)mp.pData + (size_t)y * mp.RowPitch, row);
	ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)t->staging, 0);
	j->pix = pix, j->pw = pw, j->ph = ph;
}

// render draws a frame, again on a new device if the GPU was lost.
static void render(job *j) {
	for (int k = 0; k < 2; k++) {
		free(j->err);
		j->err = NULL;
		if (!make_device(&j->err)) return;
		draw(j);
		if (!j->err || ID3D11Device_GetDeviceRemovedReason(d3d.device) == S_OK) return;
		reset();
	}
}

static struct {
	SRWLOCK mu;
	CONDITION_VARIABLE more;
	job *head, *tail;
	bool started;
} queue = {SRWLOCK_INIT, CONDITION_VARIABLE_INIT, NULL, NULL, false};

static void rendered(void *ctx);

static DWORD WINAPI render_thread(void *arg) {
	(void)arg;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	for (;;) {
		AcquireSRWLockExclusive(&queue.mu);
		while (!queue.head) SleepConditionVariableSRW(&queue.more, &queue.mu, INFINITE, 0);
		job *j = queue.head;
		queue.head = j->next;
		if (!queue.head) queue.tail = NULL;
		ReleaseSRWLockExclusive(&queue.mu);
		if (j->release) {
			target_free(j->t);
			free(j->t);
			free(j);
			continue;
		}
		render(j);
		wh_post(rendered, j);
	}
	return 0;
}

static void enqueue(job *j) {
	AcquireSRWLockExclusive(&queue.mu);
	if (!queue.started) {
		HANDLE t = CreateThread(NULL, 0, render_thread, NULL, 0, NULL);
		if (t) CloseHandle(t);
		queue.started = true;
	}
	j->next = NULL;
	if (queue.tail) queue.tail->next = j;
	else queue.head = j;
	queue.tail = j;
	WakeConditionVariable(&queue.more);
	ReleaseSRWLockExclusive(&queue.mu);
}

// ---- The view (main thread) ----

struct wh_shader {
	code *c;
	bool failed;
	uint32_t *words;  // the core's lanes
	uint32_t nwords;
	bool animated;
	int64_t start;  // ns
	bool busy;      // a frame is rendering
	bool dirty;     // and the inputs changed since it started
	GpBitmap *bmp;  // the frame on show, over pix, pw by ph
	uint8_t *pix;
	int pw, ph;
	uint8_t *spare;  // the buffer the next frame renders into
	size_t spare_cap;
	uint32_t *last;  // the inputs of the last frame started
	int last_w, last_h;
	target *t;     // the GPU's side, which only the render thread touches
	uint32_t gen;  // a frame started for an older program is dropped
};

static code *code_copy(const code *c) {
	code *d = wh_alloc(sizeof *d);
	*d = *c;
	d->hlsl = wh_strdup(c->hlsl);
	d->offsets = wh_alloc((c->lanes ? c->lanes : 1) * sizeof *d->offsets);
	memcpy(d->offsets, c->offsets, c->lanes * sizeof *d->offsets);
	return d;
}

void wh_shader_new(node *n) {
	wh_shader *s = wh_alloc(sizeof *s);
	s->start = wh_now_ns();
	s->t = wh_alloc(sizeof *s->t);
	n->shader = s;
}

// shader_clock runs the timer of an animated shader's frames.
static void shader_clock(node *n) {
	wh_shader *s = n->shader;
	if (!n->hwnd || !s) return;
	if (s->animated && s->c && s->c->time >= 0) SetTimer(n->hwnd, TIMER_SHADER, 16, NULL);
	else KillTimer(n->hwnd, TIMER_SHADER);
}

static void set_words(wh_shader *s, uint32_t n) {
	if (s->nwords == n) return;
	free(s->words);
	s->words = wh_alloc((n ? n : 1) * sizeof *s->words);
	s->nwords = n;
}

void wh_set_shader_prop(node *n, uint16_t p, const ak_value *v) {
	wh_shader *s = n->shader;
	if (!s) return;
	switch (p) {
	case AK_PROP_SHADER_CODE: {
		code_free(s->c);
		s->c = NULL;
		s->failed = false;
		s->gen++;
		free(s->last);
		s->last = NULL;
		const char *why = NULL;
		s->c = decode_code((const uint8_t *)v->s.p, v->s.len, &why);
		if (!s->c) {
			s->failed = true;
			wh_log("a ui.Shader doesn't run: %s", why);
			break;
		}
		set_words(s, s->c->lanes);
		break;
	}
	case AK_PROP_SHADER_UNIFORMS: {
		uint32_t nw = (uint32_t)(v->s.len / 4);
		set_words(s, nw);
		for (uint32_t i = 0; i < nw; i++) s->words[i] = wh_u32((const uint8_t *)v->s.p + 4 * i);
		break;
	}
	case AK_PROP_SHADER_ANIMATED:
		s->animated = v->i != 0;
		shader_clock(n);
		break;
	}
	wh_render_shader(n);
}

static uint32_t bits(float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

void wh_render_shader(node *n) {
	wh_shader *s = n->shader;
	if (!s || !s->c || s->failed || !n->hwnd) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	int w = r.right - r.left, h = r.bottom - r.top;
	code *c = s->c;
	if (w <= 0 || h <= 0 || s->nwords != c->lanes) return;
	if (s->busy) {
		s->dirty = true;
		return;
	}
	float k = wh_scale_of(n);
	uint32_t *in = wh_alloc(c->lanes * sizeof *in);
	memcpy(in, s->words, c->lanes * sizeof *in);
	in[c->size] = bits((float)w / k);
	in[c->size + 1] = bits((float)h / k);
	in[c->scale] = bits(k);
	if (c->time >= 0) in[c->time] = bits((float)((double)(wh_now_ns() - s->start) / 1e9));
	if (w == s->last_w && h == s->last_h && s->last && !memcmp(in, s->last, c->lanes * sizeof *in)) {
		free(in);
		return;
	}
	free(s->last);
	s->last = wh_alloc(c->lanes * sizeof *in);
	memcpy(s->last, in, c->lanes * sizeof *in);
	s->last_w = w, s->last_h = h;
	job *j = wh_alloc(sizeof *j);
	j->t = s->t, j->id = n->id, j->gen = s->gen, j->c = code_copy(c), j->in = in, j->w = w, j->h = h;
	j->buf = s->spare, j->buf_cap = s->spare_cap;
	s->spare = NULL, s->spare_cap = 0;
	s->busy = true, s->dirty = false;
	enqueue(j);
}

static void job_free(job *j) {
	code_free(j->c);
	free(j->in);
	free(j->buf);
	free(j->pix);
	free(j->err);
	free(j);
}

static void rendered(void *ctx) {
	job *j = ctx;
	node *n = wh_node(j->id);
	wh_shader *s = n ? n->shader : NULL;
	if (!s || s->t != j->t) {
		job_free(j);
		return;
	}
	s->busy = false;
	if (s->gen == j->gen) {
		if (j->err) {
			s->failed = true;
			wh_log("a ui.Shader doesn't run: %s", j->err);
		} else if (j->pix) {
			if (s->bmp) GdipDisposeImage((GpImage *)s->bmp);
			free(s->spare);
			s->spare = s->pix, s->spare_cap = (size_t)s->pw * (size_t)s->ph * 4;
			s->pix = j->pix, s->pw = j->pw, s->ph = j->ph;
			j->pix = NULL;
			s->bmp = g_bitmap(s->pw, s->ph, s->pix);
			wh_redraw(n);
		}
	}
	bool again = s->dirty || s->gen != j->gen;
	job_free(j);
	if (again) {
		s->dirty = false;
		wh_render_shader(n);
	}
}

static GpImageAttributes *clamp_attrs;

void wh_paint_shader(node *n, gfx g, float w, float h, float radius) {
	wh_shader *s = n->shader;
	if (!s) return;
	RECT r;
	GetClientRect(n->hwnd, &r);
	if (r.right - r.left != s->last_w || r.bottom - r.top != s->last_h) wh_render_shader(n);  // resized: a frame for the new size
	if (!s->bmp) return;
	GpPath *clip = NULL;
	if (radius > 0) {
		clip = g_path();
		float rr[4];
		wh_radii(n, w, h, rr);  // its corners, each its own
		g_path_corners(clip, 0, 0, w, h, rr);
		g_clip_path(g, clip);
	}
	// The frame's edges clamped, not blended with transparent past them.
	if (!clamp_attrs && GdipCreateImageAttributes(&clamp_attrs) == Ok) GdipSetImageAttributesWrapMode(clamp_attrs, WrapModeTileFlipXY, 0, FALSE);
	GdipDrawImageRectRect(g, (GpImage *)s->bmp, 0, 0, w, h, 0, 0, (float)s->pw, (float)s->ph, UnitPixel, clamp_attrs, NULL, NULL);
	if (clip) {
		g_reset_clip(g);
		GdipDeletePath(clip);
	}
}

void wh_shader_release(node *n) {
	wh_shader *s = n->shader;
	if (!s) return;
	if (n->hwnd) KillTimer(n->hwnd, TIMER_SHADER);
	if (s->bmp) GdipDisposeImage((GpImage *)s->bmp);
	free(s->pix);
	free(s->spare);
	free(s->words);
	free(s->last);
	code_free(s->c);
	// The target goes on the render thread, after the frames before it (a
	// frame rendering is dropped when it's back).
	job *j = wh_alloc(sizeof *j);
	j->t = s->t, j->release = true;
	enqueue(j);
	free(s);
	n->shader = NULL;
}
