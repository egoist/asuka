// platform/record on Windows: the default microphone (waveIn) encoded to
// AAC by Media Foundation's sink writer as it records, in an MPEG-4 file
// (.m4a), on a thread of its own in COM's multithreaded apartment; the
// main thread asks it to pause, resume and stop, and reads its meter.
// Windows's AAC encoder takes 44.1 kHz: voice is mono at 96 kbit/s, music
// stereo at 128. A Windows app keeps recording out of sight: background
// and title change nothing. Settings' microphone privacy switch refused:
// not allowed. macOS runs the package's Swift; video is platform/camera's.
#include "asuka_windows.h"
#include <mmsystem.h>
#include "record.asuka.h"

#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { RATE = 44100, BUFFERS = 8 };

typedef struct {
	asuka_call *call;  // record's, until it answers (main thread)
	wchar_t *path;
	int channels, bps;
	uint32_t max_ms;
	atomic_bool stop, paused;    // asked
	atomic_bool recording;       // not paused, not stopped
	atomic_uint level, ms;
	asuka_error err;  // why it failed (the thread's, read once it's done)
} recording;

static recording *now;  // main thread

// ---- Settings' privacy switches ----

static bool reg_deny(HKEY root, const wchar_t *path) {
	wchar_t v[16];
	DWORD n = sizeof v;
	return RegGetValueW(root, path, L"Value", RRF_RT_REG_SZ, NULL, v, &n) == ERROR_SUCCESS && !wcscmp(v, L"Deny");
}

// denied reports whether Settings (Privacy & security) keeps desktop
// apps from the microphone.
static bool denied(void) {
	const wchar_t *base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
	const wchar_t *np = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone\\NonPackaged";
	HKEY roots[2] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
	for (int i = 0; i < 2; i++)
		if (reg_deny(roots[i], base) || reg_deny(roots[i], np)) return true;
	return false;
}

// ---- Media Foundation: a sink writer, PCM in and AAC out ----

static const GUID MF_MT_MAJOR_TYPE = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
static const GUID MF_MT_SUBTYPE = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID MF_MT_AUDIO_NUM_CHANNELS = {0x37e48bf5, 0x645e, 0x4c5b, {0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a}};
static const GUID MF_MT_AUDIO_SAMPLES_PER_SECOND = {0x5faeeae7, 0x0290, 0x4c31, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
static const GUID MF_MT_AUDIO_BITS_PER_SAMPLE = {0xf2deb57f, 0x40fa, 0x4764, {0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69}};
static const GUID MF_MT_AUDIO_AVG_BYTES_PER_SECOND = {0x1aab75c8, 0xcfef, 0x451c, {0xab, 0x95, 0xac, 0x03, 0x4b, 0x8e, 0x17, 0x31}};
static const GUID MF_MT_AUDIO_BLOCK_ALIGNMENT = {0x322de230, 0x9eeb, 0x43bd, {0xab, 0x7a, 0xff, 0x41, 0x22, 0x51, 0x54, 0x1d}};
static const GUID MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION = {0x7632f0e6, 0x9538, 0x4d61, {0xac, 0xda, 0xea, 0x29, 0xc8, 0xc1, 0x44, 0x56}};
static const GUID MF_MT_ALL_SAMPLES_INDEPENDENT = {0xc9173739, 0x5e56, 0x461c, {0xb7, 0x13, 0x46, 0xfb, 0x99, 0x5c, 0xb9, 0x5f}};
static const GUID MFMediaType_Audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID MFAudioFormat_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID MFAudioFormat_AAC = {0x00001610, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID MF_TRANSCODE_CONTAINERTYPE = {0x150ff23f, 0x4abc, 0x478b, {0xac, 0x4f, 0xe1, 0x91, 0x6f, 0xba, 0x1c, 0xca}};
static const GUID MFTranscodeContainerType_MPEG4 = {0xdc6cd05d, 0xb9d0, 0x40ef, {0xbd, 0x35, 0xfa, 0x62, 0x2c, 0x1a, 0xb2, 0x8a}};

// IMFAttributes' (IMFMediaType's, IMFSample's) slots, IMFSample's,
// IMFMediaBuffer's and IMFSinkWriter's.
enum {
	attr_set_u32 = 21,
	attr_set_guid = 24,
	sample_set_time = 36,
	sample_set_duration = 38,
	sample_add_buffer = 42,
	buf_lock = 3,
	buf_unlock = 4,
	buf_set_length = 6,
	writer_add_stream = 3,
	writer_set_input = 4,
	writer_begin = 5,
	sw_write = 6,
	writer_finalize = 11,
};

typedef HRESULT(STDMETHODCALLTYPE *set_u32)(void *, const GUID *, UINT32);
typedef HRESULT(STDMETHODCALLTYPE *set_guid)(void *, const GUID *, const GUID *);

static struct {
	HRESULT(WINAPI *startup)(ULONG, DWORD);
	HRESULT(WINAPI *media_type)(void **);
	HRESULT(WINAPI *attributes)(void **, UINT32);
	HRESULT(WINAPI *memory_buffer)(DWORD, void **);
	HRESULT(WINAPI *sample)(void **);
	HRESULT(WINAPI *sink_writer)(LPCWSTR, void *, void *, void **);
	bool started;
	HRESULT hr;
} mf;

static INIT_ONCE mf_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK mf_load(PINIT_ONCE once, PVOID p, PVOID *ctx) {
	HMODULE plat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	HMODULE rw = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (plat) {
		*(FARPROC *)&mf.startup = GetProcAddress(plat, "MFStartup");
		*(FARPROC *)&mf.media_type = GetProcAddress(plat, "MFCreateMediaType");
		*(FARPROC *)&mf.attributes = GetProcAddress(plat, "MFCreateAttributes");
		*(FARPROC *)&mf.memory_buffer = GetProcAddress(plat, "MFCreateMemoryBuffer");
		*(FARPROC *)&mf.sample = GetProcAddress(plat, "MFCreateSample");
	}
	if (rw) *(FARPROC *)&mf.sink_writer = GetProcAddress(rw, "MFCreateSinkWriterFromURL");
	if (mf.startup && mf.media_type && mf.attributes && mf.memory_buffer && mf.sample && mf.sink_writer) {
		mf.hr = mf.startup(0x00020070, 0);  // MF_VERSION, MFSTARTUP_FULL
		mf.started = SUCCEEDED(mf.hr);
	}
	return TRUE;
}

static bool mf_start(asuka_error *err) {
	InitOnceExecuteOnce(&mf_once, mf_load, NULL, NULL);
	if (mf.started) return true;
	if (!mf.startup || !mf.sink_writer)
		asuka_error_unsupported(err, "Media Foundation isn't on this Windows (an N edition without its Media Feature Pack)");
	else
		asuka_win_fail(err, mf.hr, "Media Foundation");
	return false;
}

// audio_type is 16-bit PCM (aac false) or AAC-LC of channels at RATE.
static void *audio_type(bool aac, int channels, int bps) {
	void *t = NULL;
	if (FAILED(mf.media_type(&t))) return NULL;
	ASUKA_SLOT(t, attr_set_guid, set_guid)(t, &MF_MT_MAJOR_TYPE, &MFMediaType_Audio);
	ASUKA_SLOT(t, attr_set_guid, set_guid)(t, &MF_MT_SUBTYPE, aac ? &MFAudioFormat_AAC : &MFAudioFormat_PCM);
	ASUKA_SLOT(t, attr_set_u32, set_u32)(t, &MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
	ASUKA_SLOT(t, attr_set_u32, set_u32)(t, &MF_MT_AUDIO_SAMPLES_PER_SECOND, RATE);
	ASUKA_SLOT(t, attr_set_u32, set_u32)(t, &MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	if (aac) {
		ASUKA_SLOT(t, attr_set_u32, set_u32)(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)bps);
		ASUKA_SLOT(t, attr_set_u32, set_u32)(t, &MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC profile, level 2
	} else {
		ASUKA_SLOT(t, attr_set_u32, set_u32)(t, &MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)(2 * channels));
		ASUKA_SLOT(t, attr_set_u32, set_u32)(t, &MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(2 * channels * RATE));
		ASUKA_SLOT(t, attr_set_u32, set_u32)(t, &MF_MT_ALL_SAMPLES_INDEPENDENT, 1);
	}
	return t;
}

typedef struct {
	void *w;  // IMFSinkWriter
	DWORD stream;
	int channels;
	int64_t written;  // frames
} writer;

static bool writer_open(writer *a, const wchar_t *path, int channels, int bps, asuka_error *err) {
	*a = (writer){.channels = channels};
	if (!mf_start(err)) return false;
	void *attrs = NULL;
	if (FAILED(mf.attributes(&attrs, 4))) {
		asuka_error_failed(err, "record", 0, "MFCreateAttributes failed");
		return false;
	}
	ASUKA_SLOT(attrs, attr_set_guid, set_guid)(attrs, &MF_TRANSCODE_CONTAINERTYPE, &MFTranscodeContainerType_MPEG4);
	HRESULT hr = mf.sink_writer(path, NULL, attrs, &a->w);
	asuka_com_release(attrs);
	if (FAILED(hr)) {
		asuka_win_fail(err, hr, "couldn't write the recording's file");
		a->w = NULL;
		return false;
	}
	void *out = audio_type(true, channels, bps), *in = audio_type(false, channels, bps);
	const char *what = NULL;
	if (!out || !in) what = "MFCreateMediaType";
	else if (FAILED(hr = ASUKA_SLOT(a->w, writer_add_stream, HRESULT(STDMETHODCALLTYPE *)(void *, void *, DWORD *))(a->w, out, &a->stream)))
		what = "no AAC encoder";
	else if (FAILED(hr = ASUKA_SLOT(a->w, writer_set_input, HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, void *, void *))(a->w, a->stream, in, NULL)))
		what = "the AAC encoder refused the sound";
	else if (FAILED(hr = ASUKA_SLOT(a->w, writer_begin, HRESULT(STDMETHODCALLTYPE *)(void *))(a->w)))
		what = "couldn't start writing";
	asuka_com_release(out);
	asuka_com_release(in);
	if (what) {
		asuka_win_fail(err, hr, what);
		asuka_com_release(a->w);
		a->w = NULL;
		return false;
	}
	return true;
}

// writer_write adds PCM (whole frames), which follows what was written.
static bool writer_write(writer *a, const uint8_t *pcm, size_t n, asuka_error *err) {
	int64_t frames = (int64_t)(n / (2 * a->channels));
	if (!frames) return true;
	void *buf = NULL, *s = NULL;
	BYTE *p = NULL;
	if (FAILED(mf.memory_buffer((DWORD)n, &buf))) {
		asuka_error_failed(err, "record", 0, "MFCreateMemoryBuffer failed");
		return false;
	}
	ASUKA_SLOT(buf, buf_lock, HRESULT(STDMETHODCALLTYPE *)(void *, BYTE **, DWORD *, DWORD *))(buf, &p, NULL, NULL);
	memcpy(p, pcm, n);
	ASUKA_SLOT(buf, buf_unlock, HRESULT(STDMETHODCALLTYPE *)(void *))(buf);
	ASUKA_SLOT(buf, buf_set_length, HRESULT(STDMETHODCALLTYPE *)(void *, DWORD))(buf, (DWORD)n);
	if (FAILED(mf.sample(&s))) {
		asuka_com_release(buf);
		asuka_error_failed(err, "record", 0, "MFCreateSample failed");
		return false;
	}
	ASUKA_SLOT(s, sample_add_buffer, HRESULT(STDMETHODCALLTYPE *)(void *, void *))(s, buf);
	asuka_com_release(buf);
	ASUKA_SLOT(s, sample_set_time, HRESULT(STDMETHODCALLTYPE *)(void *, LONGLONG))(s, a->written * 10000000 / RATE);
	ASUKA_SLOT(s, sample_set_duration, HRESULT(STDMETHODCALLTYPE *)(void *, LONGLONG))(s, frames * 10000000 / RATE);
	a->written += frames;
	HRESULT hr = ASUKA_SLOT(a->w, sw_write, HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, void *))(a->w, a->stream, s);
	asuka_com_release(s);
	if (FAILED(hr)) {
		asuka_win_fail(err, hr, "couldn't encode");
		return false;
	}
	return true;
}

// writer_close finishes the file.
static bool writer_close(writer *a, asuka_error *err) {
	if (!a->w) return true;
	HRESULT hr = ASUKA_SLOT(a->w, writer_finalize, HRESULT(STDMETHODCALLTYPE *)(void *))(a->w);
	asuka_com_release(a->w);
	a->w = NULL;
	if (FAILED(hr)) {
		if (err) asuka_win_fail(err, hr, "couldn't finish the file");
		return false;
	}
	return true;
}

// ---- The microphone: waveIn, 16-bit PCM in buffers of 50 ms ----

typedef struct {
	HWAVEIN h;
	HANDLE event;
	WAVEHDR hdrs[BUFFERS];
	uint8_t *bufs[BUFFERS];
	int next;  // the buffer that comes back next
} wave_in;

static bool wave_open(wave_in *w, int channels, asuka_error *err) {
	*w = (wave_in){0};
	if (waveInGetNumDevs() == 0) {
		asuka_error_failed(err, "record", 0, "no microphone");
		return false;
	}
	w->event = CreateEventW(NULL, FALSE, FALSE, NULL);
	WAVEFORMATEX f = {.wFormatTag = WAVE_FORMAT_PCM,
	                  .nChannels = (WORD)channels,
	                  .nSamplesPerSec = RATE,
	                  .nAvgBytesPerSec = (DWORD)(RATE * channels * 2),
	                  .nBlockAlign = (WORD)(channels * 2),
	                  .wBitsPerSample = 16};
	MMRESULT r = waveInOpen(&w->h, WAVE_MAPPER, &f, (DWORD_PTR)w->event, 0, CALLBACK_EVENT);
	if (r != MMSYSERR_NOERROR) {
		CloseHandle(w->event);
		asuka_error_failed(err, "mmsystem", r, "the microphone: waveIn error %u", (unsigned)r);
		return false;
	}
	DWORD size = RATE / 20 * channels * 2;
	for (int i = 0; i < BUFFERS; i++) {
		w->bufs[i] = malloc(size);
		w->hdrs[i] = (WAVEHDR){.lpData = (LPSTR)w->bufs[i], .dwBufferLength = size};
		waveInPrepareHeader(w->h, &w->hdrs[i], sizeof(WAVEHDR));
		waveInAddBuffer(w->h, &w->hdrs[i], sizeof(WAVEHDR));
	}
	return true;
}

// wave_read calls fn with each buffer recorded since, in order, and gives
// the buffers back to the device.
static void wave_read(wave_in *w, void (*fn)(void *ctx, const uint8_t *pcm, size_t n), void *ctx) {
	for (int i = 0; i < BUFFERS; i++) {
		WAVEHDR *hd = &w->hdrs[w->next];
		if (!(hd->dwFlags & WHDR_DONE)) return;
		if (hd->dwBytesRecorded > 0) fn(ctx, w->bufs[w->next], hd->dwBytesRecorded);
		hd->dwFlags &= ~WHDR_DONE;
		hd->dwBytesRecorded = 0;
		waveInAddBuffer(w->h, hd, sizeof(WAVEHDR));
		w->next = (w->next + 1) % BUFFERS;
	}
}

// wave_close stops recording and lets the device go: what was recorded
// and not read yet goes to fn first.
static void wave_close(wave_in *w, void (*fn)(void *ctx, const uint8_t *pcm, size_t n), void *ctx) {
	waveInStop(w->h);
	wave_read(w, fn, ctx);
	waveInReset(w->h);
	for (int i = 0; i < BUFFERS; i++) {
		waveInUnprepareHeader(w->h, &w->hdrs[i], sizeof(WAVEHDR));
		free(w->bufs[i]);
	}
	waveInClose(w->h);
	CloseHandle(w->event);
}

// ---- Recording, on its thread ----

// pcm_level is 16-bit PCM's level, 0 to 255: its RMS amplitude, silence
// below -60 dB (as AVAudioRecorder's average power reads).
static unsigned pcm_level(const uint8_t *pcm, size_t len) {
	size_t n = len / 2;
	if (!n) return 0;
	double sum = 0;
	for (size_t i = 0; i < n; i++) {
		double v = (int16_t)(pcm[2 * i] | pcm[2 * i + 1] << 8) / 32768.0;
		sum += v * v;
	}
	double rms = sqrt(sum / n);
	if (rms < 0.001) return 0;
	return (unsigned)((rms > 1 ? 1 : rms) * 255);
}

typedef struct {
	recording *r;
	writer w;
	int64_t kept, limit;  // bytes; limit 0: none
	int frame;
	bool failed;
} session;

static void put(void *ctx, const uint8_t *pcm, size_t n) {
	session *s = ctx;
	if (s->failed || (s->limit > 0 && s->kept >= s->limit)) return;
	if (s->limit > 0 && s->kept + (int64_t)n > s->limit) n = (size_t)(s->limit - s->kept);
	if (!writer_write(&s->w, pcm, n, &s->r->err)) s->failed = true;
	s->kept += (int64_t)n;
	atomic_store(&s->r->ms, (unsigned)(s->kept / s->frame * 1000 / RATE));
	atomic_store(&s->r->level, pcm_level(pcm, n));
}

static void finished(void *p);

static void run(void *p) {
	recording *r = p;
	HRESULT co = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	session s = {.r = r, .frame = 2 * r->channels, .limit = (int64_t)r->max_ms * RATE / 1000 * (2 * r->channels)};
	wave_in in;
	if (!wave_open(&in, r->channels, &r->err)) goto done;
	if (!writer_open(&s.w, r->path, r->channels, r->bps, &r->err)) {
		wave_close(&in, put, &s);
		goto done;
	}
	MMRESULT m = waveInStart(in.h);
	if (m != MMSYSERR_NOERROR) {
		asuka_error_failed(&r->err, "mmsystem", m, "the microphone: waveIn error %u", (unsigned)m);
		s.failed = true;
		wave_close(&in, put, &s);
		writer_close(&s.w, NULL);
		goto done;
	}
	atomic_store(&r->recording, true);
	while (!s.failed && (s.limit == 0 || s.kept < s.limit) && !atomic_load(&r->stop)) {
		bool pause = atomic_load(&r->paused);
		if (pause && atomic_load(&r->recording)) {
			waveInStop(in.h);
			atomic_store(&r->recording, false);
		} else if (!pause && !atomic_load(&r->recording)) {
			waveInStart(in.h);
			atomic_store(&r->recording, true);
		}
		WaitForSingleObject(in.event, 50);
		wave_read(&in, put, &s);
	}
	atomic_store(&r->recording, false);
	wave_close(&in, put, &s);
	if (!writer_close(&s.w, s.failed ? NULL : &r->err)) s.failed = true;
done:
	if (SUCCEEDED(co)) CoUninitialize();
	if (r->err.status) DeleteFileW(r->path);
	asuka_desktop_post(finished, r);
}

// finished answers record, on the main thread, and lets it go.
static void finished(void *p) {
	recording *r = p;
	if (now == r) now = NULL;
	if (r->call) {
		if (r->err.status) {
			asuka_call_fail(r->call, &r->err);
		} else {
			record_Recorder_record_done(r->call, asuka_narrow(r->path, -1));
		}
	}
	asuka_error_free(&r->err);
	free(r->path);
	free(r);
}

// The task that waits went: the recording ends (its file kept), and
// record answers cancelled.
static void on_cancel(void *p) {
	recording *r = p;
	asuka_error e = {0};
	asuka_error_cancelled(&e);
	asuka_call_fail(r->call, &e);
	r->call = NULL;
	atomic_store(&r->stop, true);
}

// new_file is a new file in the app's caches' asuka-audio.
static wchar_t *new_file(asuka_error *err) {
	char *dir = asuka_sprintf("%s\\asuka-audio", asuka_desktop_cache_dir());
	wchar_t *wdir = asuka_wide(dir);
	free(dir);
	for (wchar_t *q = wdir + 3; *q; q++) {  // past "C:\"
		if (*q != L'\\' && *q != L'/') continue;
		wchar_t c = *q;
		*q = 0;
		CreateDirectoryW(wdir, NULL);
		*q = c;
	}
	CreateDirectoryW(wdir, NULL);
	static unsigned serial;
	for (int i = 0; i < 100; i++) {
		size_t n = wcslen(wdir) + 40;
		wchar_t *path = malloc(n * sizeof *path);
		_snwprintf(path, n, L"%ls\\%08lx%04x.m4a", wdir, (unsigned long)(GetTickCount() ^ GetCurrentProcessId() << 16), serial++ & 0xffff);
		HANDLE h = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
		if (h != INVALID_HANDLE_VALUE) {
			CloseHandle(h);
			free(wdir);
			return path;
		}
		DWORD e = GetLastError();
		free(path);
		if (e != ERROR_FILE_EXISTS) {
			asuka_win_fail(err, HRESULT_FROM_WIN32(e), "record");
			free(wdir);
			return NULL;
		}
	}
	free(wdir);
	asuka_error_failed(err, "record", 0, "record: no new file");
	return NULL;
}

static void r_record(record_Recorder *self, record_Quality quality, int64_t max_duration, const char *path, bool background,
                     const char *title, asuka_call *call) {
	asuka_error e = {0};
	if (denied()) {
		asuka_error_declared(&e, record_RecordError_notAllowed);
		asuka_call_fail(call, &e);
		return;
	}
	if (now) {
		asuka_error_declared(&e, record_RecordError_busy);
		asuka_call_fail(call, &e);
		return;
	}
	wchar_t *file;
	if (path && *path) {
		file = asuka_wide(path);
	} else if (!(file = new_file(&e))) {
		asuka_call_fail(call, &e);
		return;
	}
	recording *r = calloc(1, sizeof *r);
	r->call = call;
	r->path = file;
	bool music = quality == record_Quality_music;
	r->channels = music ? 2 : 1;
	r->bps = music ? 16000 : 12000;
	r->max_ms = max_duration > 0 ? (uint32_t)(max_duration / 1000000) : 0;
	if (max_duration > 0 && !r->max_ms) r->max_ms = 1;
	if (!asuka_desktop_spawn(run, r)) {
		free(r->path);
		free(r);
		asuka_error_failed(&e, "record", 0, "record: no thread");
		asuka_call_fail(call, &e);
		return;
	}
	now = r;
	asuka_call_on_cancel(call, on_cancel, r);
}

static void r_stop(record_Recorder *self, asuka_error *err) {
	if (now) atomic_store(&now->stop, true);
}

static void r_pause(record_Recorder *self, asuka_error *err) {
	if (now) atomic_store(&now->paused, true);
}

static void r_resume(record_Recorder *self, asuka_error *err) {
	if (now) atomic_store(&now->paused, false);
}

static record_Status r_status(record_Recorder *self, asuka_error *err) {
	recording *r = now;
	if (!r) return (record_Status){0};
	record_Status s = {.recording = atomic_load(&r->recording), .elapsed = (int64_t)atomic_load(&r->ms) * 1000000};
	if (s.recording) s.level = atomic_load(&r->level) / 255.0;
	return s;
}

// Windows never asks a desktop app for the microphone: Settings' privacy
// switches (every app's, desktop apps', a policy's) allow it or don't.
static const char *access_now(void) { return denied() ? "denied" : "allowed"; }

static char *r_access(record_Recorder *self, asuka_error *err) { return asuka_strdup(access_now()); }

// requestAccess has nothing to ask: the switches' answer, at once.
static void r_request_access(record_Recorder *self, asuka_call *call) { record_Recorder_requestAccess_done(call, asuka_strdup(access_now())); }

static void r_release(record_Recorder *self) {}  // the one recorder lives with the app

static const record_Recorder_vtable vt = {.release = r_release,
                                          .record = r_record,
                                          .stop = r_stop,
                                          .pause = r_pause,
                                          .resume = r_resume,
                                          .status = r_status,
                                          .access = r_access,
                                          .requestAccess = r_request_access};

record_Recorder *record_make_Recorder(asuka_error *err) {
	static record_Recorder recorder = {&vt};
	return &recorder;
}
