// platform/transcribe on Windows: SAPI 5's in-process recognizer, the
// desktop speech recognizer Windows has on the device (English's comes
// with it, other languages' with their language packs' speech feature):
// its dictation, which needs nothing from a server, unlike WinRT's
// SpeechRecognizer. A recording is decoded by Media Foundation (whatever
// it plays: platform/record's .m4a, WAV, MP3), down to 16 kHz mono PCM in
// a temporary WAV file, and read by the recognizer from it; the
// microphone is the default audio input. Each runs on a thread of its
// own in COM's multithreaded apartment, waiting on the recognition
// context's events; listen's are posted to the main thread as they come.
// macOS runs the package's Swift.
#include "asuka_windows.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propidl.h>
#include <sapi.h>
#include "transcribe.asuka.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID clsid_inproc = {0x41B89B6B, 0x9399, 0x11D2, {0x96, 0x23, 0x00, 0xC0, 0x4F, 0x8E, 0xE6, 0x28}};
static const GUID iid_recognizer = {0xC2B5F241, 0xDAA0, 0x4507, {0x9E, 0x16, 0x5A, 0x1E, 0xAA, 0x2B, 0x7A, 0x5C}};
static const GUID clsid_stream = {0x715D9C59, 0x4442, 0x11D2, {0x96, 0x05, 0x00, 0xC0, 0x4F, 0x8E, 0xE6, 0x28}};
static const GUID iid_stream = {0x12E3CCA9, 0x7518, 0x44C5, {0xA5, 0xE7, 0xBA, 0x5A, 0x79, 0xCB, 0x92, 0x9E}};
static const GUID clsid_token = {0xEF411752, 0x3736, 0x4CB4, {0x9C, 0x8C, 0x8E, 0xF4, 0xCC, 0xB5, 0x8E, 0xFE}};
static const GUID iid_token = {0x14056589, 0xE16C, 0x11D2, {0xBB, 0x90, 0x00, 0xC0, 0x4F, 0x8E, 0xE6, 0xC0}};
static const GUID clsid_category = {0xA910187F, 0x0C7A, 0x45AC, {0x92, 0xCC, 0x59, 0xED, 0xAF, 0xB7, 0x7B, 0x53}};
static const GUID iid_category = {0x2D3D3845, 0x39AF, 0x4850, {0xBB, 0xF9, 0x40, 0xB4, 0x97, 0x80, 0x01, 0x1D}};
static const GUID spdfid_wave = {0xC31ADBAE, 0x527F, 0x4FF5, {0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C}};

static const GUID mt_major = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
static const GUID mt_subtype = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
static const GUID mt_channels = {0x37e48bf5, 0x645e, 0x4c5b, {0x89, 0xde, 0xad, 0xa9, 0xe2, 0x9b, 0x69, 0x6a}};
static const GUID mt_rate = {0x5faeeae7, 0x0290, 0x4c31, {0x9e, 0x8a, 0xc5, 0x34, 0xf6, 0x8d, 0x9d, 0xba}};
static const GUID mt_bits = {0xf2deb57f, 0x40fa, 0x4764, {0xaa, 0x33, 0xed, 0x4f, 0x2d, 0x1f, 0xf6, 0x69}};
static const GUID mf_audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID mf_pcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

enum { RATE = 16000 };  // what the desktop recognizer listens at

// The events a recognition waits for: SPFEI's bits (each event's, and
// SPFEI_FLAGCHECK's two).
#define EVENT(e) (1ull << (e))
static const ULONGLONG interest = EVENT(SPEI_END_SR_STREAM) | EVENT(SPEI_PHRASE_START) | EVENT(SPEI_RECOGNITION) | EVENT(SPEI_HYPOTHESIS) |
                                  EVENT(SPEI_FALSE_RECOGNITION) | 1ull << 30 | 1ull << 33;

// A recognition: a file's or the microphone's.
typedef struct job {
	asuka_call *call;
	wchar_t *path;  // the recording (NULL: the microphone)
	char *language;
	transcribe_fn_Heard heard;
	HANDLE stop;    // the task went (manual reset)
	HANDLE finish;  // finish was called (manual reset)
} job;

static job *listening;  // main thread

static void job_free(void *ctx) {
	job *j = ctx;
	if (listening == j) listening = NULL;
	free(j->path);
	free(j->language);
	CloseHandle(j->stop);
	CloseHandle(j->finish);
	free(j);
}

static void cancel_job(void *ctx) { SetEvent(((job *)ctx)->stop); }

// ---- Languages: the recognizers' tokens, the 64-bit registry view ----

#define TOKENS L"SOFTWARE\\Microsoft\\Speech\\Recognizers\\Tokens"

static char *device_language(void) {
	wchar_t w[LOCALE_NAME_MAX_LENGTH];
	if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH) > 0) return asuka_narrow(w, -1);
	return asuka_strdup("en-US");
}

static char *lcid_tag(LCID l) {
	wchar_t w[LOCALE_NAME_MAX_LENGTH];
	if (!l || !LCIDToLocaleName(l, w, LOCALE_NAME_MAX_LENGTH, 0)) return asuka_strdup("");
	return asuka_narrow(w, -1);
}

// recognizer_for is the token id of the recognizer for tag (its LCID's,
// or else one of its language's: "en-AU" heard by en-US's), or NULL; *got
// is the LCID it recognizes.
static wchar_t *recognizer_for(const char *tag, LCID *got) {
	wchar_t *wt = asuka_wide(tag);
	LCID want = LocaleNameToLCID(wt, 0);
	free(wt);
	if (!want) return NULL;
	HKEY k;
	if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, TOKENS, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return NULL;
	wchar_t best[256] = L"";
	int score = 0;
	for (DWORD i = 0;; i++) {
		wchar_t name[200], langs[128];
		DWORD n = 200;
		if (RegEnumKeyExW(k, i, name, &n, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
		wchar_t sub[256];
		swprintf(sub, 256, L"%ls\\Attributes", name);
		DWORD size = sizeof langs;
		if (RegGetValueW(k, sub, L"Language", RRF_RT_REG_SZ, NULL, langs, &size) != ERROR_SUCCESS) continue;
		// "409;9": the LCIDs it recognizes, in hexadecimal.
		LCID first = 0;
		for (wchar_t *p = langs; *p;) {
			wchar_t *end;
			LCID l = (LCID)wcstoul(p, &end, 16);
			if (end == p) break;
			if (!first) first = l;
			int s = l == want ? 2 : PRIMARYLANGID(l) == PRIMARYLANGID(want) ? 1 : 0;
			if (s > score) {
				score = s;
				swprintf(best, 256, L"HKEY_LOCAL_MACHINE\\" TOKENS L"\\%ls", name);
				*got = s == 2 ? want : first;
			}
			p = *end == L';' ? end + 1 : end;
		}
	}
	RegCloseKey(k);
	if (!score) return NULL;
	size_t len = wcslen(best) + 1;
	wchar_t *out = malloc(len * sizeof *out);
	memcpy(out, best, len * sizeof *out);
	return out;
}

static char *language_of(const char *language) {
	return language && *language ? asuka_strdup(language) : device_language();
}

static void check(transcribe_Recognizer *self, const char *language, asuka_call *call) {
	transcribe_Availability a = {.language = language_of(language)};
	LCID got = 0;
	wchar_t *id = recognizer_for(a.language, &got);
	a.onDevice = id != NULL;
	free(id);
	transcribe_Recognizer_check_done(call, a);
}

// ---- Settings' privacy switch for the microphone ----

static bool reg_deny(HKEY root, const wchar_t *path) {
	wchar_t v[16];
	DWORD n = sizeof v;
	return RegGetValueW(root, path, L"Value", RRF_RT_REG_SZ, NULL, v, &n) == ERROR_SUCCESS && !wcscmp(v, L"Deny");
}

static bool mic_denied(void) {
	const wchar_t *base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone";
	const wchar_t *np = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\microphone\\NonPackaged";
	HKEY roots[2] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
	for (int i = 0; i < 2; i++)
		if (reg_deny(roots[i], base) || reg_deny(roots[i], np)) return true;
	return false;
}

// ---- Decoding: Media Foundation's source reader, to PCM ----

static struct {
	bool started;
	HRESULT(WINAPI *startup)(ULONG, DWORD);
	HRESULT(WINAPI *media_type)(IMFMediaType **);
	HRESULT(WINAPI *reader)(LPCWSTR, IMFAttributes *, IMFSourceReader **);
} mf;

static bool mf_start(void) {
	static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
	BOOL pending;
	InitOnceBeginInitialize(&once, 0, &pending, NULL);
	if (pending) {
		HMODULE plat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		HMODULE rw = LoadLibraryExW(L"mfreadwrite.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (plat && rw) {
			*(FARPROC *)&mf.startup = GetProcAddress(plat, "MFStartup");
			*(FARPROC *)&mf.media_type = GetProcAddress(plat, "MFCreateMediaType");
			*(FARPROC *)&mf.reader = GetProcAddress(rw, "MFCreateSourceReaderFromURL");
			mf.started = mf.startup && mf.media_type && mf.reader && SUCCEEDED(mf.startup(0x00020070, 0));  // MF_VERSION, MFSTARTUP_FULL
		}
		InitOnceComplete(&once, 0, NULL);
	}
	return mf.started;
}

static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }

// wav_header is a 16-bit mono WAV's header for n bytes of samples.
static void wav_header(uint8_t h[44], uint32_t rate, uint32_t n) {
	memcpy(h, "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x01\0\0\0\0\0\0\0\0\0\x02\0\x10\0data\0\0\0\0", 44);
	put32(h + 4, 36 + n);
	put32(h + 24, rate);
	put32(h + 28, rate * 2);
	put32(h + 40, n);
}

// The decoded sound, down to RATE (or the recording's, if lower) and
// mono: each sample out is the average of the ones in its span.
typedef struct {
	HANDLE file;
	uint32_t in_rate, out_rate, channels;
	double step, at;  // input samples per output one; where the next one ends
	double sum;
	uint32_t count;
	uint64_t n_in;
	uint32_t bytes;
	int16_t buf[4096];
	uint32_t nbuf;
	bool failed;
} pcm_out;

static void flush_buf(pcm_out *o) {
	DWORD w = 0;
	if (o->nbuf && (!WriteFile(o->file, o->buf, o->nbuf * 2, &w, NULL) || w != o->nbuf * 2)) o->failed = true;
	o->bytes += o->nbuf * 2;
	o->nbuf = 0;
}

// rated has o take frames at rate, of channels: the sound's format, as
// its first sample or a change says. The output's rate is the first's.
static void rated(pcm_out *o, uint32_t rate, uint32_t channels) {
	if (!o->out_rate) {
		o->out_rate = rate < RATE ? rate : RATE;
		o->at = (double)rate / o->out_rate;
	}
	o->in_rate = rate, o->channels = channels;
	o->step = (double)rate / o->out_rate;
}

static void push(pcm_out *o, const int16_t *frames, uint32_t n) {
	for (uint32_t i = 0; i < n; i++) {
		int32_t s = 0;
		for (uint32_t c = 0; c < o->channels; c++) s += frames[i * o->channels + c];
		o->sum += (double)s / o->channels;
		o->count++;
		o->n_in++;
		// A span ends here: its average (twice or more, if the rate has
		// fallen below the output's).
		if ((double)o->n_in < o->at) continue;
		double v = o->sum / o->count;
		while ((double)o->n_in >= o->at) {
			o->buf[o->nbuf++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
			if (o->nbuf == 4096) flush_buf(o);
			o->at += o->step;
		}
		o->sum = 0, o->count = 0;
	}
}

// pcm_format is the reader's output's channels and rate, if it's 16-bit
// PCM.
static bool pcm_format(IMFSourceReader *r, uint32_t *channels, uint32_t *rate) {
	IMFMediaType *cur = NULL;
	if (FAILED(IMFSourceReader_GetCurrentMediaType(r, (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &cur))) return false;
	UINT32 c = 0, hz = 0, bits = 0;
	IMFMediaType_GetUINT32(cur, &mt_channels, &c);
	IMFMediaType_GetUINT32(cur, &mt_rate, &hz);
	IMFMediaType_GetUINT32(cur, &mt_bits, &bits);
	IMFMediaType_Release(cur);
	*channels = c, *rate = hz;
	return c && hz && bits == 16;
}

// decode writes path's sound to a temporary WAV file: its name, or NULL
// with err set (or j->stop set: cancelled).
static wchar_t *decode(job *j, uint32_t *rate, asuka_error *err) {
	if (!mf_start()) {
		asuka_error_unsupported(err, "transcribe: reading a recording needs Media Foundation (Windows N: the Media Feature Pack)");
		return NULL;
	}
	IMFSourceReader *r = NULL;
	IMFMediaType *t = NULL;
	wchar_t *out = NULL;
	pcm_out o = {.file = INVALID_HANDLE_VALUE};
	HRESULT hr = mf.reader(j->path, NULL, &r);
	if (FAILED(hr) || !r) {
		asuka_win_fail(err, hr, "transcribe: can't read the recording");
		goto done;
	}
	const DWORD audio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
	IMFSourceReader_SetStreamSelection(r, (DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
	IMFSourceReader_SetStreamSelection(r, audio, TRUE);
	if (FAILED(hr = mf.media_type(&t))) goto fail;
	IMFMediaType_SetGUID(t, &mt_major, &mf_audio);
	IMFMediaType_SetGUID(t, &mt_subtype, &mf_pcm);
	IMFMediaType_SetUINT32(t, &mt_bits, 16);
	if (FAILED(hr = IMFSourceReader_SetCurrentMediaType(r, audio, NULL, t))) goto fail;
	uint32_t channels = 0, in_rate = 0;
	if (!pcm_format(r, &channels, &in_rate)) goto undecodable;
	wchar_t dir[MAX_PATH], name[MAX_PATH];
	if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"asr", 0, name)) {
		asuka_win_fail(err, HRESULT_FROM_WIN32(GetLastError()), "transcribe: no temporary file");
		goto done;
	}
	out = _wcsdup(name);
	o.file = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, NULL);
	if (o.file == INVALID_HANDLE_VALUE) {
		asuka_win_fail(err, HRESULT_FROM_WIN32(GetLastError()), "transcribe: no temporary file");
		goto done;
	}
	uint8_t h[44];
	wav_header(h, RATE, 0);
	DWORD w;
	WriteFile(o.file, h, 44, &w, NULL);
	for (;;) {
		if (WaitForSingleObject(j->stop, 0) == WAIT_OBJECT_0) {
			asuka_error_cancelled(err);
			goto done;
		}
		DWORD stream = 0, flags = 0;
		LONGLONG ts = 0;
		IMFSample *s = NULL;
		if (FAILED(hr = IMFSourceReader_ReadSample(r, audio, 0, &stream, &flags, &ts, &s))) goto fail;
		// The decoder's format, known once it's decoded (HE-AAC's rate and
		// channels, which the file's type doesn't say).
		if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
			if (!pcm_format(r, &channels, &in_rate)) {
				if (s) IMFSample_Release(s);
				goto undecodable;
			}
			if (o.out_rate) rated(&o, in_rate, channels);
		}
		if (s && !o.out_rate) rated(&o, in_rate, channels);
		if (s) {
			IMFMediaBuffer *b = NULL;
			if (SUCCEEDED(IMFSample_ConvertToContiguousBuffer(s, &b))) {
				BYTE *p = NULL;
				DWORD n = 0;
				if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &n))) {
					push(&o, (const int16_t *)p, n / (2 * channels));
					IMFMediaBuffer_Unlock(b);
				}
				IMFMediaBuffer_Release(b);
			}
			IMFSample_Release(s);
		}
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
	}
	flush_buf(&o);
	if (!o.out_rate) o.out_rate = RATE;  // no sound at all
	wav_header(h, o.out_rate, o.bytes);
	SetFilePointer(o.file, 0, NULL, FILE_BEGIN);
	if (o.failed || !WriteFile(o.file, h, 44, &w, NULL)) {
		asuka_win_fail(err, HRESULT_FROM_WIN32(GetLastError()), "transcribe: can't write the temporary file");
		goto done;
	}
	*rate = o.out_rate;
	goto done;
undecodable:
	asuka_error_failed(err, "transcribe", 0, "transcribe: the recording's sound can't be decoded");
	goto done;
fail:
	asuka_win_fail(err, hr, "transcribe: can't decode the recording");
done:
	if (o.file != INVALID_HANDLE_VALUE) CloseHandle(o.file);
	if (t) IMFMediaType_Release(t);
	if (r) IMFSourceReader_Release(r);
	if (err->status != ASUKA_OK && out) {
		DeleteFileW(out);
		free(out);
		out = NULL;
	}
	return out;
}

// ---- Recognizing ----

static void *token(const wchar_t *id) {
	ISpObjectToken *t = NULL;
	if (FAILED(CoCreateInstance(&clsid_token, NULL, CLSCTX_ALL, &iid_token, (void **)&t)) || !t) return NULL;
	if (FAILED(ISpObjectToken_SetId(t, NULL, id, FALSE))) {
		ISpObjectToken_Release(t);
		return NULL;
	}
	return t;
}

// microphone is the default audio input's token, or NULL (none).
static void *microphone(void) {
	ISpObjectTokenCategory *c = NULL;
	if (FAILED(CoCreateInstance(&clsid_category, NULL, CLSCTX_ALL, &iid_category, (void **)&c)) || !c) return NULL;
	void *t = NULL;
	LPWSTR id = NULL;
	if (SUCCEEDED(ISpObjectTokenCategory_SetId(c, SPCAT_AUDIOIN, FALSE)) && SUCCEEDED(ISpObjectTokenCategory_GetDefaultTokenId(c, &id)) && id) {
		t = token(id);
		CoTaskMemFree(id);
	}
	ISpObjectTokenCategory_Release(c);
	return t;
}

static void clear_event(SPEVENT *e) {
	if (e->elParamType == SPET_LPARAM_IS_TOKEN || e->elParamType == SPET_LPARAM_IS_OBJECT) asuka_com_release((void *)e->lParam);
	else if (e->elParamType == SPET_LPARAM_IS_POINTER || e->elParamType == SPET_LPARAM_IS_STRING) CoTaskMemFree((void *)e->lParam);
}

// What's been said: the phrases' text, and their words' timing.
typedef struct {
	char *text;
	size_t len;
	transcribe_Segment *segs;
	size_t nsegs, capsegs;
} said;

static void append(said *s, const char *t) {
	if (!*t) return;
	size_t n = strlen(t);
	s->text = realloc(s->text, s->len + n + 2);
	if (s->len) s->text[s->len++] = ' ';
	memcpy(s->text + s->len, t, n + 1);
	s->len += n;
}

static char *phrase_text(ISpRecoResult *r) {
	LPWSTR w = NULL;
	if (FAILED(ISpRecoResult_GetText(r, SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE, TRUE, &w, NULL)) || !w) return asuka_strdup("");
	char *t = asuka_narrow(w, -1);
	CoTaskMemFree(w);
	return t;
}

// words adds r's words, timed in the stream, to s.
static void words(said *s, ISpRecoResult *r) {
	SPPHRASE *p = NULL;
	SPRECORESULTTIMES times = {0};
	if (FAILED(ISpRecoResult_GetPhrase(r, &p)) || !p) return;
	ISpRecoResult_GetResultTimes(r, &times);
	for (ULONG i = 0; i < p->Rule.ulCountOfElements; i++) {
		const SPPHRASEELEMENT *e = &p->pElements[i];
		if (!e->pszDisplayText) continue;
		if (s->nsegs == s->capsegs) {
			s->capsegs = s->capsegs ? s->capsegs * 2 : 16;
			s->segs = realloc(s->segs, s->capsegs * sizeof *s->segs);
		}
		s->segs[s->nsegs++] = (transcribe_Segment){
			.text = asuka_narrow(e->pszDisplayText, -1),
			.start = (double)(times.ullStart + e->ulAudioTimeOffset) / 1e7,
			.duration = (double)e->ulAudioSizeTime / 1e7,
		};
	}
	CoTaskMemFree(p);
}

typedef enum { END_STREAM, END_FINISHED, END_CANCELLED } ending;

// recognize has rec's dictation hear input until its stream ends, finish
// is called (listening) or the task goes, adding what it hears to s (and,
// listening, telling j->heard as it goes). An HRESULT: why it couldn't.
static HRESULT recognize(job *j, ISpRecognizer *rec, said *s, ending *how) {
	ISpRecoContext *ctx = NULL;
	ISpRecoGrammar *g = NULL;
	HRESULT hr;
	if (FAILED(hr = ISpRecognizer_CreateRecoContext(rec, &ctx))) goto done;
	if (FAILED(hr = ISpRecoContext_SetNotifyWin32Event(ctx))) goto done;
	if (FAILED(hr = ISpRecoContext_SetInterest(ctx, interest, interest))) goto done;
	if (FAILED(hr = ISpRecoContext_CreateGrammar(ctx, 1, &g))) goto done;
	if (FAILED(hr = ISpRecoGrammar_LoadDictation(g, NULL, SPLO_STATIC))) goto done;
	if (FAILED(hr = ISpRecoGrammar_SetDictationState(g, SPRS_ACTIVE))) goto done;
	ISpRecognizer_SetRecoState(rec, SPRST_ACTIVE);
	HANDLE ev = ISpRecoContext_GetNotifyEventHandle(ctx);
	bool phrase = false, finishing = false;
	for (;;) {
		HANDLE waits[3] = {j->stop, ev, j->finish};
		DWORD got = WaitForMultipleObjects(j->path || finishing ? 2 : 3, waits, FALSE, INFINITE);
		if (got == WAIT_OBJECT_0) {
			*how = END_CANCELLED;
			break;
		}
		if (got == WAIT_OBJECT_0 + 2) {
			// Finishing: the microphone off; a phrase being said is heard to
			// its end.
			finishing = true;
			ISpRecognizer_SetRecoState(rec, SPRST_INACTIVE);
			if (!phrase) {
				*how = END_FINISHED;
				break;
			}
			continue;
		}
		bool ended = false;
		for (;;) {
			SPEVENT e = {0};
			ULONG n = 0;
			if (FAILED(ISpRecoContext_GetEvents(ctx, 1, &e, &n)) || !n) break;
			switch (e.eEventId) {
			case SPEI_PHRASE_START: phrase = true; break;
			case SPEI_HYPOTHESIS:
				if (!j->path && e.lParam) {
					char *t = phrase_text((ISpRecoResult *)e.lParam);
					said now = {.text = s->text ? asuka_strdup(s->text) : NULL, .len = s->len};
					append(&now, t);
					transcribe_Heard h = {.text = now.text ? now.text : "", .final = false};
					transcribe_fn_Heard_call(j->heard, &h);
					free(now.text);
					free(t);
				}
				break;
			case SPEI_RECOGNITION:
				phrase = false;
				if (e.lParam) {
					char *t = phrase_text((ISpRecoResult *)e.lParam);
					append(s, t);
					free(t);
					words(s, (ISpRecoResult *)e.lParam);
					if (!j->path) {
						transcribe_Heard h = {.text = s->text ? s->text : "", .final = false};
						transcribe_fn_Heard_call(j->heard, &h);
					}
				}
				if (finishing) ended = true, *how = END_FINISHED;
				break;
			case SPEI_FALSE_RECOGNITION:
				phrase = false;
				if (finishing) ended = true, *how = END_FINISHED;
				break;
			case SPEI_END_SR_STREAM: ended = true, *how = finishing ? END_FINISHED : END_STREAM; break;
			default: break;
			}
			clear_event(&e);
		}
		if (ended) break;
	}
	ISpRecognizer_SetRecoState(rec, SPRST_INACTIVE_WITH_PURGE);
	hr = S_OK;
done:
	if (g) ISpRecoGrammar_Release(g);
	if (ctx) ISpRecoContext_Release(ctx);
	return hr;
}

// ---- The worker: a file's or the microphone's recognition ----

static void work(void *ctx) {
	job *j = ctx;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	asuka_error err = {0};
	said s = {0};
	ending how = END_STREAM;
	ISpRecognizer *rec = NULL;
	void *recog = NULL, *input = NULL;
	wchar_t *wav = NULL;
	LCID lcid = 0;
	wchar_t *id = recognizer_for(j->language, &lcid);
	if (!id) {
		asuka_error_declared(&err, transcribe_TranscribeError_notAvailable);
		goto answer;
	}
	if (j->path) {
		uint32_t rate = 0;
		if (!(wav = decode(j, &rate, &err))) goto answer;
		ISpStream *st = NULL;
		WAVEFORMATEX wf = {WAVE_FORMAT_PCM, 1, rate, rate * 2, 2, 16, 0};
		HRESULT hr = CoCreateInstance(&clsid_stream, NULL, CLSCTX_ALL, &iid_stream, (void **)&st);
		if (SUCCEEDED(hr)) hr = ISpStream_BindToFile(st, wav, SPFM_OPEN_READONLY, &spdfid_wave, &wf, 0);
		if (FAILED(hr)) {
			if (st) ISpStream_Release(st);
			asuka_win_fail(&err, hr, "transcribe: SAPI can't read the sound");
			goto answer;
		}
		input = st;
	} else if (!(input = microphone())) {
		asuka_error_declared(&err, transcribe_TranscribeError_notAvailable);  // no microphone
		goto answer;
	}
	HRESULT hr = CoCreateInstance(&clsid_inproc, NULL, CLSCTX_ALL, &iid_recognizer, (void **)&rec);
	if (FAILED(hr) || !rec || !(recog = token(id))) {
		asuka_error_declared(&err, transcribe_TranscribeError_notAvailable);
		goto answer;
	}
	if (FAILED(hr = ISpRecognizer_SetRecognizer(rec, recog)) || FAILED(hr = ISpRecognizer_SetInput(rec, input, TRUE))) {
		if (j->path) asuka_win_fail(&err, hr, "transcribe: the recognizer can't take the sound");
		else asuka_error_declared(&err, transcribe_TranscribeError_notAvailable);  // the microphone wouldn't open
		goto answer;
	}
	if (FAILED(hr = recognize(j, rec, &s, &how))) {
		asuka_win_fail(&err, hr, "transcribe: dictation isn't there");
		goto answer;
	}
	if (how == END_CANCELLED) asuka_error_cancelled(&err);
answer:
	if (rec) ISpRecognizer_Release(rec);
	asuka_com_release(recog);
	asuka_com_release(input);
	if (wav) {
		DeleteFileW(wav);
		free(wav);
	}
	free(id);
	if (err.status != ASUKA_OK) {
		for (size_t i = 0; i < s.nsegs; i++) free(s.segs[i].text);
		free(s.segs);
		free(s.text);
		asuka_call_fail(j->call, &err);
	} else if (j->path) {
		transcribe_Result r = {
			.text = s.text ? s.text : asuka_strdup(""),
			.segments = {s.segs, s.nsegs},
			.language = lcid_tag(lcid),
		};
		transcribe_Recognizer_file_done(j->call, r);
	} else {
		transcribe_Heard h = {.text = s.text ? s.text : "", .final = true};
		transcribe_fn_Heard_call(j->heard, &h);
		free(s.text);
		free(s.segs);
		transcribe_Recognizer_listen_done(j->call);
	}
	CoUninitialize();
	asuka_desktop_post(job_free, j);
}

static job *start(const char *path, const char *language, transcribe_fn_Heard heard, asuka_call *call) {
	job *j = calloc(1, sizeof *j);
	j->call = call;
	j->heard = heard;
	j->path = path ? asuka_wide(path) : NULL;
	j->language = language_of(language);
	j->stop = CreateEventW(NULL, TRUE, FALSE, NULL);
	j->finish = CreateEventW(NULL, TRUE, FALSE, NULL);
	asuka_call_on_cancel(call, cancel_job, j);
	if (!asuka_desktop_spawn(work, j)) {
		asuka_call_off_cancel(call);
		asuka_error e = {0};
		asuka_error_failed(&e, "transcribe", 0, "transcribe: no thread");
		asuka_call_fail(call, &e);
		job_free(j);
		return NULL;
	}
	return j;
}

static void file(transcribe_Recognizer *self, const char *path, const char *language, asuka_call *call) {
	start(path && *path ? path : "", language, (transcribe_fn_Heard){0}, call);
}

static void listen(transcribe_Recognizer *self, const char *language, transcribe_fn_Heard heard, asuka_call *call) {
	asuka_error e = {0};
	if (listening) {
		asuka_error_failed(&e, "transcribe", 1, "transcribe: already listening");
		asuka_call_fail(call, &e);
		return;
	}
	if (mic_denied()) {
		asuka_error_declared(&e, transcribe_TranscribeError_notAllowed);
		asuka_call_fail(call, &e);
		return;
	}
	listening = start(NULL, language, heard, call);  // until its job's freed
}

static void finish(transcribe_Recognizer *self, asuka_error *err) {
	if (listening) SetEvent(listening->finish);
}

static const transcribe_Recognizer_vtable vt = {.check = check, .file = file, .listen = listen, .finish = finish};

transcribe_Recognizer *transcribe_make_Recognizer(asuka_error *err) {
	transcribe_Recognizer *r = malloc(sizeof *r);
	r->vt = &vt;
	return r;
}
