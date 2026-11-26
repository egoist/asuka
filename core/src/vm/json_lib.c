// The VM's JSON builtins: json.decode(text, desc) and json.encode(value,
// desc, indent), with shapes found in the module that's running.
#include "vm/vm_int.h"
#include "rt/json.h"

static ak_module *running(void) { return ak_vm_running_module(); }

static const ak_shape *shape(void *ctx, const char *name, size_t n) {
	ak_module *m = ctx;
	for (uint32_t i = 0; m && i < m->nshapes; i++) {
		const ak_shape *sh = AK_MOD_SHAPE(m, i);
		if (strlen(sh->name) == n && memcmp(sh->name, name, n) == 0) return sh;
	}
	return NULL;
}

// Payloadless enums are ints; their case names come as a shape of the
// same name whose cases have no payloads (the compiler writes one for
// each enum JSON may read or write).
static bool cases(void *ctx, const char *name, size_t n, const char *const **out, uint32_t *count) {
	const ak_shape *s = shape(ctx, name, n);
	if (!s || !s->ncases) return false;
	ak_vm_shape *vs = (ak_vm_shape *)s;
	if (!vs->case_names) {
		vs->case_names = ak_alloc(sizeof(char *) * s->ncases);
		for (uint16_t i = 0; i < s->ncases; i++) vs->case_names[i] = s->cases[i].name;
	}
	*out = vs->case_names;
	*count = s->ncases;
	return true;
}

static ak_val decode(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *text = args[0].p, *desc = args[1].p;
	ak_json_types t = {shape, cases, running()};
	return ak_json_decode(text->data, text->len, desc->data, &t);
}

static ak_val encode(ak_val *args, uint32_t n) {
	ak_str *desc = args[1].p;
	ak_json_types t = {shape, cases, running()};
	return (ak_val){.p = ak_json_encode(args[0], desc->data, &t, n > 2 ? (int)args[2].i : 0)};
}

// json.value(value, desc, vdesc): value (of desc's type) as std/json's
// Value (of vdesc, J<name>;): what it's written as, read as any JSON,
// made directly (ak_json_value). A value converts to one where a Value
// is expected (log's fields, std/template's data).
static ak_val to_value(ak_val *args, uint32_t n) {
	AK_UNUSED(n);
	ak_str *desc = args[1].p, *vdesc = args[2].p;
	ak_json_types t = {shape, cases, running()};
	return ak_json_value(args[0], desc->data, vdesc->data, &t);
}

// ak_vm_json_types is what decoding by descriptor needs of the running
// module (std/sqlite's rows).
void ak_vm_json_types(ak_json_types *out) { *out = (ak_json_types){shape, cases, running()}; }

// ak_vm_module_json_types is likewise m's, whatever is running (asuka
// test's generated values, made before a test's function runs).
void ak_vm_module_json_types(ak_module *m, ak_json_types *out) { *out = (ak_json_types){shape, cases, m}; }

void ak_vm_json_init(void) {
	ak_builtin_register("json.decode", decode);
	ak_builtin_register("json.encode", encode);
	ak_builtin_register("json.value", to_value);
}
