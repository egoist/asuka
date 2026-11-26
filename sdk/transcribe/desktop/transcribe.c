// platform/transcribe on Windows and Linux: no recognizer on the device
// is part of either that apps can count on (Windows' speech recognition is
// the microphone's, and may use Microsoft's servers; Linux has none), so
// check says so and file and listen fail with notAvailable, rather than
// sending the sound to a server. macOS runs the package's Swift.
#ifdef _WIN32
#include "asuka_windows.h"
#endif
#include "transcribe.asuka.h"

#include <stdlib.h>
#include <string.h>

// device_language is the user's language as a BCP 47 tag.
static char *device_language(void) {
#ifdef _WIN32
	wchar_t w[LOCALE_NAME_MAX_LENGTH];
	if (GetUserDefaultLocaleName(w, LOCALE_NAME_MAX_LENGTH) > 0) return asuka_narrow(w, -1);
	return asuka_strdup("en-US");
#else
	const char *lang = getenv("LANG");
	char *s = asuka_strdup(lang && *lang ? lang : "");
	char *dot = strpbrk(s, ".@");
	if (dot) *dot = 0;
	for (char *p = s; *p; p++)
		if (*p == '_') *p = '-';
	return s;
#endif
}

static void check(transcribe_Recognizer *self, const char *language, asuka_call *call) {
	transcribe_Availability a = {.onDevice = false, .language = language && *language ? asuka_strdup(language) : device_language()};
	transcribe_Recognizer_check_done(call, a);
}

static void file(transcribe_Recognizer *self, const char *path, const char *language, asuka_call *call) {
	asuka_error e = {0};
	asuka_error_declared(&e, transcribe_TranscribeError_notAvailable);
	asuka_call_fail(call, &e);
}

// The microphone's recognizers (Windows' speech recognition, Linux has
// none) aren't ones apps can count on to stay on the device: not here.
static void listen(transcribe_Recognizer *self, const char *language, transcribe_fn_Heard heard, asuka_call *call) {
	asuka_error e = {0};
	asuka_error_declared(&e, transcribe_TranscribeError_notAvailable);
	asuka_call_fail(call, &e);
}

static void finish(transcribe_Recognizer *self, asuka_error *err) {}

static const transcribe_Recognizer_vtable vt = {.check = check, .file = file, .listen = listen, .finish = finish};

transcribe_Recognizer *transcribe_make_Recognizer(asuka_error *err) {
	transcribe_Recognizer *r = malloc(sizeof *r);
	r->vt = &vt;
	return r;
}
