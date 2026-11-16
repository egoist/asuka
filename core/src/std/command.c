// command.c: std/command's builtins: other programs, run as children,
// their arguments a list (no shell between), their standard streams
// pipes, the null device or the program's own. fork and exec on Unix
// (the program found on PATH before the fork; exec's failure reported
// through a pipe closed on exec), CreateProcessW on Windows (the command
// line quoted as the C runtime reads it back). What blocks (a pipe's
// read, a write, the wait) runs on a thread of its own and posts back to
// the main thread, so nothing holds it. Phones and the web run no other
// programs.
//
//	command.procSpawn(program, args, cwd, env, clearEnv, stdin, stdout, stderr) -> int
//	    a process's handle (> 0), or a failure: -1 unsupported, -2 no
//	    such program (or directory), -3 it can't start (failure() says why);
//	    the streams' modes: 0 inherit, 1 piped, 2 null
//	command.procFailure() -> string        why the last spawn failed
//	command.procPid(h) -> int
//	command.procRead(h, fd) -> Task<bytes?>   a piece of stdout (1) or stderr (2); none at its end
//	command.procReadAll(h, fd) -> Task<bytes> the rest of it, to its end
//	command.procWrite(h, data) -> Task<bool>  all of data into stdin; false once it's closed
//	command.procCloseStdin(h)
//	command.procWait(h) -> Task<int>       its exit code (a signal's: 128 + it); a cancelled wait kills it
//	command.procKill(h, signal)            15 term, 9 kill, 2 int, 1 hup (Windows: it ends)
//	command.procRelease(h)                 its pipes closed, its handle let go
//	command.bundledPath(name) -> string?   the app's program name ([bin]): in
//	    $ASUKA_BIN_DIR (asuka dev's), the Mac's Contents/Helpers, or beside
//	    the executable; none when it isn't there
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#elif !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE
#endif
#include "std/std.h"
#include "rt/sched.h"
#include "base/thread.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <crt_externs.h>
#endif
#if defined(__ANDROID__) || defined(__wasm__) || (defined(__APPLE__) && TARGET_OS_IPHONE)
#define AK_NO_COMMANDS 1
#endif

#if !defined(AK_NO_COMMANDS)
#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#endif

#define STR(i) ((ak_str *)args[i].p)

static AK_TLS char failure[512];

static ak_val failure_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){.p = ak_str_c(failure)};
}

#if defined(AK_NO_COMMANDS)

static ak_val spawn_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	snprintf(failure, sizeof failure, "an app here can't run other programs (a program and a desktop app can)");
	return (ak_val){.i = -1};
}
static ak_val none_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	return (ak_val){0};
}
static ak_val failed_task(ak_val *args, uint32_t n) {
	AK_UNUSED(args), AK_UNUSED(n);
	ak_future *f = ak_future_new(AK_K_REF);
	ak_throw_msg("no such process");
	ak_future_reject(f, ak_error_take());
	return (ak_val){.p = f};
}

const ak_lib_entry ak_std_command[] = {
	{"command.procSpawn", spawn_fn},
	{"command.procFailure", failure_fn},
	{"command.procPid", none_fn},
	{"command.procRead", failed_task},
	{"command.procReadAll", failed_task},
	{"command.procWrite", failed_task},
	{"command.procCloseStdin", none_fn},
	{"command.procWait", failed_task},
	{"command.procKill", none_fn},
	{"command.procRelease", none_fn},
	{"command.bundledPath", none_fn},
	{NULL, NULL},
};

#else

#if defined(_WIN32)
typedef HANDLE fd_t;
#define NO_FD NULL
#else
typedef int fd_t;
#define NO_FD (-1)
#endif

// A child: main thread only, but for what a job copies to its thread.
typedef struct {
	int64_t handle;
	int64_t pid;
#if defined(_WIN32)
	HANDLE process;
#endif
	fd_t in, out, err;  // the parent's ends of the pipes (NO_FD: none)
	bool reading[3];    // a read in flight, per fd
	bool writing, waiting, waited, released;
	int64_t code;
	int jobs;  // jobs in flight: the entry stays until they've posted
} proc;

static AK_VEC(proc *) procs;
static int64_t next_handle = 1;

static proc *find(int64_t h) {
	for (uint32_t i = 0; i < procs.len; i++)
		if (procs.data[i]->handle == h) return procs.data[i];
	return NULL;
}

static void close_fd(fd_t *f) {
	if (*f == NO_FD) return;
#if defined(_WIN32)
	CloseHandle(*f);
#else
	close(*f);
#endif
	*f = NO_FD;
}

// forget frees p once it's released and nothing works for it.
static void forget(proc *p) {
	if (!p->released || p->jobs > 0) return;
	close_fd(&p->in);
	close_fd(&p->out);
	close_fd(&p->err);
#if defined(_WIN32)
	if (p->process) CloseHandle(p->process);
#endif
	for (uint32_t i = 0; i < procs.len; i++) {
		if (procs.data[i] == p) {
			procs.data[i] = procs.data[--procs.len];
			break;
		}
	}
	ak_free(p);
}

// ---- Spawning ----

static ak_arr *arr_arg(ak_val v) { return v.p; }

#if defined(_WIN32)

static wchar_t *wide(const char *s, size_t n) {
	int k = MultiByteToWideChar(CP_UTF8, 0, s, (int)n, NULL, 0);
	wchar_t *w = ak_alloc(sizeof(wchar_t) * ((size_t)k + 1));
	MultiByteToWideChar(CP_UTF8, 0, s, (int)n, w, k);
	w[k] = 0;
	return w;
}

typedef struct {
	wchar_t *data;
	size_t len, cap;
} wbuf;

static void wput(wbuf *b, const wchar_t *s, size_t n) {
	if (b->len + n + 1 > b->cap) {
		b->cap = (b->len + n + 1) * 2;
		b->data = ak_realloc(b->data, sizeof(wchar_t) * b->cap);
	}
	memcpy(b->data + b->len, s, sizeof(wchar_t) * n);
	b->len += n;
	b->data[b->len] = 0;
}

// quote appends an argument as the C runtime's command line parser reads
// it back (CommandLineToArgvW): quoted when it has a space, a tab or a
// quote, its backslashes doubled before a quote.
static void quote(wbuf *b, const wchar_t *a) {
	if (*a && !wcspbrk(a, L" \t\n\v\"")) {
		wput(b, a, wcslen(a));
		return;
	}
	wput(b, L"\"", 1);
	for (const wchar_t *p = a;; p++) {
		size_t slashes = 0;
		while (*p == L'\\') p++, slashes++;
		if (!*p) {
			for (size_t i = 0; i < slashes * 2; i++) wput(b, L"\\", 1);
			break;
		}
		if (*p == L'"') {
			for (size_t i = 0; i < slashes * 2 + 1; i++) wput(b, L"\\", 1);
		} else {
			for (size_t i = 0; i < slashes; i++) wput(b, L"\\", 1);
		}
		wput(b, p, 1);
	}
	wput(b, L"\"", 1);
}

static int env_cmp(const void *a, const void *b) { return _wcsicmp(*(wchar_t *const *)a, *(wchar_t *const *)b); }

// pipe_for makes the child's end of stream (mode 1 a pipe, 2 NUL, 0 the
// program's own) and the parent's (out, for a pipe).
static HANDLE child_end(int mode, bool input, DWORD std, HANDLE *parent) {
	*parent = NULL;
	SECURITY_ATTRIBUTES sa = {sizeof sa, NULL, TRUE};
	if (mode == 1) {
		HANDLE r, w;
		if (!CreatePipe(&r, &w, &sa, 0)) return INVALID_HANDLE_VALUE;
		*parent = input ? w : r;
		SetHandleInformation(*parent, HANDLE_FLAG_INHERIT, 0);
		return input ? r : w;
	}
	if (mode == 2) return CreateFileW(L"NUL", input ? GENERIC_READ : GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, NULL);
	HANDLE h = GetStdHandle(std);
	HANDLE dup = INVALID_HANDLE_VALUE;
	if (h && h != INVALID_HANDLE_VALUE) DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &dup, 0, TRUE, DUPLICATE_SAME_ACCESS);
	return dup;
}

static int64_t spawn(proc *p, const ak_str *prog, const ak_arr *argv, const ak_str *cwd, const ak_arr *env, bool clear, const int modes[3]) {
	wbuf line = {0};
	wchar_t *wp = wide(prog->data, prog->len);
	quote(&line, wp);
	ak_free(wp);
	for (uint32_t i = 0; i < argv->len; i++) {
		const ak_str *a = argv->data[i].p;
		wchar_t *w = wide(a->data, a->len);
		wput(&line, L" ", 1);
		quote(&line, w);
		ak_free(w);
	}
	// The environment: the program's (unless clear), env's over it, sorted
	// as Windows keeps it.
	AK_VEC(wchar_t *) vars = {0};
	if (!clear) {
		wchar_t *block = GetEnvironmentStringsW();
		for (wchar_t *e = block; e && *e; e += wcslen(e) + 1) {
			size_t k = wcslen(e) + 1;
			wchar_t *c = ak_alloc(sizeof(wchar_t) * k);
			memcpy(c, e, sizeof(wchar_t) * k);
			ak_vec_push(&vars, c);
		}
		if (block) FreeEnvironmentStringsW(block);
	}
	for (uint32_t i = 0; env && i + 1 < env->len; i += 2) {
		const ak_str *k = env->data[i].p, *v = env->data[i + 1].p;
		wchar_t *wk = wide(k->data, k->len);
		size_t kl = wcslen(wk);
		for (uint32_t j = 0; j < vars.len; j++) {
			if (_wcsnicmp(vars.data[j], wk, kl) == 0 && vars.data[j][kl] == L'=') {
				ak_free(vars.data[j]);
				vars.data[j] = vars.data[--vars.len];
				break;
			}
		}
		wchar_t *wv = wide(v->data, v->len);
		size_t n = kl + 1 + wcslen(wv) + 1;
		wchar_t *kv = ak_alloc(sizeof(wchar_t) * n);
		swprintf(kv, n, L"%ls=%ls", wk, wv);
		ak_free(wk);
		ak_free(wv);
		ak_vec_push(&vars, kv);
	}
	qsort(vars.data, vars.len, sizeof *vars.data, env_cmp);
	wbuf block = {0};
	for (uint32_t i = 0; i < vars.len; i++) {
		wput(&block, vars.data[i], wcslen(vars.data[i]) + 1);
		ak_free(vars.data[i]);
	}
	ak_vec_free(&vars);
	wput(&block, L"\0", 1);
	HANDLE ends[3], parents[3];
	DWORD stds[3] = {STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE};
	for (int i = 0; i < 3; i++) ends[i] = child_end(modes[i], i == 0, stds[i], &parents[i]);
	STARTUPINFOW si;
	memset(&si, 0, sizeof si);
	si.cb = sizeof si;
	si.dwFlags = STARTF_USESTDHANDLES;
	si.hStdInput = ends[0];
	si.hStdOutput = ends[1];
	si.hStdError = ends[2];
	PROCESS_INFORMATION pi;
	wchar_t *wcwd = cwd->len ? wide(cwd->data, cwd->len) : NULL;
	BOOL ok = CreateProcessW(NULL, line.data, NULL, NULL, TRUE, CREATE_UNICODE_ENVIRONMENT, block.data, wcwd, &si, &pi);
	DWORD e = GetLastError();
	ak_free(wcwd);
	ak_free(line.data);
	ak_free(block.data);
	for (int i = 0; i < 3; i++)
		if (ends[i] && ends[i] != INVALID_HANDLE_VALUE) CloseHandle(ends[i]);
	if (!ok) {
		for (int i = 0; i < 3; i++)
			if (parents[i]) CloseHandle(parents[i]);
		snprintf(failure, sizeof failure, "%s: %s", prog->data, e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? "no such program" : "it can't start");
		if (e == ERROR_DIRECTORY && cwd->len) snprintf(failure, sizeof failure, "%s: no directory %s", prog->data, cwd->data);
		return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? -ENOENT : -EIO;
	}
	CloseHandle(pi.hThread);
	p->process = pi.hProcess;
	p->pid = pi.dwProcessId;
	p->in = parents[0];
	p->out = parents[1];
	p->err = parents[2];
	return 0;
}

#else

// look finds prog as exec would on PATH (a name with a / as it is).
static char *look(const char *prog, const char *path_env) {
	if (strchr(prog, '/')) return strcpy(ak_alloc(strlen(prog) + 1), prog);
	const char *path = path_env && *path_env ? path_env : "/usr/local/bin:/usr/bin:/bin";
	size_t pl = strlen(prog);
	for (const char *s = path;;) {
		const char *e = strchr(s, ':');
		size_t n = e ? (size_t)(e - s) : strlen(s);
		char *cand = ak_alloc(n + pl + 3);
		if (n == 0) snprintf(cand, n + pl + 3, "./%s", prog);
		else snprintf(cand, n + pl + 3, "%.*s/%s", (int)n, s, prog);
		struct stat st;
		if (stat(cand, &st) == 0 && S_ISREG(st.st_mode) && access(cand, X_OK) == 0) return cand;
		ak_free(cand);
		if (!e) break;
		s = e + 1;
	}
	return NULL;
}

static char **env_list(void) {
#if defined(__APPLE__)
	return *_NSGetEnviron();
#else
	extern char **environ;
	return environ;
#endif
}

static void cloexec(int fd) { fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC); }

static int64_t spawn(proc *p, const ak_str *prog, const ak_arr *argv, const ak_str *cwd, const ak_arr *env, bool clear, const int modes[3]) {
	// The environment: the program's (unless clear), env's over it.
	AK_VEC(char *) envp = {0};
	const char *path_env = NULL;
	for (uint32_t i = 0; env && i + 1 < env->len; i += 2) {
		const ak_str *k = env->data[i].p, *v = env->data[i + 1].p;
		char *kv = ak_alloc(k->len + v->len + 2);
		snprintf(kv, k->len + v->len + 2, "%s=%s", k->data, v->data);
		ak_vec_push(&envp, kv);
		if (strcmp(k->data, "PATH") == 0) path_env = v->data;
	}
	if (!clear) {
		for (char **e = env_list(); e && *e; e++) {
			const char *eq = strchr(*e, '=');
			if (!eq) continue;
			bool over = false;
			for (uint32_t i = 0; env && i + 1 < env->len; i += 2) {
				const ak_str *k = env->data[i].p;
				if ((size_t)(eq - *e) == k->len && memcmp(*e, k->data, k->len) == 0) over = true;
			}
			if (over) continue;
			ak_vec_push(&envp, strcpy(ak_alloc(strlen(*e) + 1), *e));
		}
		if (!path_env) path_env = getenv("PATH");
	}
	ak_vec_push(&envp, NULL);
	char *exe = look(prog->data, path_env);
	int64_t rc = 0;
	char **av = ak_alloc(sizeof(char *) * (argv->len + 2));
	av[0] = (char *)prog->data;
	for (uint32_t i = 0; i < argv->len; i++) av[i + 1] = ((ak_str *)argv->data[i].p)->data;
	av[argv->len + 1] = NULL;
	int pipes[3][2] = {{-1, -1}, {-1, -1}, {-1, -1}};
	int report[2] = {-1, -1};
	if (!exe) {
		snprintf(failure, sizeof failure, "%s: no such program (looked on PATH)", prog->data);
		rc = -ENOENT;
		goto done;
	}
	for (int i = 0; i < 3; i++) {
		if (modes[i] == 1) {
			if (pipe(pipes[i]) != 0) {
				snprintf(failure, sizeof failure, "%s: %s", prog->data, strerror(errno));
				rc = -errno;
				goto done;
			}
			cloexec(pipes[i][0]);
			cloexec(pipes[i][1]);
		}
	}
	if (pipe(report) != 0) {
		rc = -errno;
		goto done;
	}
	cloexec(report[0]);
	cloexec(report[1]);
	fflush(stdout);
	fflush(stderr);
	pid_t pid = fork();
	if (pid < 0) {
		snprintf(failure, sizeof failure, "%s: %s", prog->data, strerror(errno));
		rc = -errno;
		goto done;
	}
	if (pid == 0) {
		// The child: only what's safe after a fork, then exec.
		for (int i = 0; i < 3; i++) {
			if (modes[i] == 1) {
				dup2(i == 0 ? pipes[i][0] : pipes[i][1], i);
			} else if (modes[i] == 2) {
				int nul = open("/dev/null", i == 0 ? O_RDONLY : O_WRONLY);
				if (nul >= 0) {
					dup2(nul, i);
					if (nul > 2) close(nul);
				}
			}
		}
		if (cwd->len && chdir(cwd->data) != 0) {
			int e = errno | 0x10000;  // the directory's fault
			ssize_t w = write(report[1], &e, sizeof e);
			(void)w;
			_exit(127);
		}
		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);
		signal(SIGPIPE, SIG_DFL);
		execve(exe, av, envp.data);
		int e = errno;
		ssize_t w = write(report[1], &e, sizeof e);
		(void)w;
		_exit(127);
	}
	close(report[1]);
	report[1] = -1;
	int e = 0;
	ssize_t got;
	do {
		got = read(report[0], &e, sizeof e);
	} while (got < 0 && errno == EINTR);
	if (got == sizeof e) {
		int st;
		waitpid(pid, &st, 0);
		if (e & 0x10000) snprintf(failure, sizeof failure, "%s: no directory %s (%s)", prog->data, cwd->data, strerror(e & 0xffff));
		else snprintf(failure, sizeof failure, "%s: %s", prog->data, strerror(e));
		rc = -(e & 0xffff);
		goto done;
	}
	p->pid = pid;
	p->in = pipes[0][1];
	p->out = pipes[1][0];
	p->err = pipes[2][0];
	pipes[0][1] = pipes[1][0] = pipes[2][0] = -1;
done:
	for (int i = 0; i < 3; i++)
		for (int k = 0; k < 2; k++)
			if (pipes[i][k] >= 0) close(pipes[i][k]);
	if (report[0] >= 0) close(report[0]);
	if (report[1] >= 0) close(report[1]);
	for (uint32_t i = 0; i + 1 < envp.len; i++) ak_free(envp.data[i]);
	ak_vec_free(&envp);
	ak_free(av);
	ak_free(exe);
	return rc;
}
#endif

static ak_val spawn_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	int modes[3] = {(int)args[5].i, (int)args[6].i, (int)args[7].i};
	proc *p = ak_alloc(sizeof *p);
	memset(p, 0, sizeof *p);
	p->in = p->out = p->err = NO_FD;
	failure[0] = 0;
	int64_t rc = spawn(p, STR(0), arr_arg(args[1]), STR(2), arr_arg(args[3]), args[4].i != 0, modes);
	if (rc < 0) {
		ak_free(p);
		return (ak_val){.i = rc == -ENOENT ? -2 : -3};  // not found, or it can't start
	}
	p->handle = next_handle++;
	ak_vec_push(&procs, p);
	return (ak_val){.i = p->handle};
}

static ak_val pid_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	proc *p = find(args[0].i);
	return (ak_val){.i = p ? p->pid : 0};
}

// ---- Jobs: what blocks, on threads of their own ----

typedef enum { CJ_READ, CJ_READALL, CJ_WRITE, CJ_WAIT } job_kind;

typedef struct {
	job_kind kind;
	int64_t handle;
	int fd;  // a read's: 1, 2
	fd_t f;
	int64_t pid;
#if defined(_WIN32)
	HANDLE process;
#endif
	ak_future *fut;
	uint8_t *data;  // a write's bytes, a read's
	size_t len;
	bool ok;
	int64_t code;
} job;

static void job_done(void *ctx) {
	job *j = ctx;
	ak_sched_unhold();
	proc *p = find(j->handle);
	if (p) p->jobs--;
	switch (j->kind) {
	case CJ_READ:
		if (p) p->reading[j->fd] = false;
		if (j->fut->state == AK_FUT_PENDING) {
			ak_future_resolve(j->fut, (ak_val){.p = j->ok && j->len > 0 ? ak_bytes_new(j->data, j->len) : NULL});
		}
		break;
	case CJ_READALL:
		if (p) p->reading[j->fd] = false;
		if (j->fut->state == AK_FUT_PENDING) ak_future_resolve(j->fut, (ak_val){.p = ak_bytes_new(j->data, j->len)});
		break;
	case CJ_WRITE:
		if (p) p->writing = false;
		if (j->fut->state == AK_FUT_PENDING) ak_future_resolve(j->fut, (ak_val){.i = j->ok});
		break;
	case CJ_WAIT:
		if (p) {
			p->waiting = false;
			p->waited = true;
			p->code = j->code;
		}
		if (j->fut->state == AK_FUT_PENDING) ak_future_resolve(j->fut, (ak_val){.i = j->code});
		break;
	}
	j->fut->cancel = NULL;
	j->fut->ctx = NULL;
	ak_release(j->fut);
	ak_free(j->data);
	ak_free(j);
	if (p) forget(p);
}

static void run_job(void *ctx) {
	job *j = ctx;
	switch (j->kind) {
	case CJ_READ: {
		j->data = ak_alloc(65536);
#if defined(_WIN32)
		DWORD got = 0;
		BOOL ok = ReadFile(j->f, j->data, 65536, &got, NULL);
		j->ok = ok && got > 0;
		j->len = got;
#else
		ssize_t got;
		do {
			got = read(j->f, j->data, 65536);
		} while (got < 0 && errno == EINTR);
		j->ok = got > 0;
		j->len = got > 0 ? (size_t)got : 0;
#endif
		break;
	}
	case CJ_READALL: {
		size_t cap = 65536;
		j->data = ak_alloc(cap);
		for (;;) {
			if (cap - j->len < 65536) {
				cap *= 2;
				j->data = ak_realloc(j->data, cap);
			}
#if defined(_WIN32)
			DWORD got = 0;
			if (!ReadFile(j->f, j->data + j->len, (DWORD)(cap - j->len), &got, NULL) || got == 0) break;
#else
			ssize_t got = read(j->f, j->data + j->len, cap - j->len);
			if (got < 0 && errno == EINTR) continue;
			if (got <= 0) break;
#endif
			j->len += (size_t)got;
		}
		break;
	}
	case CJ_WRITE: {
		size_t off = 0;
		j->ok = true;
		while (off < j->len) {
#if defined(_WIN32)
			DWORD w = 0;
			if (!WriteFile(j->f, j->data + off, (DWORD)(j->len - off), &w, NULL)) {
				j->ok = false;
				break;
			}
#else
			ssize_t w = write(j->f, j->data + off, j->len - off);
			if (w < 0 && errno == EINTR) continue;
			if (w <= 0) {
				j->ok = false;
				break;
			}
#endif
			off += (size_t)w;
		}
		break;
	}
	case CJ_WAIT: {
#if defined(_WIN32)
		WaitForSingleObject(j->process, INFINITE);
		DWORD code = 0;
		GetExitCodeProcess(j->process, &code);
		j->code = code;
#else
		int st = 0;
		pid_t r;
		do {
			r = waitpid((pid_t)j->pid, &st, 0);
		} while (r < 0 && errno == EINTR);
		if (r < 0) j->code = -1;
		else if (WIFEXITED(st)) j->code = WEXITSTATUS(st);
		else if (WIFSIGNALED(st)) j->code = 128 + WTERMSIG(st);
		else j->code = -1;
#endif
		break;
	}
	}
	ak_post(job_done, j);
}

static void kill_proc(proc *p, int sig) {
	if (p->waited) return;
#if defined(_WIN32)
	AK_UNUSED(sig);
	TerminateProcess(p->process, 1);
#else
	kill((pid_t)p->pid, sig);
#endif
}

// A cancelled wait kills the child; a cancelled read or write leaves its
// thread to finish (it posts to a future no one waits on).
static void wait_cancel(ak_future *f) {
	job *j = f->ctx;
	if (!j) return;
	proc *p = find(j->handle);
	if (p) kill_proc(p, 9);
}

static ak_val start_job(job *j) {
	j->fut = ak_future_new(j->kind == CJ_READ || j->kind == CJ_READALL ? AK_K_REF : AK_K_INT);
	ak_val out = {.p = j->fut};
	ak_retain(j->fut);
	if (j->kind == CJ_WAIT) {
		j->fut->ctx = j;
		j->fut->cancel = wait_cancel;
	}
	proc *p = find(j->handle);
	if (p) p->jobs++;
	ak_sched_hold();
	if (!ak_thread_start(run_job, j)) run_job(j);
	return out;
}

static ak_val failed(const char *msg) {
	ak_future *f = ak_future_new(AK_K_REF);
	ak_throw_msg("%s", msg);
	ak_future_reject(f, ak_error_take());
	return (ak_val){.p = f};
}

static ak_val read_any(ak_val *args, job_kind kind) {
	proc *p = find(args[0].i);
	int fd = (int)args[1].i;
	if (!p || (fd != 1 && fd != 2)) return failed("command: no such process");
	fd_t f = fd == 1 ? p->out : p->err;
	if (f == NO_FD) {
		ak_future *done = ak_future_new(AK_K_REF);
		ak_future_resolve(done, (ak_val){.p = kind == CJ_READALL ? ak_bytes_new(NULL, 0) : NULL});
		return (ak_val){.p = done};
	}
	if (p->reading[fd]) return failed("command: a stream has one reader");
	p->reading[fd] = true;
	job *j = ak_alloc(sizeof *j);
	memset(j, 0, sizeof *j);
	j->kind = kind, j->handle = p->handle, j->fd = fd, j->f = f;
	return start_job(j);
}

static ak_val read_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return read_any(args, CJ_READ);
}

static ak_val read_all_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	return read_any(args, CJ_READALL);
}

static ak_val write_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	proc *p = find(args[0].i);
	const ak_bytes *b = args[1].p;
	if (!p) return failed("command: no such process");
	if (p->in == NO_FD || p->writing) {
		ak_future *f = ak_future_new(AK_K_INT);
		ak_future_resolve(f, (ak_val){.i = 0});
		return (ak_val){.p = f};
	}
	p->writing = true;
	job *j = ak_alloc(sizeof *j);
	memset(j, 0, sizeof *j);
	j->kind = CJ_WRITE, j->handle = p->handle, j->f = p->in;
	j->data = ak_alloc(b->len ? b->len : 1);
	memcpy(j->data, b->data, b->len);
	j->len = b->len;
	return start_job(j);
}

static ak_val close_stdin_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	proc *p = find(args[0].i);
	if (p && !p->writing) close_fd(&p->in);
	return (ak_val){0};
}

static ak_val wait_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	proc *p = find(args[0].i);
	if (!p) return failed("command: no such process");
	if (p->waited) {
		ak_future *f = ak_future_new(AK_K_INT);
		ak_future_resolve(f, (ak_val){.i = p->code});
		return (ak_val){.p = f};
	}
	if (p->waiting) return failed("command: a process is waited for once at a time");
	p->waiting = true;
	// Its stdin is the child's to see end: a wait without writing more.
	if (!p->writing) close_fd(&p->in);
	job *j = ak_alloc(sizeof *j);
	memset(j, 0, sizeof *j);
	j->kind = CJ_WAIT, j->handle = p->handle, j->pid = p->pid;
#if defined(_WIN32)
	j->process = p->process;
#endif
	return start_job(j);
}

static ak_val kill_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	proc *p = find(args[0].i);
	if (p) kill_proc(p, (int)args[1].i);
	return (ak_val){0};
}

static ak_val release_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	proc *p = find(args[0].i);
	if (!p || p->released) return (ak_val){0};
	p->released = true;
	if (!p->writing) close_fd(&p->in);
	if (!p->reading[1]) close_fd(&p->out);
	if (!p->reading[2]) close_fd(&p->err);
	forget(p);
	return (ak_val){0};
}

// ---- The app's own programs ----

// A desktop app's [bin] programs are in it (docs/programs.md): the Mac's
// Contents/Helpers, beside the .exe on Windows, in a Linux prefix's bin
// beside the app's; asuka dev builds them into a directory of its own,
// $ASUKA_BIN_DIR.

#if defined(_WIN32)

// bundled_at says whether dir\name.exe is a file, and puts its path in
// out (UTF-8).
static bool bundled_at(const wchar_t *dir, const ak_str *name, char *out, size_t cap) {
	if (!dir || !*dir) return false;
	wchar_t *wn = wide(name->data, name->len);
	wchar_t path[32768];
	size_t dl = wcslen(dir);
	bool sep = dl && (dir[dl - 1] == L'\\' || dir[dl - 1] == L'/');
	int k = _snwprintf(path, 32768, L"%ls%ls%ls.exe", dir, sep ? L"" : L"\\", wn);
	ak_free(wn);
	if (k < 0 || k >= 32768) return false;
	path[k] = 0;
	DWORD a = GetFileAttributesW(path);
	if (a == INVALID_FILE_ATTRIBUTES || (a & FILE_ATTRIBUTE_DIRECTORY)) return false;
	return WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)cap, NULL, NULL) > 0;
}

static ak_val bundled_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *name = STR(0);
	if (!name->len || strpbrk(name->data, "/\\:")) return (ak_val){.p = NULL};
	static char out[32768 * 3];
	bool found = false;
	static wchar_t buf[32768];
	DWORD k = GetEnvironmentVariableW(L"ASUKA_BIN_DIR", buf, 32768);
	if (k > 0 && k < 32768) found = bundled_at(buf, name, out, sizeof out);
	if (!found) {
		DWORD m = GetModuleFileNameW(NULL, buf, 32768);
		if (m > 0 && m < 32768) {
			wchar_t *slash = wcsrchr(buf, L'\\');
			if (slash) *slash = 0;
			found = bundled_at(buf, name, out, sizeof out);
		}
	}
	return (ak_val){.p = found ? ak_str_c(out) : NULL};
}

#else

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif
#include <limits.h>

// exe_dir puts the directory of the process's executable, links
// resolved, in out: false when it can't be known.
static bool exe_dir(char *out) {
	char buf[PATH_MAX];
#if defined(__APPLE__)
	uint32_t size = sizeof buf;
	if (_NSGetExecutablePath(buf, &size) != 0) return false;
#else
	ssize_t k = readlink("/proc/self/exe", buf, sizeof buf - 1);
	if (k <= 0) return false;
	buf[k] = 0;
#endif
	if (!realpath(buf, out)) snprintf(out, PATH_MAX, "%s", buf);
	char *slash = strrchr(out, '/');
	if (!slash) return false;
	*slash = 0;
	return true;
}

// bundled_at says whether dir/name is an executable file, and puts its
// path, links resolved, in out.
static bool bundled_at(const char *dir, const char *name, char *out) {
	if (!dir || !*dir) return false;
	char path[PATH_MAX];
	int k = snprintf(path, sizeof path, "%s/%s", dir, name);
	if (k < 0 || (size_t)k >= sizeof path) return false;
	struct stat st;
	if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || access(path, X_OK) != 0) return false;
	if (!realpath(path, out)) snprintf(out, PATH_MAX, "%s", path);
	return true;
}

static ak_val bundled_fn(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	const ak_str *name = STR(0);
	if (!name->len || strchr(name->data, '/') || strcmp(name->data, ".") == 0 || strcmp(name->data, "..") == 0)
		return (ak_val){.p = NULL};
	char out[PATH_MAX], dir[PATH_MAX];
	bool found = bundled_at(getenv("ASUKA_BIN_DIR"), name->data, out);
	if (!found && exe_dir(dir)) {
		static const char macos[] = "/Contents/MacOS";
		size_t len = strlen(dir), ml = sizeof macos - 1;
		if (len >= ml && strcmp(dir + len - ml, macos) == 0) {
			char helpers[PATH_MAX];
			snprintf(helpers, sizeof helpers, "%.*s/Contents/Helpers", (int)(len - ml), dir);
			found = bundled_at(helpers, name->data, out);
		}
		if (!found) found = bundled_at(dir, name->data, out);
	}
	return (ak_val){.p = found ? ak_str_c(out) : NULL};
}

#endif

const ak_lib_entry ak_std_command[] = {
	{"command.procSpawn", spawn_fn},
	{"command.procFailure", failure_fn},
	{"command.procPid", pid_fn},
	{"command.procRead", read_fn},
	{"command.procReadAll", read_all_fn},
	{"command.procWrite", write_fn},
	{"command.procCloseStdin", close_stdin_fn},
	{"command.procWait", wait_fn},
	{"command.procKill", kill_fn},
	{"command.procRelease", release_fn},
	{"command.bundledPath", bundled_fn},
	{NULL, NULL},
};

#endif
