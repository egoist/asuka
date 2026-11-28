// shader.c: Shader views. The compiler translated the Kage program to
// GLSL ES 3.00 as it compiled the app (internal/shadercode: the web
// host's program, the last string of the Code prop); its uniforms are U0,
// U1... in the order of the code's, their values the core's lanes, and
// the size, scale and Time lanes the host's own.
//
// One GL context of the display's draws every shader's frames, each into
// a texture of its own, a triangle over the whole of it, through a
// framebuffer. The texture becomes a GdkGLTexture (with a fence GTK waits
// on, so nothing waits for the GPU here), which GTK's renderer draws from
// its own context: they share their objects, so no frame is copied. When
// GTK lets go of a frame, its texture goes back to a pool the next frames
// draw into, as GtkGLArea's do. On desktop GL (not GLES), the program's
// version line is GL 3.3's. GL's rows go up, GTK's down: the frame is
// drawn flipped. A frame renders as the view snapshots, when its inputs
// changed since the last one: its uniforms, its size in pixels, the scale,
// and Time, which an animated shader's frame clock moves on each frame.
//
// GL's functions are libepoxy's (which GTK draws through): it exports a
// pointer for each, to a stub that resolves it for the current context.
// Where GDK has no GL (no driver, GDK_DISABLE=gl) a Shader draws nothing,
// and says so once.
#include "host.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef unsigned int GLuint, GLenum, GLbitfield;
typedef int GLint, GLsizei;
typedef float GLfloat;
typedef char GLchar;
typedef unsigned char GLboolean;
typedef struct __GLsync *GLsync;

enum {
	GL_TRIANGLES = 4,
	GL_TEXTURE_2D = 0x0DE1,
	GL_BLEND = 0x0BE2,
	GL_SCISSOR_TEST = 0x0C11,
	GL_DEPTH_TEST = 0x0B71,
	GL_MAX_TEXTURE_SIZE = 0x0D33,
	GL_UNSIGNED_BYTE = 0x1401,
	GL_RGBA = 0x1908,
	GL_RGBA8 = 0x8058,
	GL_LINEAR = 0x2601,
	GL_TEXTURE_MAG_FILTER = 0x2800,
	GL_TEXTURE_MIN_FILTER = 0x2801,
	GL_TEXTURE_WRAP_S = 0x2802,
	GL_TEXTURE_WRAP_T = 0x2803,
	GL_CLAMP_TO_EDGE = 0x812F,
	GL_COLOR_BUFFER_BIT = 0x4000,
	GL_FRAMEBUFFER = 0x8D40,
	GL_COLOR_ATTACHMENT0 = 0x8CE0,
	GL_FRAMEBUFFER_COMPLETE = 0x8CD5,
	GL_FRAGMENT_SHADER = 0x8B30,
	GL_VERTEX_SHADER = 0x8B31,
	GL_COMPILE_STATUS = 0x8B81,
	GL_LINK_STATUS = 0x8B82,
	GL_INFO_LOG_LENGTH = 0x8B84,
	GL_ACTIVE_UNIFORMS = 0x8B86,
	GL_SYNC_GPU_COMMANDS_COMPLETE = 0x9117,
	GL_FLOAT = 0x1406,
	GL_FLOAT_VEC2 = 0x8B50,
	GL_FLOAT_VEC3 = 0x8B51,
	GL_FLOAT_VEC4 = 0x8B52,
	GL_INT = 0x1404,
	GL_INT_VEC2 = 0x8B53,
	GL_INT_VEC3 = 0x8B54,
	GL_INT_VEC4 = 0x8B55,
	GL_BOOL = 0x8B56,
	GL_BOOL_VEC2 = 0x8B57,
	GL_BOOL_VEC3 = 0x8B58,
	GL_BOOL_VEC4 = 0x8B59,
	GL_FLOAT_MAT2 = 0x8B5A,
	GL_FLOAT_MAT3 = 0x8B5B,
	GL_FLOAT_MAT4 = 0x8B5C,
	GL_UNSIGNED_INT = 0x1405,
	GL_UNSIGNED_INT_VEC2 = 0x8DC6,
	GL_UNSIGNED_INT_VEC3 = 0x8DC7,
	GL_UNSIGNED_INT_VEC4 = 0x8DC8,
};

// GL's functions: name, result, parameters.
#define GL_FUNCS(X)                                                                                                                   \
	X(glGenVertexArrays, void, (GLsizei n, GLuint *arrays))                                                                          \
	X(glBindVertexArray, void, (GLuint array))                                                                                       \
	X(glGenFramebuffers, void, (GLsizei n, GLuint *fbs))                                                                             \
	X(glBindFramebuffer, void, (GLenum target, GLuint fb))                                                                           \
	X(glFramebufferTexture2D, void, (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level))                \
	X(glCheckFramebufferStatus, GLenum, (GLenum target))                                                                             \
	X(glGetIntegerv, void, (GLenum pname, GLint *data))                                                                              \
	X(glGenTextures, void, (GLsizei n, GLuint *textures))                                                                            \
	X(glDeleteTextures, void, (GLsizei n, const GLuint *textures))                                                                   \
	X(glBindTexture, void, (GLenum target, GLuint texture))                                                                          \
	X(glTexParameteri, void, (GLenum target, GLenum pname, GLint param))                                                             \
	X(glTexImage2D, void, (GLenum target, GLint level, GLint internal, GLsizei w, GLsizei h, GLint border, GLenum format, GLenum type, const void *pixels)) \
	X(glViewport, void, (GLint x, GLint y, GLsizei w, GLsizei h))                                                                    \
	X(glDisable, void, (GLenum cap))                                                                                                 \
	X(glClearColor, void, (GLfloat r, GLfloat g, GLfloat b, GLfloat a))                                                               \
	X(glClear, void, (GLbitfield mask))                                                                                              \
	X(glUseProgram, void, (GLuint program))                                                                                          \
	X(glDrawArrays, void, (GLenum mode, GLint first, GLsizei count))                                                                 \
	X(glFenceSync, GLsync, (GLenum condition, GLbitfield flags))                                                                     \
	X(glDeleteSync, void, (GLsync sync))                                                                                             \
	X(glFlush, void, (void))                                                                                                         \
	X(glCreateShader, GLuint, (GLenum type))                                                                                         \
	X(glShaderSource, void, (GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length))                         \
	X(glCompileShader, void, (GLuint shader))                                                                                        \
	X(glGetShaderiv, void, (GLuint shader, GLenum pname, GLint *params))                                                             \
	X(glGetShaderInfoLog, void, (GLuint shader, GLsizei size, GLsizei *length, GLchar *log))                                          \
	X(glDeleteShader, void, (GLuint shader))                                                                                         \
	X(glCreateProgram, GLuint, (void))                                                                                               \
	X(glAttachShader, void, (GLuint program, GLuint shader))                                                                         \
	X(glLinkProgram, void, (GLuint program))                                                                                         \
	X(glGetProgramiv, void, (GLuint program, GLenum pname, GLint *params))                                                           \
	X(glGetProgramInfoLog, void, (GLuint program, GLsizei size, GLsizei *length, GLchar *log))                                        \
	X(glDeleteProgram, void, (GLuint program))                                                                                       \
	X(glGetActiveUniform, void, (GLuint program, GLuint index, GLsizei size, GLsizei *length, GLint *usize, GLenum *type, GLchar *name)) \
	X(glGetUniformLocation, GLint, (GLuint program, const GLchar *name))                                                             \
	X(glUniform1fv, void, (GLint loc, GLsizei n, const GLfloat *v))                                                                  \
	X(glUniform2fv, void, (GLint loc, GLsizei n, const GLfloat *v))                                                                  \
	X(glUniform3fv, void, (GLint loc, GLsizei n, const GLfloat *v))                                                                  \
	X(glUniform4fv, void, (GLint loc, GLsizei n, const GLfloat *v))                                                                  \
	X(glUniform1iv, void, (GLint loc, GLsizei n, const GLint *v))                                                                    \
	X(glUniform2iv, void, (GLint loc, GLsizei n, const GLint *v))                                                                    \
	X(glUniform3iv, void, (GLint loc, GLsizei n, const GLint *v))                                                                    \
	X(glUniform4iv, void, (GLint loc, GLsizei n, const GLint *v))                                                                    \
	X(glUniform1uiv, void, (GLint loc, GLsizei n, const GLuint *v))                                                                  \
	X(glUniform2uiv, void, (GLint loc, GLsizei n, const GLuint *v))                                                                  \
	X(glUniform3uiv, void, (GLint loc, GLsizei n, const GLuint *v))                                                                  \
	X(glUniform4uiv, void, (GLint loc, GLsizei n, const GLuint *v))                                                                  \
	X(glUniformMatrix2fv, void, (GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v))                                       \
	X(glUniformMatrix3fv, void, (GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v))                                       \
	X(glUniformMatrix4fv, void, (GLint loc, GLsizei n, GLboolean transpose, const GLfloat *v))

static struct {
#define X(name, ret, args) ret(*name) args;
	GL_FUNCS(X)
#undef X
} gl;

// A texture a frame drew into, and the fence after it.
typedef struct {
	GLuint tex;
	int w, h;
	GLsync sync;
} gl_frame;

typedef struct {
	GLint loc;
	GLenum type;
	uint32_t lane, lanes;
} setter;

// A shader's program, linked, and where each active uniform's values come
// from: its uniform's lanes.
typedef struct {
	char *glsl;
	GLuint id;
	bool failed;
	AK_VEC(setter) setters;
} gl_program;

static struct {
	bool tried;
	GdkGLContext *ctx;
	bool es;
	GLuint vao, fbo;
	int max_size;
	AK_VEC(gl_program *) programs;
	pthread_mutex_t mu;    // free, which GTK's destroy notifications fill
	AK_VEC(gl_frame) free;  // frames GTK let go of
	AK_VEC(gl_frame) pool;  // their textures, to draw into again (their syncs deleted)
} glr = {.mu = PTHREAD_MUTEX_INITIALIZER};

typedef struct {
	char *glsl;
	uint32_t lanes, size, scale;
	int time;  // -1: none
	struct {
		uint32_t lane, lanes;
	} *uniforms;
	uint32_t nuniforms;
} shader_code;

typedef struct {
	shader_code code;
	bool has_code, animated;
	AK_VEC(uint32_t) words;  // the core's lanes
	gint64 start;
	guint tick;
	GdkTexture *texture;  // the frame on show
	AK_VEC(uint32_t) last;  // the inputs of the last frame rendered
	int last_w, last_h;
} kage;

static kage *kage_of(node *n) { return n->kind == AK_KIND_SHADER ? n->ext : NULL; }

// gl_ready reports whether shaders draw with GL, making the context the
// first time.
static bool gl_ready(void) {
	if (glr.tried) return glr.ctx != NULL;
	glr.tried = true;
	const char *why = NULL;
	if (!lx_lib_load(ASUKA_LIB_EPOXY)) {
		why = "no libepoxy";
	} else {
#define X(name, ret, args)                       \
	{                                            \
		void **p = lx_sym("epoxy_" #name);       \
		gl.name = p ? (ret(*) args) * p : NULL;  \
		if (!gl.name) why = "no " #name;         \
	}
		GL_FUNCS(X)
#undef X
	}
	GError *e = NULL;
	GdkGLContext *ctx = NULL;
	if (!why) {
		ctx = gdk_display_create_gl_context(gdk_display_get_default(), &e);
		if (!ctx) why = e ? e->message : "GDK has no GL";
	}
	if (ctx && !gdk_gl_context_realize(ctx, &e)) {
		why = e ? e->message : "GDK's GL context doesn't realize";
		g_object_unref(ctx);
		ctx = NULL;
	}
	if (ctx) {
		int major, minor;
		gdk_gl_context_get_version(ctx, &major, &minor);
		bool es = gdk_gl_context_get_use_es(ctx);
		if (major < 3 || (!es && major == 3 && minor < 3)) {
			why = "its GL is older than 3.3 (GLES 3.0)";
			g_object_unref(ctx);
			ctx = NULL;
		}
		glr.es = es;
	}
	if (!ctx) {
		lx_log("a Shader draws nothing: %s", why);
		if (e) g_error_free(e);
		return false;
	}
	glr.ctx = ctx;
	gdk_gl_context_make_current(ctx);
	gl.glGenVertexArrays(1, &glr.vao);
	gl.glGenFramebuffers(1, &glr.fbo);
	GLint m = 0;
	gl.glGetIntegerv(GL_MAX_TEXTURE_SIZE, &m);
	glr.max_size = m > 1024 ? m : 1024;
	gdk_gl_context_clear_current();
	return true;
}

// release takes a frame back from GTK (any thread).
static void release(gpointer data) {
	gl_frame *f = data;
	pthread_mutex_lock(&glr.mu);
	ak_vec_push(&glr.free, *f);
	pthread_mutex_unlock(&glr.mu);
	free(f);
}

// reap deletes the fences of the frames GTK let go of and pools their
// textures, keeping a few.
static void reap(void) {
	pthread_mutex_lock(&glr.mu);
	for (uint32_t i = 0; i < glr.free.len; i++) {
		gl_frame f = glr.free.data[i];
		if (f.sync) gl.glDeleteSync(f.sync);
		f.sync = NULL;
		ak_vec_push(&glr.pool, f);
	}
	glr.free.len = 0;
	pthread_mutex_unlock(&glr.mu);
	while (glr.pool.len > 8) {
		gl.glDeleteTextures(1, &glr.pool.data[0].tex);
		memmove(glr.pool.data, glr.pool.data + 1, (glr.pool.len - 1) * sizeof *glr.pool.data);
		glr.pool.len--;
	}
}

// take_frame is a texture of w by h pixels to draw into: a pooled one of
// that size, or a new one.
static gl_frame take_frame(int w, int h) {
	for (uint32_t i = 0; i < glr.pool.len; i++)
		if (glr.pool.data[i].w == w && glr.pool.data[i].h == h) {
			gl_frame f = glr.pool.data[i];
			glr.pool.data[i] = glr.pool.data[--glr.pool.len];
			return f;
		}
	gl_frame f = {0, w, h, NULL};
	gl.glGenTextures(1, &f.tex);
	gl.glBindTexture(GL_TEXTURE_2D, f.tex);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	gl.glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	gl.glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	gl.glBindTexture(GL_TEXTURE_2D, 0);
	return f;
}

// versioned is a GLSL ES 3.00 program for the context: as it is on GLES,
// GL 3.3's on desktop GL (malloc'd).
static char *versioned(const char *src) {
	const char *es = "#version 300 es";
	if (glr.es || strncmp(src, es, strlen(es))) return str_dup(ak_cstr(src));
	ak_buf b = {0};
	ak_buf_put(&b, "#version 330 core", 17);
	ak_buf_put(&b, src + strlen(es), strlen(src + strlen(es)));
	char *out = str_dup(ak_strn((const char *)b.data, b.len));
	ak_buf_free(&b);
	return out;
}

static GLuint compile(GLenum kind, const char *src) {
	char *s = versioned(src);
	GLuint sh = gl.glCreateShader(kind);
	gl.glShaderSource(sh, 1, (const GLchar *const *)&s, NULL);
	gl.glCompileShader(sh);
	free(s);
	GLint ok = 0;
	gl.glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (ok) return sh;
	char log[2048] = "";
	gl.glGetShaderInfoLog(sh, sizeof log, NULL, log);
	lx_log("a Shader doesn't run: GL doesn't compile it: %s", log);
	gl.glDeleteShader(sh);
	return 0;
}

static const char vertex_src[] =
	"#version 300 es\n"
	"void main() {\n"
	"  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
	"  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

// program_of is c's program, compiled and linked the first time it draws.
static gl_program *program_of(const shader_code *c) {
	for (uint32_t i = 0; i < glr.programs.len; i++)
		if (!strcmp(glr.programs.data[i]->glsl, c->glsl)) return glr.programs.data[i];
	gl_program *p = calloc(1, sizeof *p);
	if (!p) abort();
	p->glsl = str_dup(ak_cstr(c->glsl));
	ak_vec_push(&glr.programs, p);
	GLuint v = compile(GL_VERTEX_SHADER, vertex_src), f = v ? compile(GL_FRAGMENT_SHADER, c->glsl) : 0;
	if (!v || !f) {
		if (v) gl.glDeleteShader(v);
		p->failed = true;
		return p;
	}
	GLuint id = gl.glCreateProgram();
	gl.glAttachShader(id, v);
	gl.glAttachShader(id, f);
	gl.glLinkProgram(id);
	gl.glDeleteShader(v);
	gl.glDeleteShader(f);
	GLint ok = 0;
	gl.glGetProgramiv(id, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[2048] = "";
		gl.glGetProgramInfoLog(id, sizeof log, NULL, log);
		lx_log("a Shader doesn't run: GL doesn't link it: %s", log);
		gl.glDeleteProgram(id);
		p->failed = true;
		return p;
	}
	p->id = id;
	GLint count = 0;
	gl.glGetProgramiv(id, GL_ACTIVE_UNIFORMS, &count);
	for (GLint i = 0; i < count; i++) {
		char name[128];
		GLint size;
		GLenum type;
		gl.glGetActiveUniform(id, (GLuint)i, sizeof name, NULL, &size, &type, name);
		if (name[0] != 'U' || name[1] < '0' || name[1] > '9') continue;
		char *end;
		long k = strtol(name + 1, &end, 10);
		if (*end && strcmp(end, "[0]")) continue;
		if (k < 0 || (uint32_t)k >= c->nuniforms) continue;
		ak_vec_push(&p->setters, ((setter){gl.glGetUniformLocation(id, name), type, c->uniforms[k].lane, c->uniforms[k].lanes}));
	}
	return p;
}

static void set_uniform(const setter *s, const uint32_t *in, uint32_t n) {
	if (s->lane + s->lanes > n) return;
	const void *v = in + s->lane;
	GLsizei c = 1;
	const GLfloat *f = v;
	const GLint *i = v;
	const GLuint *u = v;
	switch (s->type) {
	case GL_FLOAT: gl.glUniform1fv(s->loc, (GLsizei)s->lanes, f); break;
	case GL_FLOAT_VEC2: gl.glUniform2fv(s->loc, (GLsizei)s->lanes / 2, f); break;
	case GL_FLOAT_VEC3: gl.glUniform3fv(s->loc, (GLsizei)s->lanes / 3, f); break;
	case GL_FLOAT_VEC4: gl.glUniform4fv(s->loc, (GLsizei)s->lanes / 4, f); break;
	case GL_FLOAT_MAT2: gl.glUniformMatrix2fv(s->loc, (GLsizei)s->lanes / 4, 0, f); break;
	case GL_FLOAT_MAT3: gl.glUniformMatrix3fv(s->loc, (GLsizei)s->lanes / 9, 0, f); break;
	case GL_FLOAT_MAT4: gl.glUniformMatrix4fv(s->loc, (GLsizei)s->lanes / 16, 0, f); break;
	case GL_INT:
	case GL_BOOL: gl.glUniform1iv(s->loc, (GLsizei)s->lanes, i); break;
	case GL_INT_VEC2:
	case GL_BOOL_VEC2: gl.glUniform2iv(s->loc, (GLsizei)s->lanes / 2, i); break;
	case GL_INT_VEC3:
	case GL_BOOL_VEC3: gl.glUniform3iv(s->loc, (GLsizei)s->lanes / 3, i); break;
	case GL_INT_VEC4:
	case GL_BOOL_VEC4: gl.glUniform4iv(s->loc, (GLsizei)s->lanes / 4, i); break;
	case GL_UNSIGNED_INT: gl.glUniform1uiv(s->loc, (GLsizei)s->lanes, u); break;
	case GL_UNSIGNED_INT_VEC2: gl.glUniform2uiv(s->loc, (GLsizei)s->lanes / 2, u); break;
	case GL_UNSIGNED_INT_VEC3: gl.glUniform3uiv(s->loc, (GLsizei)s->lanes / 3, u); break;
	case GL_UNSIGNED_INT_VEC4: gl.glUniform4uiv(s->loc, (GLsizei)s->lanes / 4, u); break;
	}
	(void)c;
}

// render draws a frame of the shader c with the lanes in, w by h pixels,
// and returns it as a GdkTexture (NULL if it didn't draw).
static GdkTexture *render(const shader_code *c, const uint32_t *in, uint32_t nin, int w, int h) {
	GdkGLContext *prev = gdk_gl_context_get_current();
	if (prev) g_object_ref(prev);
	gdk_gl_context_make_current(glr.ctx);
	GdkTexture *t = NULL;
	reap();
	gl_program *p = program_of(c);
	if (!p->failed) {
		// A view larger than a texture can be renders at a fraction of its
		// pixels (the GL draws its points at those, its size the view's).
		int tw = w, th = h, m = glr.max_size;
		if (tw > m || th > m) {
			double f = fmin((double)m / tw, (double)m / th);
			tw = (int)fmax(1, fmin(m, tw * f));
			th = (int)fmax(1, fmin(m, th * f));
		}
		gl_frame f = take_frame(tw, th);
		gl.glBindFramebuffer(GL_FRAMEBUFFER, glr.fbo);
		gl.glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f.tex, 0);
		if (gl.glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
			gl.glDeleteTextures(1, &f.tex);
			lx_log("a Shader has no framebuffer to draw into");
		} else {
			gl.glViewport(0, 0, tw, th);
			gl.glDisable(GL_BLEND);
			gl.glDisable(GL_SCISSOR_TEST);
			gl.glDisable(GL_DEPTH_TEST);
			gl.glClearColor(0, 0, 0, 0);
			gl.glClear(GL_COLOR_BUFFER_BIT);
			gl.glUseProgram(p->id);
			for (uint32_t i = 0; i < p->setters.len; i++) set_uniform(&p->setters.data[i], in, nin);
			gl.glBindVertexArray(glr.vao);
			gl.glDrawArrays(GL_TRIANGLES, 0, 3);
			f.sync = gl.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
			gl.glFlush();
			gl.glBindFramebuffer(GL_FRAMEBUFFER, 0);
			GdkGLTextureBuilder *b = gdk_gl_texture_builder_new();
			gdk_gl_texture_builder_set_context(b, glr.ctx);
			gdk_gl_texture_builder_set_id(b, f.tex);
			gdk_gl_texture_builder_set_width(b, tw);
			gdk_gl_texture_builder_set_height(b, th);
			gdk_gl_texture_builder_set_format(b, GDK_MEMORY_R8G8B8A8_PREMULTIPLIED);
			gdk_gl_texture_builder_set_sync(b, f.sync);
			gl_frame *keep = malloc(sizeof *keep);
			if (!keep) abort();
			*keep = f;
			t = gdk_gl_texture_builder_build(b, release, keep);
			g_object_unref(b);
		}
	}
	if (prev) {
		gdk_gl_context_make_current(prev);
		g_object_unref(prev);
	} else {
		gdk_gl_context_clear_current();
	}
	return t;
}

// ---- The view ----

static void free_code(shader_code *c) {
	free(c->glsl);
	free(c->uniforms);
	*c = (shader_code){0};
}

// decode reads the Code prop: strings u32-length (Kage, Metal, AGSL); u32
// lanes, size, scale, time+1; u32 Metal's size, a u32 per lane; u32
// uniform count, per uniform a string name, u32 lane, u32 lanes, u8 Kind;
// then the GLSL program, a string.
static bool decode(ak_span b, shader_code *c) {
	ak_reader r = {(const uint8_t *)b.p, b.len, 0, false};
	uint32_t n;
	for (int i = 0; i < 3; i++) ak_read_bytes(&r, &n);
	c->lanes = ak_read_u32(&r);
	c->size = ak_read_u32(&r);
	c->scale = ak_read_u32(&r);
	c->time = (int)ak_read_u32(&r) - 1;
	ak_read_u32(&r);
	if (r.err || c->lanes > 1 << 16) return false;
	for (uint32_t i = 0; i < c->lanes; i++) ak_read_u32(&r);
	c->nuniforms = ak_read_u32(&r);
	if (r.err || c->nuniforms > 1 << 16) return false;
	c->uniforms = calloc(c->nuniforms ? c->nuniforms : 1, sizeof *c->uniforms);
	if (!c->uniforms) abort();
	for (uint32_t i = 0; i < c->nuniforms; i++) {
		ak_read_bytes(&r, &n);
		c->uniforms[i].lane = ak_read_u32(&r);
		c->uniforms[i].lanes = ak_read_u32(&r);
		if (r.off < r.len) ak_read_u8(&r);
	}
	const uint8_t *g = r.off < r.len ? ak_read_bytes(&r, &n) : NULL;
	if (r.err || !g || !n) return false;
	c->glsl = str_dup(ak_strn((const char *)g, n));
	return c->size + 1 < c->lanes && c->scale < c->lanes && c->time < (int)c->lanes;
}

static void free_kage(node *n) {
	kage *k = n->ext;
	if (k->tick && n->widget) gtk_widget_remove_tick_callback(n->widget, k->tick);
	if (k->texture) g_object_unref(k->texture);
	free_code(&k->code);
	ak_vec_free(&k->words);
	ak_vec_free(&k->last);
	free(k);
	n->ext = NULL;
}

static bool kage_create(node *n) {
	if (n->kind != AK_KIND_SHADER) return false;
	kage *k = calloc(1, sizeof *k);
	if (!k) abort();
	k->start = g_get_monotonic_time();
	n->ext = k;
	n->ext_free = free_kage;
	return true;
}

static gboolean kage_tick(GtkWidget *w, GdkFrameClock *clock, gpointer data) {
	(void)w;
	(void)clock;
	node *n = data;
	kage *k = kage_of(n);
	if (!k || !k->tick) return 0;
	lx_queue_draw(n);
	return 1;
}

// clock runs an animated shader's frames on its view's frame clock: each
// frame queues a draw, which renders one with the new Time.
static void clock_on(node *n, kage *k) {
	bool on = k->animated && k->has_code && k->code.time >= 0 && n->widget;
	if (on == (k->tick != 0)) return;
	if (!on) {
		gtk_widget_remove_tick_callback(n->widget, k->tick);
		k->tick = 0;
		return;
	}
	k->tick = gtk_widget_add_tick_callback(n->widget, kage_tick, n, NULL);
}

static void resize_words(kage *k, uint32_t n) {
	ak_vec_reserve(&k->words, n);
	for (uint32_t i = k->words.len; i < n; i++) k->words.data[i] = 0;
	k->words.len = n;
}

static bool kage_prop(node *n, ak_prop p, const ak_value *v) {
	kage *k = kage_of(n);
	if (!k) return false;
	switch (p) {
	case AK_PROP_SHADER_CODE:
		free_code(&k->code);
		k->has_code = false;
		k->last.len = 0;
		if (!decode(v->s, &k->code)) {
			lx_log("a Shader doesn't run: its code has no GLSL program");
			free_code(&k->code);
			break;
		}
		k->has_code = true;
		if (k->words.len != k->code.lanes) resize_words(k, k->code.lanes);
		break;
	case AK_PROP_SHADER_UNIFORMS: {
		uint32_t c = (uint32_t)(v->s.len / 4);
		resize_words(k, c);
		memcpy(k->words.data, v->s.p, (size_t)c * 4);
		break;
	}
	case AK_PROP_SHADER_ANIMATED: k->animated = v->i != 0; break;
	default: return false;
	}
	clock_on(n, k);
	lx_queue_draw(n);
	return true;
}

static uint32_t f32_bits(float f) {
	uint32_t u;
	memcpy(&u, &f, 4);
	return u;
}

// render_if_changed renders a frame of n's shader if its inputs changed
// since the last one.
static void render_if_changed(node *n, kage *k) {
	if (!k->has_code || !n->widget || !n->placed || !gl_ready()) return;
	const shader_code *c = &k->code;
	float s = lx_scale_of(n);
	int w = (int)ceilf(n->frame[2] * s), h = (int)ceilf(n->frame[3] * s);
	if (w <= 0 || h <= 0 || k->words.len != c->lanes) return;
	uint32_t *in = malloc((size_t)c->lanes * 4);
	if (!in) abort();
	memcpy(in, k->words.data, (size_t)c->lanes * 4);
	in[c->size] = f32_bits((float)w / s);
	in[c->size + 1] = f32_bits((float)h / s);
	in[c->scale] = f32_bits(s);
	if (c->time >= 0) in[c->time] = f32_bits((float)((double)(g_get_monotonic_time() - k->start) / 1e6));
	bool same = w == k->last_w && h == k->last_h && k->last.len == c->lanes && !memcmp(in, k->last.data, (size_t)c->lanes * 4);
	if (!same) {
		k->last.len = 0;
		ak_vec_reserve(&k->last, c->lanes);
		memcpy(k->last.data, in, (size_t)c->lanes * 4);
		k->last.len = c->lanes;
		k->last_w = w;
		k->last_h = h;
		GdkTexture *t = render(c, in, c->lanes, w, h);
		if (t) {
			if (k->texture) g_object_unref(k->texture);
			k->texture = t;
		}
	}
	free(in);
}

// draw draws the frame on show over the view, flipped (GL's rows go up),
// clipped to its corners, after rendering a new one if its inputs changed.
static bool kage_draw(node *n, GtkSnapshot *snap, const graphene_rect_t *b) {
	kage *k = kage_of(n);
	if (!k) return false;
	render_if_changed(n, k);
	if (!k->texture) return true;
	bool rounded = lx_rounded_node(n) && !n->clip;  // a clipping view clips it already
	if (rounded) {
		GskRoundedRect rr = lx_node_rounded(n, *b);
		gtk_snapshot_push_rounded_clip(snap, &rr);
	}
	gtk_snapshot_save(snap);
	graphene_point_t to = {b->origin.x, b->origin.y + b->size.height};
	gtk_snapshot_translate(snap, &to);
	gtk_snapshot_scale(snap, 1, -1);
	graphene_rect_t r = {{0, 0}, b->size};
	gtk_snapshot_append_scaled_texture(snap, k->texture, GSK_SCALING_FILTER_LINEAR, &r);
	gtk_snapshot_restore(snap);
	if (rounded) gtk_snapshot_pop(snap);
	return true;
}

const area lx_area_shader = {
	.name = "shader",
	.create = kage_create,
	.prop = kage_prop,
	.draw = kage_draw,
};
