// areas.c: the areas the host asks in turn (host.h's area).
#include "host.h"

extern const area lx_area_controls, lx_area_anim, lx_area_split, lx_area_scroll, lx_area_list, lx_area_rich, lx_area_popover, lx_area_shape, lx_area_canvas, lx_area_dnd, lx_area_native, lx_area_video, lx_area_web, lx_area_shader, lx_area_tray, lx_area_hotkey, lx_area_dock, lx_area_notify, lx_area_recent, lx_area_login, lx_area_apps, lx_area_system, lx_area_compose, lx_area_update, lx_area_overlay, lx_area_capture;

const area *const lx_areas[] = {
	&lx_area_split,
	&lx_area_scroll,
	&lx_area_list,
	&lx_area_rich,
	&lx_area_popover,
	&lx_area_overlay,
	&lx_area_shape,
	&lx_area_canvas,
	&lx_area_dnd,
	&lx_area_native,
	&lx_area_video,
	&lx_area_web,
	&lx_area_shader,
	&lx_area_tray,
	&lx_area_hotkey,
	&lx_area_dock,
	&lx_area_notify,
	&lx_area_recent,
	&lx_area_login,
	&lx_area_apps,
	&lx_area_system,
	&lx_area_compose,
	&lx_area_update,
	&lx_area_capture,
	&lx_area_controls,
	&lx_area_anim,
};
const size_t lx_area_count = AK_LEN(lx_areas);
