// platform/camera on Windows, with Media Foundation's capture engine
// (IMFCaptureEngine, Windows 8). The View is a window of its own the
// engine's preview sink draws the camera into, filling it (cropped to
// keep its aspect); its photo sink takes capture's photos (JPEG), its
// record sink record's videos in video mode (H.264 and AAC in MPEG-4).
// With scan on, the preview comes to the view as frames (the preview
// sink's sample callback), which it draws itself and reads codes in,
// four times a second, with asuka_barcode.h: each code reported as it
// comes into view, again once it's been out of view for a second. take
// has no system camera screen to show: it shows a dialog of the
// package's over the window in use, a live preview with Take Photo and
// Cancel (Enter and Escape), as on a Mac. Photos and videos go to the
// app's caches (asuka-camera). Settings' camera privacy switch refused:
// "not allowed".
//
// The camera is chosen by its facing: the panel the PC's firmware says
// it's on (front: facing the user; back), else as its name says ("Front",
// "Rear"/"Back"); else the first one. One facing the user (a front
// panel's, a USB webcam's) is shown mirrored. Media Foundation is loaded
// when first used (an N edition of Windows may lack it). The engine's
// events come on Media Foundation's threads and are posted to the main
// one.
//
// The zoom is the camera's own where its control's values read as
// factors (IAMCameraControl's zoom: a UVC camera's), else digital in
// photo mode: the middle of the frames, the preview's (its sink's source
// rectangle, or the frames drawn), photos' (cropped as they're written)
// and frames'; the capture engine records what the camera sees whole, so
// in video mode a webcam's zoom is 1. Ctrl and the wheel, or a
// touchscreen's pinch, zoom. A webcam has no torch, flash, exposure bias
// or focus point a desktop app reaches through these: they do nothing.
// capture's photo is the engine's, rewritten with WIC (cropped, mirrored,
// scaled, of its quality) when it must be. In video mode the camera's
// media type nearest videoQuality's size is chosen, and mirror mirrors
// the camera's stream (IMFCaptureSource's) for a recording of the user.
// frameRate's frames are the preview's (mirrored only with mirror), a ring of JPEGs (WIC) written off
// the main thread; maxBytes ends a recording as its file nears it.
#include "asuka_barcode.h"
#include "asuka_windows.h"
#include <wincodec.h>
#include "camera.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const GUID clsid_engine_factory = {0xEFCE38D3, 0x8914, 0x4674, {0xA7, 0xDF, 0xAE, 0x1B, 0x3D, 0x65, 0x4B, 0x8A}};
static const GUID clsid_engine = {0xEFCE38D3, 0x8914, 0x4674, {0xA7, 0xDF, 0xAE, 0x1B, 0x3D, 0x65, 0x4B, 0x8A}};
static const GUID iid_engine_factory = {0x8F02D140, 0x56FC, 0x4302, {0xA7, 0x05, 0x3A, 0x97, 0xC7, 0x8B, 0xE7, 0x79}};
static const GUID iid_engine = {0xA6BBA433, 0x176B, 0x48B2, {0xB3, 0x75, 0x53, 0xAA, 0x03, 0x47, 0x32, 0x07}};
static const GUID iid_event_callback = {0xAEDA51C0, 0x9025, 0x4983, {0x90, 0x12, 0xDE, 0x59, 0x7B, 0x88, 0xB0, 0x89}};
static const GUID iid_sample_callback = {0x52150B82, 0xAB39, 0x4467, {0x98, 0x0F, 0xE4, 0x8B, 0xF0, 0x82, 0x2E, 0xCD}};
static const GUID iid_preview_sink = {0x77346CFD, 0x5B49, 0x4D73, {0xAC, 0xE0, 0x5B, 0x52, 0xA8, 0x59, 0xF2, 0xE0}};
static const GUID iid_photo_sink = {0xD2D43CC8, 0x48BB, 0x4AA7, {0x95, 0xDB, 0x10, 0xC0, 0x69, 0x77, 0xE7, 0x77}};
static const GUID iid_record_sink = {0x3323B55A, 0xF92A, 0x4FE2, {0x8E, 0xDC, 0xE9, 0xBF, 0xC0, 0x63, 0x4D, 0x77}};
static const GUID iid_media_source = {0x279A808D, 0xAEC7, 0x40C8, {0x9C, 0x6B, 0xA6, 0xB4, 0x92, 0xC7, 0x8A, 0x66}};
static const GUID mf_video_only = {0x7E025171, 0xCF32, 0x4F2E, {0x8F, 0x19, 0x41, 0x05, 0x77, 0xB7, 0x3A, 0x66}};
static const GUID mf_media_type_image = {0x72178C23, 0xE45B, 0x11D5, {0xBC, 0x2A, 0x00, 0xB0, 0xD0, 0xF3, 0xF4, 0xAB}};
static const GUID mt_major_type = {0x48EBA18E, 0xF8C9, 0x4687, {0xBF, 0x11, 0x0A, 0x74, 0xC9, 0xF9, 0x6A, 0x8F}};
static const GUID mt_subtype = {0xF7E34C9A, 0x42E8, 0x4714, {0xB7, 0x4B, 0xCB, 0x29, 0xD7, 0x2C, 0x35, 0xE5}};
static const GUID mt_audio_channels = {0x37E48BF5, 0x645E, 0x4C5B, {0x89, 0xDE, 0xAD, 0xA9, 0xE2, 0x9B, 0x69, 0x6A}};
static const GUID mt_audio_rate = {0x5FAEEAE7, 0x0290, 0x4C31, {0x9E, 0x8A, 0xC5, 0x34, 0xF6, 0x8D, 0x9D, 0xBA}};
static const GUID mt_audio_bits = {0xF2DEB57F, 0x40FA, 0x4764, {0xAA, 0x33, 0xED, 0x4F, 0x2D, 0x1F, 0xF6, 0x69}};
static const GUID mt_audio_bytes_per_sec = {0x1AAB75C8, 0xCFEF, 0x451C, {0xAB, 0x95, 0xAC, 0x03, 0x4B, 0x8E, 0x17, 0x31}};
static const GUID mt_aac_profile = {0x7632F0E6, 0x9538, 0x4D61, {0xAC, 0xDA, 0xEA, 0x29, 0xC8, 0xC1, 0x44, 0x56}};
static const GUID mt_frame_size = {0x1652C33D, 0xD6B2, 0x4012, {0xB8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xA3, 0x7D}};
static const GUID mt_frame_rate = {0xC459A2E8, 0x3D2C, 0x4E44, {0xB1, 0x32, 0xFE, 0xE5, 0x15, 0x6C, 0x7B, 0xB0}};
static const GUID mt_avg_bitrate = {0x20332624, 0xFB0D, 0x4D9E, {0xBD, 0x0D, 0xCB, 0xF6, 0x78, 0x6C, 0x10, 0x2E}};
static const GUID mt_interlace = {0xE2724BB8, 0xE676, 0x4806, {0xB4, 0xB2, 0xA8, 0xD6, 0xEF, 0xB4, 0x4C, 0xCD}};
static const GUID mt_pixel_aspect = {0xC6376A1E, 0x8D0A, 0x4027, {0xBE, 0x45, 0x6D, 0x9A, 0x0A, 0xD3, 0x9B, 0xB6}};
static const GUID mt_default_stride = {0x644B4E48, 0x1E02, 0x4516, {0xB0, 0xEB, 0xC0, 0x1C, 0xA9, 0xD4, 0x9A, 0xC6}};
static const GUID mf_audio = {0x73647561, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID mf_video = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID mf_aac = {0x00001610, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID mf_h264 = {0x34363248, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID mf_rgb32 = {0x00000016, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
static const GUID mf_jpeg = {0x19E4A5AA, 0x5662, 0x4FC5, {0xA0, 0xC0, 0x17, 0x58, 0x02, 0x8E, 0x10, 0x57}};
static const GUID dev_source_type = {0xC60AC5FE, 0x252A, 0x478F, {0xA0, 0xEF, 0xBC, 0x8F, 0xA5, 0xF7, 0xCA, 0xD3}};
static const GUID dev_source_vidcap = {0x8AC3587A, 0x4AE7, 0x42D8, {0x99, 0xE0, 0x0A, 0x60, 0x13, 0xEE, 0xF9, 0x0F}};
static const GUID dev_source_name = {0x60D0E559, 0x52F8, 0x4FA2, {0xBB, 0xCE, 0xAC, 0xDB, 0x34, 0xA8, 0xEC, 0x01}};
static const GUID dev_source_link = {0x58F0AAD8, 0x22BF, 0x4F8A, {0xBB, 0x3D, 0xD2, 0xC4, 0x97, 0x8C, 0x6E, 0x2F}};
static const GUID iid_camera_control = {0xC6E13370, 0x30AC, 0x11D0, {0xA1, 0x8C, 0x00, 0xA0, 0xC9, 0x11, 0x89, 0x56}};

// The capture engine's events.
static const GUID ev_initialized = {0x219992BC, 0xCF92, 0x4531, {0xA1, 0xAE, 0x96, 0xE1, 0xE8, 0x86, 0xC8, 0xF1}};
static const GUID ev_preview_started = {0xA416DF21, 0xF9D3, 0x4A74, {0x99, 0x1B, 0xB8, 0x17, 0x29, 0x89, 0x52, 0xC4}};
static const GUID ev_preview_stopped = {0x13D5143C, 0x1EDD, 0x4E50, {0xA2, 0xEF, 0x35, 0x0A, 0x47, 0x67, 0x80, 0x60}};
static const GUID ev_record_started = {0xAC2B027B, 0xDDF9, 0x48A0, {0x89, 0xBE, 0x38, 0xAB, 0x35, 0xEF, 0x45, 0xC0}};
static const GUID ev_record_stopped = {0x55E5200A, 0xF98F, 0x4C0D, {0xA9, 0xEC, 0x9E, 0xB2, 0x5E, 0xD3, 0xD7, 0x73}};
static const GUID ev_photo_taken = {0x3C50C445, 0x7304, 0x48EB, {0x86, 0x5D, 0xBB, 0xA1, 0x9B, 0xA3, 0xAF, 0x5C}};
static const GUID ev_error = {0x46B89FC6, 0x33CC, 0x4399, {0x9D, 0xAD, 0x78, 0x4D, 0xE7, 0x7D, 0x58, 0x7C}};

// The preferred streams of the engine's source.
enum { stream_preview = 0xFFFFFFFA, stream_record = 0xFFFFFFF9, stream_photo = 0xFFFFFFF8, stream_audio = 0xFFFFFFF7 };

// Slots: IMFAttributes', IMFCaptureEngine's and its sinks'.
enum {
	attr_get_u32 = 7,
	attr_get_u64 = 8,
	attr_get_string_length = 11,
	attr_get_string = 12,
	attr_set_u32 = 21,
	attr_set_u64 = 22,
	attr_set_guid = 24,
	attr_copy_all = 32,
	activate_object = 33,
	event_extended_type = 34,
	event_status = 35,
	source_shutdown = 12,
	sample_to_buffer = 41,
	buffer_lock = 3,
	buffer_unlock = 4,
	engine_initialize = 3,
	engine_start_preview = 4,
	engine_stop_preview = 5,
	engine_start_record = 6,
	engine_stop_record = 7,
	engine_take_photo = 8,
	engine_get_sink = 9,
	engine_get_source = 10,
	capture_source_available_type = 9,
	capture_source_set_type = 10,
	capture_source_current_type = 11,
	capture_source_set_mirror = 15,
	capture_source_stream_index = 16,
	camera_control_range = 3,
	camera_control_set = 4,
	sink_add_stream = 5,
	sink_remove_all = 7,
	preview_set_hwnd = 8,
	preview_update_video = 10,
	preview_set_sample_callback = 11,
	preview_set_mirror = 13,
	photo_set_file = 8,
	record_set_file = 9,
};

// COM calls by slot, of n arguments of a pointer's size (a pointer, a
// DWORD, a BOOL), exactly: stdcall's callee pops what it takes on x86.
#define U(x) ((uintptr_t)(x))
#define CALL1(o, slot, a) ASUKA_SLOT(o, slot, HRESULT(STDMETHODCALLTYPE *)(void *, uintptr_t))(o, U(a))
#define CALL2(o, slot, a, b) ASUKA_SLOT(o, slot, HRESULT(STDMETHODCALLTYPE *)(void *, uintptr_t, uintptr_t))(o, U(a), U(b))
#define CALL3(o, slot, a, b, c) ASUKA_SLOT(o, slot, HRESULT(STDMETHODCALLTYPE *)(void *, uintptr_t, uintptr_t, uintptr_t))(o, U(a), U(b), U(c))
#define CALL4(o, slot, a, b, c, d) \
	ASUKA_SLOT(o, slot, HRESULT(STDMETHODCALLTYPE *)(void *, uintptr_t, uintptr_t, uintptr_t, uintptr_t))(o, U(a), U(b), U(c), U(d))
#define CALL0(obj, slot) ASUKA_SLOT(obj, slot, HRESULT(STDMETHODCALLTYPE *)(void *))(obj)

// ---- Media Foundation ----

static struct {
	bool tried, ok;
	HRESULT(WINAPI *startup)(ULONG, DWORD);
	HRESULT(WINAPI *media_type)(void **);
	HRESULT(WINAPI *attributes)(void **, UINT32);
	HRESULT(WINAPI *enum_devices)(void *, void ***, UINT32 *);
	const char *why;
} mf;

// mf_start loads and starts Media Foundation, once: NULL, or why it can't.
static const char *mf_start(void) {
	if (mf.tried) return mf.ok ? NULL : mf.why;
	mf.tried = true;
	HMODULE plat = LoadLibraryExW(L"mfplat.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	HMODULE mfd = LoadLibraryExW(L"mf.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (plat) {
		*(FARPROC *)&mf.startup = GetProcAddress(plat, "MFStartup");
		*(FARPROC *)&mf.media_type = GetProcAddress(plat, "MFCreateMediaType");
		*(FARPROC *)&mf.attributes = GetProcAddress(plat, "MFCreateAttributes");
	}
	if (mfd) *(FARPROC *)&mf.enum_devices = GetProcAddress(mfd, "MFEnumDeviceSources");
	if (!mf.startup || !mf.media_type || !mf.attributes) {
		mf.why = "Media Foundation isn't on this Windows (an N edition without its Media Feature Pack)";
		return mf.why;
	}
	if (FAILED(mf.startup(0x00020070, 0))) {  // MF_VERSION, MFSTARTUP_FULL
		mf.why = "Media Foundation didn't start";
		return mf.why;
	}
	mf.ok = true;
	return NULL;
}

static void set_guid(void *t, const GUID *k, const GUID *v) { CALL2(t, attr_set_guid, k, v); }
static void set_u32(void *t, const GUID *k, UINT32 v) { CALL2(t, attr_set_u32, k, v); }
static void set_u64(void *t, const GUID *k, UINT64 v) { ASUKA_SLOT(t, attr_set_u64, HRESULT(STDMETHODCALLTYPE *)(void *, const GUID *, UINT64))(t, k, v); }
static bool get_u64(void *t, const GUID *k, UINT64 *v) { return SUCCEEDED(CALL2(t, attr_get_u64, k, v)); }

static void *new_media_type(void) {
	void *t = NULL;
	return SUCCEEDED(mf.media_type(&t)) ? t : NULL;
}

// The cameras.

enum { panel_unknown, panel_front, panel_back };

typedef struct {
	char *name;
	void *activate;  // IMFActivate
	int panel;       // where the PC's firmware says it is
} device;

// The device properties a camera's panel is read from (devpkey.h's).
typedef struct {
	GUID fmtid;
	ULONG pid;
} devkey;

static const devkey key_instance_id = {{0x78C34FC8, 0x104A, 0x4ACA, {0x9E, 0xA4, 0x52, 0x4D, 0x52, 0x99, 0x6E, 0x57}}, 256};
static const devkey key_location = {{0x540B947E, 0x8B40, 0x45BC, {0xA8, 0xA2, 0x6A, 0x0B, 0x89, 0x4C, 0xBD, 0xA2}}, 9};

// panel_of reads the panel a camera's on from its device's physical
// location (ACPI's _PLD, as cfgmgr32 gives it: its Panel bits, 67 to 69:
// 4 front, 5 back). A USB webcam has none: unknown.
static int panel_of(const wchar_t *link) {
	typedef DWORD(WINAPI * iface_prop_fn)(const wchar_t *, const devkey *, ULONG *, BYTE *, ULONG *, ULONG);
	typedef DWORD(WINAPI * locate_fn)(DWORD *, const wchar_t *, ULONG);
	typedef DWORD(WINAPI * node_prop_fn)(DWORD, const devkey *, ULONG *, BYTE *, ULONG *, ULONG);
	static iface_prop_fn iface_prop;
	static locate_fn locate;
	static node_prop_fn node_prop;
	static bool tried;
	if (!tried) {
		tried = true;
		HMODULE cm = LoadLibraryExW(L"cfgmgr32.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (cm) {
			*(FARPROC *)&iface_prop = GetProcAddress(cm, "CM_Get_Device_Interface_PropertyW");
			*(FARPROC *)&locate = GetProcAddress(cm, "CM_Locate_DevNodeW");
			*(FARPROC *)&node_prop = GetProcAddress(cm, "CM_Get_DevNode_PropertyW");
		}
	}
	if (!iface_prop || !locate || !node_prop || !link) return panel_unknown;
	wchar_t id[512];
	ULONG type = 0, size = sizeof id;
	DWORD node = 0;
	if (iface_prop(link, &key_instance_id, &type, (BYTE *)id, &size, 0) || locate(&node, id, 0)) return panel_unknown;  // CR_SUCCESS is 0
	BYTE pld[64];
	size = sizeof pld;
	if (node_prop(node, &key_location, &type, pld, &size, 0) || size < 9) return panel_unknown;
	int panel = (pld[8] >> 3) & 7;
	return panel == 4 ? panel_front : panel == 5 ? panel_back : panel_unknown;
}

static void free_devices(device *ds, UINT32 n) {
	for (UINT32 i = 0; i < n; i++) {
		free(ds[i].name);
		asuka_com_release(ds[i].activate);
	}
	free(ds);
}

// devices are the cameras (NULL with *n 0 when there are none, or no
// way to ask).
static device *devices(UINT32 *n) {
	*n = 0;
	if (mf_start() || !mf.enum_devices) return NULL;
	void *attrs = NULL;
	if (FAILED(mf.attributes(&attrs, 1))) return NULL;
	set_guid(attrs, &dev_source_type, &dev_source_vidcap);
	void **arr = NULL;
	UINT32 count = 0;
	HRESULT hr = mf.enum_devices(attrs, &arr, &count);
	asuka_com_release(attrs);
	if (FAILED(hr) || !count) {
		CoTaskMemFree(arr);
		return NULL;
	}
	device *ds = calloc(count, sizeof *ds);
	for (UINT32 i = 0; i < count; i++) {
		ds[i].activate = arr[i];
		UINT32 len = 0;
		if (SUCCEEDED(CALL2(arr[i], attr_get_string_length, &dev_source_name, &len))) {
			wchar_t *w = calloc(len + 1, sizeof *w);
			if (SUCCEEDED(CALL4(arr[i], attr_get_string, &dev_source_name, w, len + 1, NULL))) ds[i].name = asuka_narrow(w, -1);
			free(w);
		}
		if (!ds[i].name) ds[i].name = asuka_strdup("");
		if (SUCCEEDED(CALL2(arr[i], attr_get_string_length, &dev_source_link, &len))) {
			wchar_t *w = calloc(len + 1, sizeof *w);
			if (SUCCEEDED(CALL4(arr[i], attr_get_string, &dev_source_link, w, len + 1, NULL))) ds[i].panel = panel_of(w);
			free(w);
		}
	}
	CoTaskMemFree(arr);
	*n = count;
	return ds;
}

// device_for is the camera of a facing: on that panel, else named so,
// else the first.
static device *device_for(device *ds, UINT32 n, bool front) {
	if (!n) return NULL;
	for (UINT32 i = 0; i < n; i++)
		if (ds[i].panel == (front ? panel_front : panel_back)) return &ds[i];
	static const char *back_words[] = {"rear", "back", "world"}, *front_words[] = {"front", "user", "integrated"};
	const char **words = front ? front_words : back_words;
	for (UINT32 i = 0; i < n; i++) {
		char *lower = asuka_strdup(ds[i].name);
		for (char *c = lower; *c; c++)
			if (*c >= 'A' && *c <= 'Z') *c += 32;
		bool hit = false;
		for (int k = 0; k < 3 && !hit; k++) hit = strstr(lower, words[k]) != NULL;
		free(lower);
		if (hit) return &ds[i];
	}
	return &ds[0];
}

// capability_denied reports whether Settings' privacy switch for a
// capability ("webcam", "microphone") is off, for every app or for
// desktop apps.
static bool capability_denied(const wchar_t *name) {
	HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
	wchar_t base[200];
	for (int i = 0; i < 2; i++)
		for (int nonpackaged = 0; nonpackaged < 2; nonpackaged++) {
			_snwprintf(base, 200, L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\%ls%ls", name,
			           nonpackaged ? L"\\NonPackaged" : L"");
			wchar_t v[16];
			DWORD n = sizeof v;
			if (RegGetValueW(roots[i], base, L"Value", RRF_RT_REG_SZ, NULL, v, &n) == ERROR_SUCCESS && !wcscmp(v, L"Deny")) return true;
		}
	return false;
}

static bool has_microphone(void) {
	static UINT(WINAPI * count)(void);
	if (!count) {
		HMODULE winmm = LoadLibraryExW(L"winmm.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if (winmm) *(FARPROC *)&count = GetProcAddress(winmm, "waveInGetNumDevs");
	}
	return count && count() > 0;
}

// ---- The capture engine ----

typedef struct {
	void *engine;    // IMFCaptureEngine
	void *source;    // the device's IMFMediaSource
	void *callback;  // its event callback
	void *control;   // its IAMCameraControl, if it has one
	bool audio;
	int width, height;  // the preview's frames, once it's set up
	// Its own zoom, where its values read as factors.
	bool optical;
	camera_zoom_range zoom;
	double max_zoom;
	// The camera's stream mirrored (a recording of the user, mirror on),
	// and so the preview's and the photos' where they're that stream.
	bool preview_mirrored, photo_mirrored;
	double bitrate;  // the recording's, bits a second
} engine;

// An event, from Media Foundation's thread to the main one.
typedef struct {
	unsigned view;  // the view's id
	void *engine;   // which of its engines
	GUID kind;
	HRESULT status;
} engine_event;

static void engine_event_main(void *p);

typedef struct {
	unsigned view;
	void *engine;  // set once made (the callback's made first)
} event_ctx;

static void on_event(void *p, void *event, void *unused) {
	event_ctx *c = p;
	engine_event *e = calloc(1, sizeof *e);
	e->view = c->view;
	e->engine = c->engine;
	CALL1(event, event_extended_type, &e->kind);
	CALL1(event, event_status, &e->status);
	asuka_desktop_post(engine_event_main, e);
}

static void engine_close(engine *e) {
	if (!e) return;
	asuka_com_release(e->control);
	asuka_com_release(e->engine);
	if (e->source) {
		CALL0(e->source, source_shutdown);
		asuka_com_release(e->source);
	}
	asuka_com_release(e->callback);
	free(e);
}

// engine_new starts a capture engine on a camera (with the default
// microphone, for recording, if audio): its events go to the view of id;
// ev_initialized comes first. NULL with *why set.
static engine *engine_new(device *d, bool audio, unsigned view, const char **why) {
	if ((*why = mf_start())) return NULL;
	void *f = NULL;
	if (FAILED(CoCreateInstance(&clsid_engine_factory, NULL, CLSCTX_INPROC_SERVER, &iid_engine_factory, &f)) || !f) {
		*why = "no capture engine on this Windows";
		return NULL;
	}
	engine *e = calloc(1, sizeof *e);
	e->audio = audio;
	HRESULT hr = CALL3(f, 3, &clsid_engine, &iid_engine, &e->engine);  // CreateInstance
	asuka_com_release(f);
	if (FAILED(hr) || !e->engine) {
		free(e);
		*why = "no capture engine";
		return NULL;
	}
	if (FAILED(CALL2(d->activate, activate_object, &iid_media_source, &e->source)) || !e->source) {
		e->source = NULL;
		engine_close(e);
		*why = "no camera";
		return NULL;
	}
	// Its zoom (CameraControl_Zoom), if its control has one.
	e->control = asuka_com_query(e->source, &iid_camera_control);
	long min = 0, max = 0, step = 0, def = 0, caps = 0;
	if (e->control && SUCCEEDED(ASUKA_SLOT(e->control, camera_control_range, HRESULT(STDMETHODCALLTYPE *)(void *, long, long *, long *, long *, long *, long *))(
	                      e->control, 3, &min, &max, &step, &def, &caps))) {
		e->zoom = (camera_zoom_range){min, max};
		e->optical = camera_zoom_optical(e->zoom, &e->max_zoom);
	}
	void *attrs = NULL;
	if (FAILED(mf.attributes(&attrs, 1))) {
		engine_close(e);
		*why = "no camera";
		return NULL;
	}
	if (!audio) set_u32(attrs, &mf_video_only, 1);
	event_ctx *c = calloc(1, sizeof *c);
	c->view = view;
	c->engine = e->engine;
	e->callback = asuka_winrt_delegate(&iid_event_callback, on_event, c, free);
	hr = CALL4(e->engine, engine_initialize, e->callback, attrs, NULL, e->source);
	asuka_com_release(attrs);
	if (FAILED(hr)) {
		engine_close(e);
		*why = "no camera";
		return NULL;
	}
	return e;
}

// sink is one of the engine's sinks as iid (0 record, 1 preview, 2
// photo): a reference, or NULL.
static void *sink(engine *e, int kind, const GUID *iid) {
	void *s = NULL;
	if (FAILED(CALL2(e->engine, engine_get_sink, kind, &s)) || !s) return NULL;
	void *q = asuka_com_query(s, iid);
	asuka_com_release(s);
	return q;
}

// device_type is the source's current media type for a stream (a
// reference), or NULL.
static void *device_type(engine *e, DWORD stream) {
	void *src = NULL, *t = NULL;
	if (FAILED(CALL1(e->engine, engine_get_source, &src)) || !src) return NULL;
	CALL2(src, capture_source_current_type, stream, &t);
	asuka_com_release(src);
	return t;
}

// optical_zoom zooms the camera itself (CameraControl_Zoom, manual).
static void optical_zoom(engine *e, double zoom) {
	if (!e->optical) return;
	ASUKA_SLOT(e->control, camera_control_set, HRESULT(STDMETHODCALLTYPE *)(void *, long, long, long))(e->control, 3, camera_zoom_value(e->zoom, zoom), 2);
}

static void *capture_source(engine *e) {
	void *src = NULL;
	return SUCCEEDED(CALL1(e->engine, engine_get_source, &src)) ? src : NULL;
}

// stream_of is the camera's stream a preferred one is (its index).
static DWORD stream_of(void *src, DWORD preferred) {
	DWORD i = preferred;
	if (FAILED(ASUKA_SLOT(src, capture_source_stream_index, HRESULT(STDMETHODCALLTYPE *)(void *, UINT32, DWORD *))(src, preferred, &i))) i = preferred;
	return i;
}

// set_up chooses, for video mode, the camera's media type nearest
// videoQuality's size (24 frames a second or more, nearest 30), and
// mirrors its stream for a recording of the user that's mirrored: the
// preview's and photos' too where they're that stream.
static void set_up(engine *e, bool video, camera_VideoQuality q, bool mirror) {
	void *src = capture_source(e);
	if (!src) return;
	if (video) {
		camera_size want[4];
		camera_quality_sizes(q, want);
		void *best = NULL;
		double score = 1e18;
		for (DWORD i = 0; i < 4096; i++) {
			void *t = NULL;
			if (FAILED(ASUKA_SLOT(src, capture_source_available_type, HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, DWORD, void **))(src, stream_record, i, &t)) || !t)
				break;
			UINT64 size = 0, rate = 0;
			get_u64(t, &mt_frame_size, &size);
			get_u64(t, &mt_frame_rate, &rate);
			double fps = (UINT32)rate ? (double)(rate >> 32) / (double)(UINT32)rate : 0;
			double d = camera_size_distance(want[0], (int)(size >> 32), (int)(UINT32)size) + (fps < 24 ? 1 : fabs(fps - 30) / 1000);
			if (d < score) {
				asuka_com_release(best);
				best = t, score = d;
			} else {
				asuka_com_release(t);
			}
		}
		if (best) ASUKA_SLOT(src, capture_source_set_type, HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, void *))(src, stream_record, best);
		asuka_com_release(best);
	}
	e->preview_mirrored = e->photo_mirrored = false;
	if (video && mirror) {
		DWORD rec = stream_of(src, stream_record);
		if (SUCCEEDED(ASUKA_SLOT(src, capture_source_set_mirror, HRESULT(STDMETHODCALLTYPE *)(void *, DWORD, BOOL))(src, rec, TRUE))) {
			e->preview_mirrored = stream_of(src, stream_preview) == rec;
			e->photo_mirrored = stream_of(src, stream_photo) == rec;
		}
	}
	asuka_com_release(src);
}

// start_preview draws the preview into hwnd (ev_preview_started follows),
// mirrored if mirror.
static bool start_preview(engine *e, HWND hwnd, bool mirror) {
	void *s = sink(e, 1, &iid_preview_sink);
	if (!s) return false;
	CALL0(s, sink_remove_all);
	void *t = device_type(e, stream_preview);
	bool ok = false;
	if (t) {
		UINT64 size;
		if (get_u64(t, &mt_frame_size, &size)) e->width = (int)(size >> 32), e->height = (int)(UINT32)size;
		DWORD idx;
		ok = SUCCEEDED(CALL4(s, sink_add_stream, (DWORD)stream_preview, t, NULL, &idx)) && SUCCEEDED(CALL1(s, preview_set_hwnd, hwnd));
		if (ok) CALL1(s, preview_set_mirror, mirror ? TRUE : FALSE);
		ok = ok && SUCCEEDED(CALL0(e->engine, engine_start_preview));
		asuka_com_release(t);
	}
	asuka_com_release(s);
	return ok;
}

// fit_preview draws the preview filling w×h pixels of its window,
// cropped to keep its aspect, and to the middle a zoom shows (1: all).
static void fit_preview(engine *e, int w, int h, double zoom) {
	if (w <= 0 || h <= 0) return;
	void *s = sink(e, 1, &iid_preview_sink);
	if (!s) return;
	float src[4] = {0, 0, 1, 1};  // MFVideoNormalizedRect
	if (e->width > 0 && e->height > 0) {
		float fa = (float)w / h, va = (float)e->width / e->height;
		if (va > fa) {  // wider: crop the sides
			float k = fa / va;
			src[0] = (1 - k) / 2, src[2] = (1 + k) / 2;
		} else {
			float k = va / fa;
			src[1] = (1 - k) / 2, src[3] = (1 + k) / 2;
		}
	}
	if (zoom > 1) {
		float k = (float)(1 / zoom);
		for (int i = 0; i < 2; i++) {
			float mid = (src[i] + src[i + 2]) / 2, half = (src[i + 2] - src[i]) / 2 * k;
			src[i] = mid - half, src[i + 2] = mid + half;
		}
	}
	RECT dst = {0, 0, w, h};
	COLORREF black = 0;
	CALL3(s, preview_update_video, src, &dst, &black);
	asuka_com_release(s);
}

// take_photo writes a JPEG of what the camera sees to path
// (ev_photo_taken follows).
static bool take_photo(engine *e, const char *path) {
	void *s = sink(e, 2, &iid_photo_sink);
	if (!s) return false;
	CALL0(s, sink_remove_all);
	bool ok = false;
	void *dt = device_type(e, stream_photo), *t = dt ? new_media_type() : NULL;
	if (t) {
		CALL1(dt, attr_copy_all, t);
		set_guid(t, &mt_major_type, &mf_media_type_image);
		set_guid(t, &mt_subtype, &mf_jpeg);
		DWORD idx;
		wchar_t *w = asuka_wide(path);
		ok = SUCCEEDED(CALL4(s, sink_add_stream, (DWORD)stream_photo, t, NULL, &idx)) && SUCCEEDED(CALL1(s, photo_set_file, w)) &&
		     SUCCEEDED(CALL0(e->engine, engine_take_photo));
		free(w);
	}
	asuka_com_release(t);
	asuka_com_release(dt);
	asuka_com_release(s);
	return ok;
}

// aac_type is AAC-LC of channels at rate, bytes_per_sec (what Windows's
// encoder makes: 12000, 16000, 20000 or 24000).
static void *aac_type(int channels, int rate, int bytes_per_sec) {
	void *t = new_media_type();
	if (!t) return NULL;
	set_guid(t, &mt_major_type, &mf_audio);
	set_guid(t, &mt_subtype, &mf_aac);
	set_u32(t, &mt_audio_channels, channels);
	set_u32(t, &mt_audio_rate, rate);
	set_u32(t, &mt_audio_bits, 16);
	set_u32(t, &mt_audio_bytes_per_sec, bytes_per_sec);
	set_u32(t, &mt_aac_profile, 0x29);  // AAC profile, level 2
	return t;
}

// start_recording records H.264 (and, with sound, AAC from the
// microphone) to an MPEG-4 file at path (ev_record_started follows).
static bool start_recording(engine *e, const char *path, bool sound) {
	void *s = sink(e, 0, &iid_record_sink);
	if (!s) return false;
	CALL0(s, sink_remove_all);
	wchar_t *w = asuka_wide(path);
	HRESULT hr = CALL1(s, record_set_file, w);
	free(w);
	void *dt = SUCCEEDED(hr) ? device_type(e, stream_record) : NULL, *v = dt ? new_media_type() : NULL;
	bool ok = false;
	if (v) {
		set_guid(v, &mt_major_type, &mf_video);
		set_guid(v, &mt_subtype, &mf_h264);
		UINT64 size = 0, rate = 0;
		get_u64(dt, &mt_frame_size, &size);
		if (!get_u64(dt, &mt_frame_rate, &rate) || !(UINT32)rate) rate = (UINT64)30 << 32 | 1;
		set_u64(v, &mt_frame_size, size);
		set_u64(v, &mt_frame_rate, rate);
		set_u64(v, &mt_pixel_aspect, (UINT64)1 << 32 | 1);
		set_u32(v, &mt_interlace, 2);  // progressive
		double fps = (double)(rate >> 32) / (double)((UINT32)rate ? (UINT32)rate : 1);
		double bitrate = (double)(size >> 32) * (double)(UINT32)size * fps * 0.15;
		e->bitrate = (bitrate < 20e6 ? bitrate : 20e6) + (sound && e->audio ? 24000 * 8 : 0);
		set_u32(v, &mt_avg_bitrate, (UINT32)(bitrate < 20e6 ? bitrate : 20e6));
		DWORD idx;
		if (SUCCEEDED(CALL4(s, sink_add_stream, (DWORD)stream_record, v, NULL, &idx))) {
			if (sound && e->audio) {
				void *a = aac_type(2, 48000, 24000);
				if (a) CALL4(s, sink_add_stream, (DWORD)stream_audio, a, NULL, &idx);  // without sound if refused
				asuka_com_release(a);
			}
			ok = SUCCEEDED(CALL0(e->engine, engine_start_record));
		}
	}
	asuka_com_release(v);
	asuka_com_release(dt);
	asuka_com_release(s);
	return ok;
}

static void stop_recording_engine(engine *e) { CALL2(e->engine, engine_stop_record, TRUE, FALSE); }

// ---- JPEGs, with WIC (any thread: COM is started on it) ----

static IWICImagingFactory *wic(void) {
	CoInitializeEx(NULL, asuka_desktop_on_main() ? COINIT_APARTMENTTHREADED : COINIT_MULTITHREADED);
	IWICImagingFactory *f = NULL;
	CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **)&f);
	return f;
}

// write_jpeg writes a WIC image as a JPEG of quality (0 to 1) to path.
static bool write_jpeg(IWICImagingFactory *f, IWICBitmapSource *src, const char *path, double quality) {
	IWICFormatConverter *bgr = NULL;
	IWICStream *st = NULL;
	IWICBitmapEncoder *enc = NULL;
	IWICBitmapFrameEncode *out = NULL;
	IPropertyBag2 *bag = NULL;
	UINT w = 0, h = 0;
	HRESULT hr = IWICBitmapSource_GetSize(src, &w, &h);
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateFormatConverter(f, &bgr);
	if (SUCCEEDED(hr)) hr = IWICFormatConverter_Initialize(bgr, src, &GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, NULL, 0, WICBitmapPaletteTypeCustom);
	wchar_t *wp = asuka_wide(path);
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateStream(f, &st);
	if (SUCCEEDED(hr)) hr = IWICStream_InitializeFromFilename(st, wp, GENERIC_WRITE);
	free(wp);
	if (SUCCEEDED(hr)) hr = IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatJpeg, NULL, &enc);
	if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Initialize(enc, (IStream *)st, WICBitmapEncoderNoCache);
	if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_CreateNewFrame(enc, &out, &bag);
	if (SUCCEEDED(hr) && bag) {
		PROPBAG2 opt;
		memset(&opt, 0, sizeof opt);
		opt.pstrName = L"ImageQuality";
		VARIANT v;
		VariantInit(&v);
		v.vt = VT_R4;
		v.fltVal = (float)(quality > 0 && quality <= 1 ? quality : 0.9);
		IPropertyBag2_Write(bag, 1, &opt, &v);
	}
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Initialize(out, bag);
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetSize(out, w, h);
	WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_SetPixelFormat(out, &fmt);
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_WriteSource(out, (IWICBitmapSource *)bgr, NULL);
	if (SUCCEEDED(hr)) hr = IWICBitmapFrameEncode_Commit(out);
	if (SUCCEEDED(hr)) hr = IWICBitmapEncoder_Commit(enc);
	if (bag) IPropertyBag2_Release(bag);
	if (out) IWICBitmapFrameEncode_Release(out);
	if (enc) IWICBitmapEncoder_Release(enc);
	if (st) IWICStream_Release(st);
	if (bgr) IWICFormatConverter_Release(bgr);
	return SUCCEEDED(hr);
}

// jpeg_pixels writes w×h pixels of BGR as a JPEG.
static bool jpeg_pixels(const char *path, uint8_t *bgr, int w, int h, double quality) {
	IWICImagingFactory *f = wic();
	if (!f) return false;
	IWICBitmap *bmp = NULL;
	bool ok = SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(f, (UINT)w, (UINT)h, &GUID_WICPixelFormat24bppBGR, (UINT)w * 3, (UINT)(w * h * 3), bgr, &bmp)) &&
	          write_jpeg(f, (IWICBitmapSource *)bmp, path, quality);
	if (bmp) IWICBitmap_Release(bmp);
	IWICImagingFactory_Release(f);
	return ok;
}

// rewrite_photo writes the engine's photo at from again to `to`: the
// middle a zoom shows, mirrored if mirror, its long side at most max (0:
// as it is), of quality.
static bool rewrite_photo(const char *from, const char *to, double zoom, bool mirror, int max, double quality) {
	IWICImagingFactory *f = wic();
	if (!f) return false;
	IWICBitmapDecoder *dec = NULL;
	IWICBitmapFrameDecode *frame = NULL;
	IWICBitmapClipper *clip = NULL;
	IWICBitmapFlipRotator *flip = NULL;
	IWICBitmapScaler *scaler = NULL;
	wchar_t *w = asuka_wide(from);
	HRESULT hr = IWICImagingFactory_CreateDecoderFromFilename(f, w, NULL, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
	free(w);
	if (SUCCEEDED(hr)) hr = IWICBitmapDecoder_GetFrame(dec, 0, &frame);
	IWICBitmapSource *src = (IWICBitmapSource *)frame;
	UINT fw = 0, fh = 0;
	if (SUCCEEDED(hr)) hr = IWICBitmapSource_GetSize(src, &fw, &fh);
	camera_rect r = camera_zoom_crop((int)fw, (int)fh, zoom);
	if (SUCCEEDED(hr) && (r.w != (int)fw || r.h != (int)fh)) {
		WICRect wr = {r.x, r.y, r.w, r.h};
		hr = IWICImagingFactory_CreateBitmapClipper(f, &clip);
		if (SUCCEEDED(hr)) hr = IWICBitmapClipper_Initialize(clip, src, &wr);
		if (SUCCEEDED(hr)) src = (IWICBitmapSource *)clip;
	}
	if (SUCCEEDED(hr) && mirror) {
		hr = IWICImagingFactory_CreateBitmapFlipRotator(f, &flip);
		if (SUCCEEDED(hr)) hr = IWICBitmapFlipRotator_Initialize(flip, src, WICBitmapTransformFlipHorizontal);
		if (SUCCEEDED(hr)) src = (IWICBitmapSource *)flip;
	}
	int ow = r.w, oh = r.h;
	camera_fit(r.w, r.h, max, &ow, &oh);
	if (SUCCEEDED(hr) && (ow != r.w || oh != r.h)) {
		hr = IWICImagingFactory_CreateBitmapScaler(f, &scaler);
		if (SUCCEEDED(hr)) hr = IWICBitmapScaler_Initialize(scaler, src, (UINT)ow, (UINT)oh, WICBitmapInterpolationModeFant);
		if (SUCCEEDED(hr)) src = (IWICBitmapSource *)scaler;
	}
	bool ok = SUCCEEDED(hr) && write_jpeg(f, src, to, quality);
	if (scaler) IWICBitmapScaler_Release(scaler);
	if (flip) IWICBitmapFlipRotator_Release(flip);
	if (clip) IWICBitmapClipper_Release(clip);
	if (frame) IWICBitmapFrameDecode_Release(frame);
	if (dec) IWICBitmapDecoder_Release(dec);
	IWICImagingFactory_Release(f);
	return ok;
}

// ---- A view's frames, when it scans or hands frames to frame ----
//
// The preview's frames, in RGB32 (the engine converts what the camera
// gives), from the preview sink's sample callback: the latest drawn by
// the view (the middle a digital zoom shows, mirrored as the preview
// is), the codes read in one every quarter second, and frameRate's
// written as JPEGs, off the main thread. Shared by Media Foundation's
// threads, a reading's, a writing's and the main one: counted references.

typedef struct {
	LONG refs;
	CRITICAL_SECTION mu;  // what follows, to bmp
	unsigned view;
	uint8_t *latest;
	int w, h;
	bool posted, closed, reading;
	int64_t last;
	unsigned kinds;       // what to read (0: nothing it reads was asked for)
	double zoom;          // the digital zoom's (1: none)
	bool mirror;          // drawn mirrored
	bool frame_mirror;    // frame's written mirrored
	camera_frames clock;  // frameRate's
	// The main thread's: the frame drawn.
	HBITMAP bmp;
	void *bits;
	int bw, bh;
} frames;

static void frames_unref(void *p) {
	frames *f = p;
	if (InterlockedDecrement(&f->refs)) return;
	DeleteCriticalSection(&f->mu);
	free(f->latest);
	if (f->bmp) DeleteObject(f->bmp);
	free(f);
}

static unsigned barcode_kinds(const camera_kinds *k) {
	if (!k->n) return ASUKA_BARCODE_ALL;
	unsigned m = 0;
	for (size_t i = 0; i < k->n; i++) m |= asuka_barcode_kind(k->kinds[i]);
	return m;
}

typedef struct view view;
static view *view_of(unsigned id);
static void view_invalidate(view *v);
static void codes_found(view *v, asuka_barcode *codes, size_t n);
static void frame_written(view *v, const char *path, int w, int h, int64_t ms);

// A frame for frame, written off the main thread.
typedef struct {
	frames *f;
	char *path;
	uint8_t *px;  // the part of the frame the zoom shows: BGRX, rows of w*4
	int w, h, max;
	bool mirror;
	int64_t ms;
	int ow, oh;
	bool ok;
} frame_job;

static void frame_main(void *p) {
	frame_job *j = p;
	EnterCriticalSection(&j->f->mu);
	j->f->clock.writing = false;
	bool closed = j->f->closed;
	LeaveCriticalSection(&j->f->mu);
	view *v = closed ? NULL : view_of(j->f->view);
	if (v && j->ok) frame_written(v, j->path, j->ow, j->oh, j->ms);
	frames_unref(j->f);
	free(j->path);
	free(j);
}

static void write_frame(void *p) {
	frame_job *j = p;
	camera_fit(j->w, j->h, j->max, &j->ow, &j->oh);
	uint8_t *bgr = malloc((size_t)j->ow * j->oh * 3);
	camera_resample(j->px, (size_t)j->w * 4, (camera_rect){0, 0, j->w, j->h}, j->mirror, bgr, j->ow, j->oh);
	j->ok = jpeg_pixels(j->path, bgr, j->ow, j->oh, 0.8);
	free(bgr);
	free(j->px);
	asuka_desktop_post(frame_main, j);
}

typedef struct {
	frames *f;
	unsigned kinds;
	uint8_t *gray;
	int w, h;
	asuka_barcode *codes;
	size_t n;
} reading;

static void read_main(void *p) {
	reading *r = p;
	EnterCriticalSection(&r->f->mu);
	bool closed = r->f->closed;
	LeaveCriticalSection(&r->f->mu);
	view *v = closed ? NULL : view_of(r->f->view);
	if (v) codes_found(v, r->codes, r->n);
	asuka_barcode_free(r->codes, r->n);
	frames_unref(r->f);
	free(r);
}

static void read_codes(void *p) {
	reading *r = p;
	r->n = asuka_barcode_decode(r->gray, r->w, r->h, r->w, r->kinds, &r->codes);
	free(r->gray);
	EnterCriticalSection(&r->f->mu);
	r->f->reading = false;
	LeaveCriticalSection(&r->f->mu);
	asuka_desktop_post(read_main, r);
}

static void redraw_main(void *p) {
	frames *f = p;
	EnterCriticalSection(&f->mu);
	f->posted = false;
	bool closed = f->closed;
	LeaveCriticalSection(&f->mu);
	view *v = closed ? NULL : view_of(f->view);
	if (v) view_invalidate(v);
	frames_unref(f);
}

// on_sample takes a frame, on Media Foundation's thread: kept for the
// view to draw, and read for codes if it's time.
static void on_sample(void *p, void *sample, void *unused) {
	frames *f = p;
	void *mb = NULL;
	if (!sample || FAILED(CALL1(sample, sample_to_buffer, &mb)) || !mb) return;
	BYTE *data = NULL;
	DWORD len = 0;
	if (FAILED(CALL3(mb, buffer_lock, &data, NULL, &len))) {
		asuka_com_release(mb);
		return;
	}
	EnterCriticalSection(&f->mu);
	size_t need = (size_t)f->w * f->h * 4;
	if (!f->closed && f->w > 0 && f->h > 0 && len >= need) {
		if (!f->latest) f->latest = malloc(need);
		memcpy(f->latest, data, need);
		if (!f->posted) {
			f->posted = true;
			InterlockedIncrement(&f->refs);
			asuka_desktop_post(redraw_main, f);
		}
		int64_t now = camera_now_ms();
		if (!f->reading && now - f->last >= 250 && f->kinds) {
			f->reading = true;
			f->last = now;
			reading *r = calloc(1, sizeof *r);
			r->f = f;
			r->kinds = f->kinds;
			r->w = f->w, r->h = f->h;
			r->gray = malloc((size_t)f->w * f->h);
			// BGRX's luminance, BT.601's weights in integers.
			for (size_t i = 0, j = 0; j < (size_t)f->w * f->h; i += 4, j++)
				r->gray[j] = (uint8_t)((299u * data[i + 2] + 587u * data[i + 1] + 114u * data[i] + 500) / 1000);
			InterlockedIncrement(&f->refs);
			if (!asuka_desktop_spawn(read_codes, r)) {
				f->reading = false;
				free(r->gray);
				free(r);
				InterlockedDecrement(&f->refs);  // the view's still holds it
			}
		}
		char *path = NULL;
		int64_t ms = 0;
		if (camera_frames_due(&f->clock, now, &path, &ms)) {
			camera_rect r = camera_zoom_crop(f->w, f->h, f->zoom);
			frame_job *j = calloc(1, sizeof *j);
			*j = (frame_job){f, path, malloc((size_t)r.w * r.h * 4), r.w, r.h, f->clock.size > 0 ? f->clock.size : 480, f->frame_mirror, ms, 0, 0, false};
			for (int y = 0; y < r.h; y++) memcpy(j->px + (size_t)y * r.w * 4, data + ((size_t)(r.y + y) * f->w + r.x) * 4, (size_t)r.w * 4);
			InterlockedIncrement(&f->refs);
			if (!asuka_desktop_spawn(write_frame, j)) {
				f->clock.writing = false;
				free(j->px);
				free(j->path);
				free(j);
				InterlockedDecrement(&f->refs);
			}
		}
	}
	LeaveCriticalSection(&f->mu);
	CALL0(mb, buffer_unlock);
	asuka_com_release(mb);
}

// preview_frames starts the preview without drawing it: f gets each
// frame.
static bool preview_frames(engine *e, frames *f) {
	void *s = sink(e, 1, &iid_preview_sink);
	if (!s) return false;
	CALL0(s, sink_remove_all);
	void *dt = device_type(e, stream_preview), *t = dt ? new_media_type() : NULL;
	bool ok = false;
	if (t) {
		UINT64 size = 0;
		get_u64(dt, &mt_frame_size, &size);
		e->width = (int)(size >> 32), e->height = (int)(UINT32)size;
		CALL1(dt, attr_copy_all, t);
		set_guid(t, &mt_subtype, &mf_rgb32);
		set_u32(t, &mt_default_stride, (UINT32)(e->width * 4));
		EnterCriticalSection(&f->mu);
		f->w = e->width, f->h = e->height;
		LeaveCriticalSection(&f->mu);
		DWORD idx;
		if (SUCCEEDED(CALL4(s, sink_add_stream, (DWORD)stream_preview, t, NULL, &idx))) {
			InterlockedIncrement(&f->refs);
			void *cb = asuka_winrt_delegate(&iid_sample_callback, on_sample, f, frames_unref);
			ok = SUCCEEDED(CALL2(s, preview_set_sample_callback, idx, cb)) && SUCCEEDED(CALL0(e->engine, engine_start_preview));
			asuka_com_release(cb);
		}
	}
	asuka_com_release(t);
	asuka_com_release(dt);
	asuka_com_release(s);
	return ok;
}

// paint draws the latest frame filling r, cropped to keep its aspect and
// to the zoom's middle, mirrored as the preview is.
static void frames_paint(frames *f, HDC dc, RECT r) {
	EnterCriticalSection(&f->mu);
	int w = f->w, h = f->h;
	double zoom = f->zoom;
	bool mirror = f->mirror;
	if (w > 0 && h > 0 && f->latest) {
		if (!f->bmp || f->bw != w || f->bh != h) {
			if (f->bmp) DeleteObject(f->bmp);
			BITMAPINFO bi = {0};
			bi.bmiHeader.biSize = sizeof bi.bmiHeader;
			bi.bmiHeader.biWidth = w;
			bi.bmiHeader.biHeight = -h;  // top-down
			bi.bmiHeader.biPlanes = 1;
			bi.bmiHeader.biBitCount = 32;
			f->bmp = CreateDIBSection(NULL, &bi, DIB_RGB_COLORS, &f->bits, NULL, 0);
			f->bw = w, f->bh = h;
		}
		if (f->bmp) memcpy(f->bits, f->latest, (size_t)w * h * 4);
	}
	LeaveCriticalSection(&f->mu);
	int rw = r.right - r.left, rh = r.bottom - r.top;
	if (!f->bmp || rw <= 0 || rh <= 0) {
		FillRect(dc, &r, GetStockObject(BLACK_BRUSH));
		return;
	}
	// The part of the zoom's middle of the view's aspect, centered.
	camera_rect c = camera_zoom_crop(w, h, zoom);
	int sx = c.x, sy = c.y, sw = c.w, sh = c.h;
	double fa = (double)rw / rh, va = (double)sw / sh;
	if (va > fa) {
		int nw = (int)(sh * fa);
		sx += (sw - nw) / 2, sw = nw > 0 ? nw : 1;
	} else {
		int nh = (int)(sw / fa);
		sy += (sh - nh) / 2, sh = nh > 0 ? nh : 1;
	}
	HDC mem = CreateCompatibleDC(dc);
	HGDIOBJ old = SelectObject(mem, f->bmp);
	SetStretchBltMode(dc, HALFTONE);
	SetBrushOrgEx(dc, 0, 0, NULL);
	if (mirror) StretchBlt(dc, 0, 0, rw, rh, mem, sx + sw - 1, sy, -sw, sh, SRCCOPY);
	else StretchBlt(dc, 0, 0, rw, rh, mem, sx, sy, sw, sh, SRCCOPY);
	SelectObject(mem, old);
	DeleteDC(mem);
}

static void frames_close(frames *f) {
	EnterCriticalSection(&f->mu);
	f->closed = true;
	LeaveCriticalSection(&f->mu);
	frames_unref(f);
}

// ---- The view ----

typedef struct sheet sheet;

// A window's context: its view or dialog while it has one (a window
// outlives both: the host destroys it after).
typedef struct {
	view *v;
	sheet *s;
} wnd;

struct view {
	camera_View base;
	camera_View_events events;
	HWND hwnd;
	wnd *w;
	unsigned id;
	sheet *sheet;  // the take dialog's (never the current one), or NULL
	engine *engine;
	char running[48];  // what the engine was made for: facing, mode...
	bool front, video, scan, configuring, previewing, destroyed, failed;
	bool snap;       // the dialog's photo is being taken
	bool user;       // its camera faces the user: shown mirrored
	bool ready_due;  // ready is said once it shows
	// The props.
	double zoom;
	bool pinch, mirror;
	camera_VideoQuality quality;
	camera_frames clock;  // frameRate's and frameSize
	camera_kinds kinds;
	camera_seen seen;
	frames *frames;
	// A pinch on a touchscreen: where it began.
	double gesture_zoom;
	ULONGLONG gesture_span;
	camera_pending *capturing;
	char *photo_path;
	int cap_max;  // what the waiting capture asked for
	double cap_quality;
	camera_pending *rec_reply;
	bool recording;
	char *rec_path;
	int64_t rec_max_bytes, rec_last_size;
	view *next;
};

enum { timer_duration = 1, timer_bytes = 2 };
#define BYTES_EVERY 250

static view *views;
// current is the view on screen capture and record use: the last made
// of those still there.
static view *current;

static view *view_of(unsigned id) {
	for (view *v = views; v; v = v->next)
		if (v->id == id) return v;
	return NULL;
}

static void sheet_failed(sheet *s, const char *msg);
static void sheet_photo(sheet *s, const char *path);
static void configure_later(view *v);

static void view_invalidate(view *v) { InvalidateRect(v->hwnd, NULL, FALSE); }

static void fail(view *v, const char *msg) {
	if (v->destroyed) return;
	if (v->sheet) sheet_failed(v->sheet, msg);
	else camera_View_error(v->events, msg);
}

static void codes_found(view *v, asuka_barcode *codes, size_t n) {
	int64_t now = camera_now_ms();
	for (size_t i = 0; i < n && !v->destroyed; i++) {
		const char *kind = asuka_barcode_name(codes[i].kind);
		if (!camera_kinds_wants(&v->kinds, kind) || !camera_seen_new(&v->seen, kind, codes[i].text, now) || v->sheet) continue;
		camera_Code c = {(char *)kind, codes[i].text};
		camera_View_code(v->events, &c);
	}
}

static void frame_written(view *v, const char *path, int w, int h, int64_t ms) {
	if (v->destroyed || v->sheet) return;
	camera_Frame f = {(char *)path, w, h, ms * 1000000};
	camera_View_frame(v->events, &f);
}

static void remove_file(const char *path) {
	if (!path) return;
	wchar_t *w = asuka_wide(path);
	DeleteFileW(w);
	free(w);
}

// The zoom: the camera's own where it has one, else digital in photo
// mode (the recording's the camera's whole view: 1 in video mode).
static double max_zoom(const view *v) {
	if (v->engine && v->engine->optical) return v->engine->max_zoom;
	return v->video ? 1 : CAMERA_DIGITAL_ZOOM;
}

static double zoom_now(const view *v) { return camera_clamp(v->zoom, 1, max_zoom(v)); }

// digital is the zoom the view crops by (1: the camera's own zoom does it).
static double digital(const view *v) { return v->engine && v->engine->optical ? 1 : zoom_now(v); }

// preview_mirror: the preview's mirrored for a camera facing the user,
// unless its stream is already.
static bool preview_mirror(const view *v) { return v->user && !(v->engine && v->engine->preview_mirrored); }

// sampled: the preview comes as frames (to read codes in, or for frame).
static bool sampled(const view *v) { return v->scan || v->clock.rate > 0; }

// sync_frames gives the frames what the props say.
static void sync_frames(view *v) {
	frames *f = v->frames;
	if (!f) return;
	EnterCriticalSection(&f->mu);
	f->zoom = digital(v);
	f->mirror = preview_mirror(v);
	// frame's: mirrored only with mirror (a camera facing the user), its
	// stream flipped back if it's mirrored already.
	f->frame_mirror = (v->mirror && v->user) != (v->engine && v->engine->preview_mirrored);
	f->clock.rate = v->clock.rate;
	f->clock.size = v->clock.size;
	f->kinds = v->scan ? barcode_kinds(&v->kinds) : 0;
	LeaveCriticalSection(&f->mu);
	view_invalidate(v);
}

static void apply_zoom(view *v) {
	if (!v->engine) return;
	if (v->engine->optical) {
		optical_zoom(v->engine, zoom_now(v));
	} else if (v->frames) {
		sync_frames(v);
	} else if (v->previewing) {
		RECT r;
		GetClientRect(v->hwnd, &r);
		fit_preview(v->engine, r.right, r.bottom, digital(v));
	}
}

// zoom_to zooms to z (within the camera's range); the user's pinch says
// so (zoomChange).
static void zoom_to(view *v, double z, bool user) {
	if (user) z = camera_clamp(z, 1, max_zoom(v));
	if (z == v->zoom) return;
	v->zoom = z;
	apply_zoom(v);
	if (user && !v->sheet) camera_View_zoomChange(v->events, z);
}

// ready says what the camera can do, once it shows.
static void ready(view *v) {
	v->ready_due = false;
	if (v->sheet || v->destroyed) return;
	camera_Info i = {.minZoom = 1, .maxZoom = max_zoom(v)};
	camera_View_ready(v->events, &i);
}

static void close_engine(view *v) {
	if (!v->engine) return;
	if (v->previewing) CALL0(v->engine->engine, engine_stop_preview);
	engine_close(v->engine);
	v->engine = NULL;
	v->previewing = false;
	v->running[0] = 0;
	if (v->frames) {
		frames_close(v->frames);
		v->frames = NULL;
	}
}

static void record_done(view *v, const char *err) {
	bool was = v->recording;
	v->recording = false;
	if (v->hwnd) KillTimer(v->hwnd, timer_duration), KillTimer(v->hwnd, timer_bytes);
	if (!was) return;
	if (err) {
		remove_file(v->rec_path);
		camera_fail(&v->rec_reply, err);
	} else {
		camera_answer(&v->rec_reply, v->rec_path);
	}
	configure_later(v);  // what changed while it recorded
}

static void configure(view *v) {
	if (v->destroyed || v->recording) return;
	char want[48];
	snprintf(want, sizeof want, "%s/%s%s%s%d", v->front ? "front" : "back", v->video ? "video" : "photo", sampled(v) ? "/frames" : "",
	         v->video && v->mirror ? "/mirror" : "", v->video ? (int)v->quality : 0);
	if (v->engine && !strcmp(v->running, want)) return;
	close_engine(v);
	if (capability_denied(L"webcam")) {
		v->failed = true;
		fail(v, "not allowed");
		return;
	}
	UINT32 n;
	device *ds = devices(&n);
	device *d = device_for(ds, n, v->front);
	const char *why = NULL;
	engine *e = d ? engine_new(d, v->video && has_microphone() && !capability_denied(L"microphone"), v->id, &why) : NULL;
	v->user = d && d->panel != panel_back;  // a front panel's, or a webcam's
	free_devices(ds, n);
	if (!e) {
		v->failed = true;
		fail(v, "no camera");
		return;
	}
	v->engine = e;
	strcpy(v->running, want);
	v->failed = false;
	v->ready_due = true;
}

static void configure_main(void *p) {
	view *v = view_of((unsigned)(uintptr_t)p);
	if (!v) return;
	v->configuring = false;
	configure(v);
}

// configure_later configures once for the props set together.
static void configure_later(view *v) {
	if (v->configuring) return;
	v->configuring = true;
	asuka_desktop_post(configure_main, (void *)(uintptr_t)v->id);
}

// A photo rewritten (cropped, mirrored, scaled, of its quality) off the
// main thread.
typedef struct {
	camera_pending *reply;
	char *from, *to;
	double zoom, quality;
	bool mirror;
	int max;
	bool ok;
} rewrite;

static void rewritten(void *p) {
	rewrite *r = p;
	remove_file(r->from);
	if (!r->ok || !r->reply) remove_file(r->to);  // failed, or its task went
	if (r->ok) camera_answer(&r->reply, r->to);
	else camera_fail(&r->reply, "couldn't save the photo");
	free(r->from), free(r->to), free(r);
}

static void rewrite_run(void *p) {
	rewrite *r = p;
	r->to = camera_file(".jpg");
	r->ok = r->to && rewrite_photo(r->from, r->to, r->zoom, r->mirror, r->max, r->quality);
	asuka_desktop_post(rewritten, r);
}

// photo_taken answers capture with the engine's photo, rewritten if it
// must be: the zoom's middle (digital), mirrored (mirror, a camera facing
// the user), at most cap_max, of cap_quality.
static void photo_taken(view *v, char *path) {
	bool mirror = (v->mirror && v->user) != v->engine->photo_mirrored;
	double zoom = digital(v);
	if (!mirror && !(zoom > 1) && v->cap_max <= 0 && !(v->cap_quality > 0)) {
		camera_answer(&v->capturing, path);
		free(path);
		return;
	}
	rewrite *r = calloc(1, sizeof *r);
	*r = (rewrite){NULL, path, NULL, zoom, v->cap_quality > 0 ? v->cap_quality : 0.9, mirror, v->cap_max, false};
	camera_move(&v->capturing, &r->reply);
	if (!asuka_desktop_spawn(rewrite_run, r)) {
		r->ok = false;
		rewritten(r);
	}
}

// event is the engine's event, on the main thread.
static void event(view *v, const GUID *kind, HRESULT status) {
	bool ok = SUCCEEDED(status);
	if (IsEqualGUID(kind, &ev_initialized)) {
		if (!ok) {
			v->failed = true;
			fail(v, status == E_ACCESSDENIED ? "not allowed" : "no camera");
			return;
		}
		set_up(v->engine, v->video, v->quality, v->video && v->mirror && v->user);
		bool started;
		if (sampled(v)) {
			frames *f = calloc(1, sizeof *f);
			f->refs = 1;
			InitializeCriticalSection(&f->mu);
			f->view = v->id;
			f->clock = v->clock;
			v->frames = f;
			sync_frames(v);
			started = preview_frames(v->engine, f);
		} else {
			started = start_preview(v->engine, v->hwnd, preview_mirror(v));
		}
		if (!started) {
			v->failed = true;
			fail(v, "no preview");
		}
	} else if (IsEqualGUID(kind, &ev_preview_started)) {
		v->previewing = ok;
		if (ok) {
			if (v->engine->optical) optical_zoom(v->engine, zoom_now(v));
			if (!v->frames) {
				RECT r;
				GetClientRect(v->hwnd, &r);
				fit_preview(v->engine, r.right, r.bottom, digital(v));
			}
			if (v->ready_due) ready(v);
		}
	} else if (IsEqualGUID(kind, &ev_preview_stopped)) {
		v->previewing = false;
	} else if (IsEqualGUID(kind, &ev_photo_taken)) {
		char *path = v->photo_path;
		v->photo_path = NULL;
		bool snap = v->snap;
		v->snap = false;
		if (!ok) remove_file(path);
		if (snap) {
			sheet_photo(v->sheet, ok ? path : NULL);  // may destroy v
			free(path);
		} else if (ok) {
			photo_taken(v, path);
		} else {
			camera_fail(&v->capturing, "no photo");
			free(path);
		}
	} else if (IsEqualGUID(kind, &ev_record_started)) {
		if (!ok) record_done(v, "couldn't record");
	} else if (IsEqualGUID(kind, &ev_record_stopped)) {
		record_done(v, ok ? NULL : "the recording failed");
	} else if (IsEqualGUID(kind, &ev_error)) {
		camera_fail(&v->capturing, "no photo");
		if (v->snap) {
			v->snap = false;
			sheet_photo(v->sheet, NULL);  // destroys v
			return;
		}
		if (v->recording) record_done(v, "the recording failed");
		if (!v->previewing) {
			v->failed = true;
			fail(v, "the camera stopped");
		}
	}
}

static void engine_event_main(void *p) {
	engine_event *e = p;
	view *v = view_of(e->view);
	if (v && !v->destroyed && v->engine && v->engine->engine == e->engine) event(v, &e->kind, e->status);
	free(e);
}

// photo starts a photo: NULL, or why it can't.
static const char *photo(view *v) {
	if (!v->engine || !v->previewing) return "the camera isn't showing";
	char *path = camera_file(".jpg");
	if (!path) return "no file in the caches for the photo";
	if (!take_photo(v->engine, path)) {
		remove_file(path);
		free(path);
		return "no photo";
	}
	free(v->photo_path);
	v->photo_path = path;
	return NULL;
}

static bool wnd_proc(void *ctx, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, LRESULT *result);

static void stop_recording(view *v) {
	if (v->recording && v->engine) stop_recording_engine(v->engine);
}

// bytes_check reads the recording's file as it grows: it ends before it
// would pass maxBytes (at the rate it's written, or its bitrate's).
static void bytes_check(view *v) {
	if (!v->recording || !v->rec_path) return;
	wchar_t *w = asuka_wide(v->rec_path);
	WIN32_FILE_ATTRIBUTE_DATA a;
	bool got = GetFileAttributesExW(w, GetFileExInfoStandard, &a);
	free(w);
	if (!got) return;
	int64_t size = (int64_t)((uint64_t)a.nFileSizeHigh << 32 | a.nFileSizeLow);
	double rate = (double)(size - v->rec_last_size) * 8 * 1000 / BYTES_EVERY;
	v->rec_last_size = size;
	double known = v->engine ? v->engine->bitrate : 0;
	if (camera_record_full(size, v->rec_max_bytes, rate > known ? rate : known, BYTES_EVERY)) {
		KillTimer(v->hwnd, timer_bytes);
		stop_recording(v);
	}
}

static bool view_proc(view *v, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, LRESULT *result) {
	switch (m) {
	case WM_PAINT:
		if (v->frames) {
			PAINTSTRUCT ps;
			HDC dc = BeginPaint(hwnd, &ps);
			RECT r;
			GetClientRect(hwnd, &r);
			frames_paint(v->frames, dc, r);
			EndPaint(hwnd, &ps);
			*result = 0;
			return true;
		}
		break;
	case WM_SIZE:
		if (v->frames) InvalidateRect(hwnd, NULL, FALSE);
		else if (v->engine && v->previewing) fit_preview(v->engine, LOWORD(lp), HIWORD(lp), digital(v));
		break;
	case WM_ERASEBKGND:
		if (v->frames && v->frames->bmp) {
			*result = 1;  // the frame covers it
			return true;
		} else {
			RECT r;
			GetClientRect(hwnd, &r);
			FillRect((HDC)wp, &r, GetStockObject(BLACK_BRUSH));
			*result = 1;
			return true;
		}
	case WM_MOUSEWHEEL:
		// Ctrl and the wheel zoom (a precision touchpad's pinch comes so
		// too): 10% a notch.
		if (v->pinch && !v->sheet && (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)) {
			zoom_to(v, zoom_now(v) * pow(1.1, GET_WHEEL_DELTA_WPARAM(wp) / (double)WHEEL_DELTA), true);
			*result = 0;
			return true;
		}
		break;
	case WM_GESTURE: {
		// A touchscreen's pinch: the fingers' span, from where it began.
		GESTUREINFO gi;
		memset(&gi, 0, sizeof gi);
		gi.cbSize = sizeof gi;
		if (!v->pinch || v->sheet || !GetGestureInfo((HGESTUREINFO)lp, &gi) || gi.dwID != GID_ZOOM) break;
		if (gi.dwFlags & GF_BEGIN) {
			v->gesture_zoom = zoom_now(v);
			v->gesture_span = gi.ullArguments;
		} else if (v->gesture_span) {
			zoom_to(v, v->gesture_zoom * (double)gi.ullArguments / (double)v->gesture_span, true);
		}
		CloseGestureInfoHandle((HGESTUREINFO)lp);
		*result = 0;
		return true;
	}
	case WM_TIMER:
		if (wp == timer_duration) {  // record's max duration
			KillTimer(hwnd, timer_duration);
			stop_recording(v);
			*result = 0;
			return true;
		}
		if (wp == timer_bytes) {
			bytes_check(v);
			*result = 0;
			return true;
		}
		break;
	}
	return false;
}

static void set_facing(camera_View *self, camera_Facing f) {
	view *v = (view *)self;
	v->front = f == camera_Facing_front;
	configure_later(v);
}
static void set_mode(camera_View *self, camera_Mode m) {
	view *v = (view *)self;
	v->video = m == camera_Mode_video;
	configure_later(v);
}
static void set_scan(camera_View *self, bool on) {
	view *v = (view *)self;
	v->scan = on;
	if (v->frames) sync_frames(v);
	configure_later(v);
}
static void set_codes(camera_View *self, camera_list_string codes) {
	view *v = (view *)self;
	camera_kinds_set(&v->kinds, codes);
	sync_frames(v);
}
static void set_zoom(camera_View *self, double z) { zoom_to((view *)self, z, false); }
static void set_pinch(camera_View *self, bool on) { ((view *)self)->pinch = on; }
// A webcam's torch, flash, exposure bias and focus point aren't a desktop
// app's: these do nothing on Windows.
static void set_torch(camera_View *self, bool on) {}
static void set_flash(camera_View *self, camera_Flash f) {}
static void set_exposure(camera_View *self, double ev) {}
static void set_tap(camera_View *self, bool on) {}
static void set_photo_quality(camera_View *self, camera_PhotoQuality q) {}  // the engine's photos favor nothing
static void set_video_quality(camera_View *self, camera_VideoQuality q) {
	view *v = (view *)self;
	v->quality = q;
	if (v->video) configure_later(v);
}
static void set_mirror(camera_View *self, bool on) {
	view *v = (view *)self;
	v->mirror = on;
	sync_frames(v);
	if (v->video) configure_later(v);  // the stream's mirrored for a recording
}
static void set_frame_rate(camera_View *self, double r) {
	view *v = (view *)self;
	bool was = sampled(v);
	v->clock.rate = camera_clamp(r, 0, CAMERA_MAX_FRAME_RATE);
	if (sampled(v) != was) configure_later(v);
	else sync_frames(v);
}
static void set_frame_size(camera_View *self, int64_t s) {
	view *v = (view *)self;
	v->clock.size = s > 0 ? (int)(s < 8192 ? s : 8192) : 480;
	sync_frames(v);
}

// destroy: the camera stops, and what waits on it is told.
static void destroy(camera_View *self) {
	view *v = (view *)self;
	camera_fail(&v->capturing, "the camera isn't showing");
	if (v->recording) {
		stop_recording(v);
		record_done(v, "the camera isn't showing");
	}
	v->destroyed = true;
	if (current == v) current = NULL;
	for (view **q = &views; *q; q = &(*q)->next)
		if (*q == v) {
			*q = v->next;
			break;
		}
	close_engine(v);
	if (v->w) v->w->v = NULL;
	if (v->hwnd) KillTimer(v->hwnd, timer_duration), KillTimer(v->hwnd, timer_bytes);
	if (!v->sheet) camera_frames_clean(v->id);
	remove_file(v->photo_path);
	free(v->photo_path);
	free(v->rec_path);
	camera_kinds_free(&v->kinds);
	camera_seen_free(&v->seen);
	free(v);
}

static const camera_View_vtable view_vt = {
    .destroy = destroy,
    .set_facing = set_facing,
    .set_mode = set_mode,
    .set_scan = set_scan,
    .set_codes = set_codes,
    .set_zoom = set_zoom,
    .set_pinchToZoom = set_pinch,
    .set_torch = set_torch,
    .set_flash = set_flash,
    .set_exposure = set_exposure,
    .set_tapToFocus = set_tap,
    .set_videoQuality = set_video_quality,
    .set_photoQuality = set_photo_quality,
    .set_mirror = set_mirror,
    .set_frameRate = set_frame_rate,
    .set_frameSize = set_frame_size,
};

static view *new_view(HWND parent, camera_View_events events, sheet *s) {
	static unsigned ids;
	view *v = calloc(1, sizeof *v);
	v->base.vt = &view_vt;
	v->events = events;
	v->id = ++ids;
	v->sheet = s;
	v->front = s != NULL;
	v->zoom = 1;
	v->pinch = true;
	v->quality = camera_VideoQuality_fullHD;
	v->clock = (camera_frames){.view = v->id, .size = 480, .started = camera_now_ms()};
	v->w = calloc(1, sizeof *v->w);
	v->w->v = v;
	v->hwnd = asuka_win_window(0, L"", (parent ? WS_CHILD | WS_VISIBLE : WS_POPUP) | WS_CLIPCHILDREN, 0, 0, 0, 0, parent, wnd_proc, v->w);
	v->base.widget = v->hwnd;
	v->next = views;
	views = v;
	if (!s) current = v;
	configure_later(v);
	return v;
}

camera_View *camera_make_View(camera_View_events events) { return &new_view((HWND)events.parent, events, NULL)->base; }

// ---- The take dialog: a window of the package's over the app's, a live
// preview with Take Photo and Cancel (Enter and Escape), as on a Mac ----

struct sheet {
	camera_pending *reply;
	HWND hwnd, owner, button, cancel;
	HFONT font;
	wnd *w;
	view *view;
	double scale;
};

static sheet *taking;

// sheet_finish closes the dialog and answers: the photo, an error, or
// "" (cancelled; path and err NULL).
static void sheet_finish(sheet *s, const char *path, const char *err) {
	if (!s || taking != s) return;
	taking = NULL;
	s->w->s = NULL;
	destroy(&s->view->base);
	if (s->owner) EnableWindow(s->owner, TRUE);
	DestroyWindow(s->hwnd);
	DeleteObject(s->font);
	if (err) camera_fail(&s->reply, err);
	else camera_answer(&s->reply, path ? path : "");
	free(s);
}

static void sheet_failed(sheet *s, const char *msg) { sheet_finish(s, NULL, msg); }

static void sheet_photo(sheet *s, const char *path) {
	if (path) sheet_finish(s, path, NULL);
	else sheet_finish(s, NULL, "no photo");
}

static int px(sheet *s, double v) { return (int)(v * s->scale + 0.5); }

// layout places the preview above the bar, the buttons in it, from the
// trailing edge.
static void sheet_layout(sheet *s) {
	if (!s->view) return;
	RECT r;
	GetClientRect(s->hwnd, &r);
	int bar = px(s, 52), bw = px(s, 120), bh = px(s, 32), gap = px(s, 12);
	SetWindowPos(s->view->hwnd, NULL, 0, 0, r.right, r.bottom - bar, SWP_NOZORDER | SWP_NOACTIVATE);
	int y = r.bottom - bar + (bar - bh) / 2, x = r.right - gap * 2 - bw;
	SetWindowPos(s->button, NULL, x, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
	SetWindowPos(s->cancel, NULL, x - gap - bw, y, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void sheet_take(sheet *s) {
	if (taking != s || !IsWindowEnabled(s->button) || s->view->snap) return;
	if (photo(s->view)) return;  // not showing yet: try again
	EnableWindow(s->button, FALSE);
	s->view->snap = true;
}

static bool sheet_proc(sheet *s, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, LRESULT *result) {
	switch (m) {
	case WM_COMMAND:
		if ((HWND)lp == s->button) sheet_take(s);
		else if ((HWND)lp == s->cancel) sheet_finish(s, NULL, NULL);
		*result = 0;
		return true;
	case WM_KEYDOWN:
		if (wp == VK_RETURN) {
			sheet_take(s);
			*result = 0;
			return true;
		}
		if (wp == VK_ESCAPE) {
			sheet_finish(s, NULL, NULL);
			*result = 0;
			return true;
		}
		break;
	case WM_SIZE:
		sheet_layout(s);
		break;
	case WM_CLOSE:
		sheet_finish(s, NULL, NULL);
		*result = 0;
		return true;
	}
	return false;
}

static bool wnd_proc(void *ctx, HWND hwnd, UINT m, WPARAM wp, LPARAM lp, LRESULT *result) {
	wnd *w = ctx;
	if (m == WM_NCDESTROY) {
		free(w);
		return false;
	}
	if (w->v) return view_proc(w->v, hwnd, m, wp, lp, result);
	if (w->s) return sheet_proc(w->s, hwnd, m, wp, lp, result);
	return false;
}

// words are the dialog's, in the user's languages as Windows has them.
static const camera_word *words(void) {
	ULONG count = 0, size = 0;
	const char *ls[8];
	size_t n = 0;
	static char bufs[8][LOCALE_NAME_MAX_LENGTH * 2];
	if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, NULL, &size) && size) {
		wchar_t *buf = calloc(size, sizeof *buf);
		if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, buf, &size))
			for (wchar_t *p = buf; *p && n < 8; p += wcslen(p) + 1) {
				char *s = asuka_narrow(p, -1);
				snprintf(bufs[n], sizeof bufs[n], "%s", s);
				free(s);
				ls[n] = bufs[n];
				n++;
			}
		free(buf);
	}
	return camera_words_for(ls, n);
}

static void show_sheet(camera_pending **reply) {
	sheet *s = calloc(1, sizeof *s);
	camera_move(reply, &s->reply);
	s->owner = (HWND)asuka_desktop_window(0);
	UINT dpi = 96;
	UINT(WINAPI * dpi_for)(HWND) = NULL;
	*(FARPROC *)&dpi_for = GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
	if (dpi_for && s->owner) dpi = dpi_for(s->owner);
	s->scale = dpi ? dpi / 96.0 : 1;
	int w = px(s, 640), h = px(s, 532), x = CW_USEDEFAULT, y = CW_USEDEFAULT;
	RECT or;
	if (s->owner && GetWindowRect(s->owner, &or) && or.right > or.left) {
		x = or.left + (or.right - or.left - w) / 2;
		y = or.top + (or.bottom - or.top - h) / 2;
	}
	const camera_word *ws = words();
	wchar_t *title = asuka_wide(ws->take), *cancel = asuka_wide(ws->cancel);
	s->w = calloc(1, sizeof *s->w);
	s->w->s = s;
	taking = s;
	s->hwnd = asuka_win_window(0, title, WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, w, h, s->owner, wnd_proc, s->w);
	s->view = new_view(s->hwnd, (camera_View_events){0}, s);
	LOGFONTW lf = {0};
	lf.lfHeight = -px(s, 14);
	lf.lfWeight = FW_NORMAL;
	lf.lfQuality = CLEARTYPE_QUALITY;
	wcscpy(lf.lfFaceName, L"Segoe UI");
	s->font = CreateFontIndirectW(&lf);
	s->button = CreateWindowExW(0, L"BUTTON", title, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, s->hwnd, (HMENU)1, GetModuleHandleW(NULL), NULL);
	s->cancel = CreateWindowExW(0, L"BUTTON", cancel, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, s->hwnd, (HMENU)2, GetModuleHandleW(NULL), NULL);
	SendMessageW(s->button, WM_SETFONT, (WPARAM)s->font, TRUE);
	SendMessageW(s->cancel, WM_SETFONT, (WPARAM)s->font, TRUE);
	free(title);
	free(cancel);
	sheet_layout(s);
	if (s->owner) EnableWindow(s->owner, FALSE);  // modal over it
	ShowWindow(s->hwnd, SW_SHOW);
	SetFocus(s->hwnd);
}

static void take_cancelled(void *ctx) {
	if (taking) sheet_finish(taking, NULL, NULL);
}

// ---- The camera's calls ----

static void c_take(camera_Camera *self, asuka_call *call) {
	if (taking) sheet_finish(taking, NULL, NULL);  // one at a time
	camera_pending *w = NULL;
	camera_wait(call, camera_Camera_take_done, &w, take_cancelled, NULL);
	if (capability_denied(L"webcam")) {
		camera_fail_declared(&w, camera_CameraError_notAllowed);
		return;
	}
	UINT32 n;
	device *ds = devices(&n);
	free_devices(ds, n);
	if (!n) {
		camera_fail_declared(&w, camera_CameraError_noCamera);
		return;
	}
	show_sheet(&w);
}

static void c_capture(camera_Camera *self, int64_t max_size, double quality, asuka_call *call) {
	camera_pending *w = NULL;
	camera_wait(call, camera_Camera_capture_done, &w, NULL, NULL);
	view *v = current;
	if (!v) {
		camera_fail_declared(&w, camera_CameraError_noView);
		return;
	}
	camera_fail(&v->capturing, "another photo was taken");
	const char *why = photo(v);
	if (why) {
		camera_fail(&w, why);
		return;
	}
	v->cap_max = max_size > 0 ? (int)(max_size < 65536 ? max_size : 65536) : 0;
	v->cap_quality = quality;
	camera_move(&w, &v->capturing);
}

// rec_cancelled: the task that records went; the recording ends.
static void rec_cancelled(void *p) {
	for (view *v = views; v; v = v->next)
		if (v == p) stop_recording(v);
}

static void c_record(camera_Camera *self, bool mute, int64_t max, int64_t max_bytes, asuka_call *call) {
	camera_pending *w = NULL;
	view *v = current;
	camera_wait(call, camera_Camera_record_done, &w, rec_cancelled, v);
	if (!v) {
		camera_fail_declared(&w, camera_CameraError_noView);
		return;
	}
	if (!v->engine || !v->previewing) camera_fail(&w, "the camera isn't showing");
	else if (!v->video) camera_fail(&w, "the camera view isn't in video mode");
	else if (v->recording) camera_fail(&w, "already recording");
	if (!w) return;
	char *path = camera_file(".mp4");
	if (!path) {
		camera_fail(&w, "no file in the caches for the video");
		return;
	}
	if (!start_recording(v->engine, path, !mute)) {
		remove_file(path);
		free(path);
		camera_fail(&w, "couldn't record");
		return;
	}
	free(v->rec_path);
	v->rec_path = path;
	v->recording = true;
	v->rec_max_bytes = max_bytes > 0 ? max_bytes : 0;
	v->rec_last_size = 0;
	camera_move(&w, &v->rec_reply);
	if (max > 0) SetTimer(v->hwnd, timer_duration, (UINT)(max / 1000000 > 0 ? max / 1000000 : 1), NULL);
	if (v->rec_max_bytes) SetTimer(v->hwnd, timer_bytes, BYTES_EVERY, NULL);
}

static void c_stop_recording(camera_Camera *self, asuka_error *err) {
	if (current) stop_recording(current);
}

// focus: a webcam focuses as it does (its own autofocus); none a desktop
// app reaches through Media Foundation focuses at a point.
static void c_focus(camera_Camera *self, double x, double y, asuka_error *err) {}

// Windows never asks a desktop app for the camera: Settings' privacy
// switches (every app's, desktop apps', a policy's) allow it or don't.
static const char *access_now(void) { return capability_denied(L"webcam") ? "denied" : "allowed"; }

static char *c_access(camera_Camera *self, asuka_error *err) { return asuka_strdup(access_now()); }

// requestAccess has nothing to ask: the switches' answer, at once.
static void c_request_access(camera_Camera *self, asuka_call *call) { camera_Camera_requestAccess_done(call, asuka_strdup(access_now())); }

static void c_release(camera_Camera *self) {}  // the one camera lives with the app

static const camera_Camera_vtable camera_vt = {.release = c_release,
                                               .take = c_take,
                                               .capture = c_capture,
                                               .record = c_record,
                                               .stopRecording = c_stop_recording,
                                               .focus = c_focus,
                                               .access = c_access,
                                               .requestAccess = c_request_access};

camera_Camera *camera_make_Camera(asuka_error *err) {
	static camera_Camera cam = {&camera_vt};
	return &cam;
}
