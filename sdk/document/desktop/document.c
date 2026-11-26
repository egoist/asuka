// platform/document on Windows and Linux: the files, read and written on a
// thread of the call's; a write is a temporary file renamed over the
// file, keeping its permissions.
#ifdef _WIN32
#include "asuka_windows.h"
#else
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include "document.asuka.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
	int op;  // 0 read, 1 write, 2 modified
	const char *path;
	asuka_bytes data;
	asuka_call *call;
} job;

#ifdef _WIN32
static void fail_last(asuka_call *call, const char *what, const char *path) {
	asuka_error e = {0};
	char *w = asuka_sprintf("%s %s", what, path);
	asuka_win_fail(&e, HRESULT_FROM_WIN32(GetLastError()), w);
	free(w);
	asuka_call_fail(call, &e);
}

static void run(void *p) {
	job *j = p;
	wchar_t *path = asuka_wide(j->path);
	if (j->op == 0) {
		HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		LARGE_INTEGER size;
		if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &size)) {
			fail_last(j->call, "open", j->path);
		} else {
			asuka_bytes b = {malloc(size.QuadPart ? (size_t)size.QuadPart : 1), 0};
			DWORD got = 1;
			while (b.len < (size_t)size.QuadPart && got > 0) {
				DWORD want = (DWORD)((size_t)size.QuadPart - b.len > (1u << 30) ? (1u << 30) : (size_t)size.QuadPart - b.len);
				if (!ReadFile(f, b.data + b.len, want, &got, NULL)) break;
				b.len += got;
			}
			if (b.len < (size_t)size.QuadPart) {
				free(b.data);
				fail_last(j->call, "read", j->path);
			} else {
				document_Files_read_done(j->call, b);
			}
		}
		if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
	} else if (j->op == 1) {
		size_t n = wcslen(path);
		wchar_t *tmp = malloc((n + 32) * sizeof(wchar_t));
		swprintf(tmp, n + 32, L"%ls.%lu.tmp", path, (unsigned long)GetCurrentThreadId());
		HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD wrote = 0;
		BOOL ok = f != INVALID_HANDLE_VALUE;
		for (size_t off = 0; ok && off < j->data.len; off += wrote) {
			DWORD want = (DWORD)(j->data.len - off > (1u << 30) ? (1u << 30) : j->data.len - off);
			ok = WriteFile(f, j->data.data + off, want, &wrote, NULL);
		}
		DWORD code = GetLastError();
		if (f != INVALID_HANDLE_VALUE) ok = CloseHandle(f) && ok;
		if (ok) ok = MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
		if (!ok) {
			if (code == 0) code = GetLastError();
			DeleteFileW(tmp);
			SetLastError(code);
			fail_last(j->call, "write", j->path);
		} else {
			document_Files_write_done(j->call);
		}
		free(tmp);
	} else {
		WIN32_FILE_ATTRIBUTE_DATA a;
		if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) {
			DWORD code = GetLastError();
			if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) document_Files_modified_done(j->call, 0);
			else fail_last(j->call, "stat", j->path);
		} else {
			// 100 ns since 1601 to milliseconds since 1970.
			uint64_t t = (uint64_t)a.ftLastWriteTime.dwHighDateTime << 32 | a.ftLastWriteTime.dwLowDateTime;
			document_Files_modified_done(j->call, (int64_t)(t / 10000 - 11644473600000ULL));
		}
	}
	free(path);
	free(j);
}
#else
static void fail_errno(asuka_call *call, int code, const char *what, const char *path) {
	asuka_error e = {0};
	asuka_error_failed(&e, "errno", code, "%s %s: %s", what, path, strerror(code));
	asuka_call_fail(call, &e);
}

static void run(void *p) {
	job *j = p;
	if (j->op == 0) {
		int fd = open(j->path, O_RDONLY | O_CLOEXEC);
		struct stat st;
		if (fd < 0 || fstat(fd, &st) != 0) {
			fail_errno(j->call, errno, "open", j->path);
		} else {
			size_t cap = st.st_size > 0 ? (size_t)st.st_size : 4096;
			asuka_bytes b = {malloc(cap), 0};
			for (;;) {
				if (b.len == cap) b.data = realloc(b.data, cap *= 2);
				ssize_t n = read(fd, b.data + b.len, cap - b.len);
				if (n < 0 && errno == EINTR) continue;
				if (n <= 0) {
					if (n < 0) {
						int code = errno;
						free(b.data);
						b.data = NULL;
						fail_errno(j->call, code, "read", j->path);
					}
					break;
				}
				b.len += (size_t)n;
			}
			if (b.data) document_Files_read_done(j->call, b);
		}
		if (fd >= 0) close(fd);
	} else if (j->op == 1) {
		mode_t mode = 0644;
		struct stat st;
		if (stat(j->path, &st) == 0) mode = st.st_mode & 07777;
		// The temporary file beside it, hidden: .name.XXXXXX.
		const char *slash = strrchr(j->path, '/');
		char *tmp;
		if (slash) tmp = asuka_sprintf("%.*s/.%s.XXXXXX", (int)(slash - j->path), j->path, slash + 1);
		else tmp = asuka_sprintf(".%s.XXXXXX", j->path);
		int fd = mkstemp(tmp);
		int code = 0;
		if (fd < 0) code = errno;
		for (size_t off = 0; !code && off < j->data.len;) {
			ssize_t n = write(fd, j->data.data + off, j->data.len - off);
			if (n < 0 && errno == EINTR) continue;
			if (n < 0) code = errno;
			else off += (size_t)n;
		}
		if (fd >= 0 && close(fd) != 0 && !code) code = errno;
		if (!code && fchmodat(AT_FDCWD, tmp, mode, 0) != 0) code = errno;
		if (!code && rename(tmp, j->path) != 0) code = errno;
		if (code) {
			if (fd >= 0) unlink(tmp);
			fail_errno(j->call, code, "write", j->path);
		} else {
			document_Files_write_done(j->call);
		}
		free(tmp);
	} else {
		struct stat st;
		if (stat(j->path, &st) != 0) {
			if (errno == ENOENT || errno == ENOTDIR) document_Files_modified_done(j->call, 0);
			else fail_errno(j->call, errno, "stat", j->path);
		} else {
			document_Files_modified_done(j->call, (int64_t)st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000);
		}
	}
	free(j);
}
#endif

static void start(int op, const char *path, asuka_bytes data, asuka_call *call) {
	job *j = malloc(sizeof *j);
	*j = (job){op, path, data, call};
	if (!asuka_desktop_spawn(run, j)) {
		free(j);
		asuka_error e = {0};
		asuka_error_failed(&e, "asuka", 0, "no thread for the file");
		asuka_call_fail(call, &e);
	}
}

static void d_read(document_Files *self, const char *path, asuka_call *call) { start(0, path, (asuka_bytes){0}, call); }
static void d_write(document_Files *self, const char *path, asuka_bytes data, asuka_call *call) { start(1, path, data, call); }
static void d_modified(document_Files *self, const char *path, asuka_call *call) { start(2, path, (asuka_bytes){0}, call); }

// A locked file is one that can't be written: Windows' read-only
// attribute, Unix's write bits.
#ifdef _WIN32
static bool d_locked(document_Files *self, const char *path, asuka_error *err) {
	wchar_t *w = asuka_wide(path);
	DWORD a = GetFileAttributesW(w);
	free(w);
	return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY);
}

static void d_set_locked(document_Files *self, const char *path, bool on, asuka_error *err) {
	wchar_t *w = asuka_wide(path);
	DWORD a = GetFileAttributesW(w);
	if (a == INVALID_FILE_ATTRIBUTES || !SetFileAttributesW(w, on ? a | FILE_ATTRIBUTE_READONLY : a & ~(DWORD)FILE_ATTRIBUTE_READONLY))
		asuka_win_fail(err, HRESULT_FROM_WIN32(GetLastError()), "document: can't lock or unlock the file");
	free(w);
}
#else
static bool d_locked(document_Files *self, const char *path, asuka_error *err) {
	struct stat st;
	return stat(path, &st) == 0 && !(st.st_mode & S_IWUSR);
}

static void d_set_locked(document_Files *self, const char *path, bool on, asuka_error *err) {
	struct stat st;
	// Locked: no one writes it; unlocked: its owner does again.
	if (stat(path, &st) != 0 || chmod(path, on ? st.st_mode & ~(mode_t)(S_IWUSR | S_IWGRP | S_IWOTH) : st.st_mode | S_IWUSR) != 0)
		asuka_error_failed(err, "errno", errno, "document: can't lock or unlock %s: %s", path, strerror(errno));
}
#endif

static const document_Files_vtable vt = {.read = d_read, .write = d_write, .modified = d_modified, .locked = d_locked, .setLocked = d_set_locked};

document_Files *document_make_Files(asuka_error *err) {
	document_Files *f = malloc(sizeof *f);
	f->vt = &vt;
	return f;
}
