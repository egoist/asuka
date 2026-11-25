// Stand-ins for the core's functions, for the host's package alone (swift
// test): weak, so an app's executable, which links the core, takes the
// core's. The Asuka CLI doesn't build this file.
#include <stdlib.h>
#include "asuka.h"

#define WEAK __attribute__((weak))

WEAK void asuka_start(asuka_host_callbacks *callbacks) { (void)callbacks; }
WEAK int asuka_program_start(int argc, char **argv, asuka_host_callbacks *callbacks) {
	(void)argc;
	(void)argv;
	(void)callbacks;
	return 1;
}
WEAK void asuka_unload(void) {}
WEAK int asuka_load(const uint8_t *module, size_t len, char **err) { (void)module; (void)len; (void)err; return -1; }
WEAK int asuka_run(char **err) { (void)err; return -1; }
WEAK int asuka_has_app(void) { return 0; }
WEAK int asuka_dev_connect(const char *host, int port, const char *hello, char **err) { (void)host; (void)port; (void)hello; (void)err; return -1; }
WEAK int asuka_dev_listen(int port, const char *hello, char **err) { (void)port; (void)hello; (void)err; return -1; }
WEAK int asuka_dev_reload(const uint8_t *module, size_t len, uint32_t *micros, char **err) { (void)module; (void)len; (void)micros; (void)err; return -1; }
WEAK int asuka_dev_restart(const uint8_t *module, size_t len, uint32_t *micros, char **err) { (void)module; (void)len; (void)micros; (void)err; return -1; }
WEAK void *asuka_alloc(size_t n) { return malloc(n); }
WEAK void asuka_free(void *p) { free(p); }
WEAK void asuka_free_string(char *s) { free(s); }
WEAK void asuka_attach(void) {}
WEAK void asuka_event(const uint8_t *buf, size_t len) { (void)buf; (void)len; }
WEAK int64_t asuka_list_count(uint32_t list) { (void)list; return 0; }
WEAK void asuka_list_bind(uint32_t list, int64_t index, uint32_t cell) { (void)list; (void)index; (void)cell; }
WEAK void asuka_list_unbind(uint32_t list, uint32_t cell) { (void)list; (void)cell; }
WEAK void asuka_run_posted(void) {}
WEAK int asuka_quit_request(int ending) { (void)ending; return 0; }
WEAK void asuka_setenv(const char *key, const char *value) { (void)key; (void)value; }
WEAK void asuka_set_app_dir(const char *path) { (void)path; }
WEAK void asuka_set_dir(int kind, const char *path) { (void)kind, (void)path; }
WEAK void asuka_set_start(const char *href) { (void)href; }
WEAK char *asuka_crash_start(const char *info) { (void)info; return NULL; }
WEAK void asuka_crash_notify(void) {}
WEAK void asuka_native_done(uint64_t call, const uint8_t *p, size_t len) { (void)call; (void)p; (void)len; }
WEAK size_t asuka_native_invoke(uint64_t id, const uint8_t *p, size_t len, uint8_t **out) { (void)id; (void)p; (void)len; *out = NULL; return 0; }
