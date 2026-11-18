// tz.h: named time zones (std/time's Zone.named), from the core's copy of
// the IANA tz database: tzdata_gen.c, which internal/tzgen writes from a
// pinned release, the same on every platform. A zone is the transitions
// zic compiled (-b slim: until its rules last changed), each a minute
// (ak_tz_mins) and a type (ak_tz_tidx; the seconds of one off a minute in
// ak_tz_sec_idx/ak_tz_secs), then its POSIX TZ string ("EST5EDT,M3.2.0,
// M11.1.0") for every instant after them. Names (links too: Asia/Calcutta)
// are sorted by their ASCII lowercase, so a lookup ignores case.
#ifndef AK_STD_TZ_H
#define AK_STD_TZ_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
	int32_t off;    // seconds east of UTC
	uint16_t abbr;  // ak_tz_strs offset: EST, CET, +03
} ak_tz_type;

typedef struct {
	uint16_t trans, ntrans;  // its transitions: ak_tz_mins[trans…]
	uint16_t types;          // its type i is ak_tz_types[ak_tz_ztypes[types + i]]; 0 before the first transition
	uint16_t footer;         // ak_tz_strs offset of its TZ string ("" none)
} ak_tz_zone;

typedef struct {
	uint16_t name;   // ak_tz_strs offset
	uint16_t zone;   // ak_tz_zones index
	uint8_t listed;  // in zone.tab (a place's zone), or UTC: what time.zones() lists
} ak_tz_name;

extern const char ak_tz_version[];
extern const char ak_tz_strs[];
extern const ak_tz_type ak_tz_types[];
extern const uint16_t ak_tz_ztypes[];
extern const int32_t ak_tz_mins[];
extern const uint8_t ak_tz_tidx[];
extern const uint16_t ak_tz_sec_idx[];
extern const uint8_t ak_tz_secs[];
extern const uint16_t ak_tz_nsecs;
extern const ak_tz_zone ak_tz_zones[];
extern const ak_tz_name ak_tz_names[];
extern const uint16_t ak_tz_nnames;

// ak_tz_find is the name s[0:n] (any case), or NULL.
const ak_tz_name *ak_tz_find(const char *s, size_t n);

// A zone's offset at an instant and its abbreviation then (not
// NUL-terminated: a TZ string's names are its pieces).
typedef struct {
	int32_t off;
	const char *abbr;
	uint32_t abbr_len;
} ak_tz_at;

// ak_tz_offset is zone's offset at Unix second t.
ak_tz_at ak_tz_offset(const ak_tz_zone *z, int64_t t);

// ak_tz_wall_offset is the offset that makes wall-clock second w (the
// zone's local time, counted as if it were UTC) an instant, by offset_at
// (a zone's offset at a Unix second): w - offset. A wall time the zone
// repeats (clocks set back) is its earlier instant, the offset before
// the change; one it skips (clocks set forward) is taken at the offset
// before the change too, so it lands as far after the change as it was
// into the gap: 02:30 on a day 02:00 becomes 03:00 is 03:30 (Temporal's
// "compatible", java.time's).
int32_t ak_tz_wall_offset(int64_t w, int32_t (*offset_at)(const void *ctx, int64_t t), const void *ctx);

// ak_tz_local_name writes the device's zone's IANA name into out (cap
// bytes) and returns its length; 0 when the system doesn't say. UTC
// when the zone is made UTC (tests, ak_zone_set_utc). The database's
// spelling when it knows the name.
size_t ak_tz_local_name(char *out, size_t cap);

// ak_tz_local_name_hook, when set, answers for the platform (the web
// host's, from the browser's Intl).
extern size_t (*ak_tz_local_name_hook)(char *out, size_t cap);

#endif
