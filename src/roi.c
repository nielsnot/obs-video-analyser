#include <obs-module.h>
#include <util/platform.h>
#include <util/darray.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include "plugin-macros.generated.h"
#include "obs-convenience.h"
#include "roi.h"
#include "common.h"
#include "util.h"

#ifdef ENABLE_PROFILE
#define PROFILE_START(x) profile_start(x)
#define PROFILE_END(x) profile_end(x)
static const char *prof_render_name = "roi_render";
#else // ENABLE_PROFILE
#define PROFILE_START(x)
#define PROFILE_END(x)
#endif // ! ENABLE_PROFILE

#define ROI_DEFAULT_CM_FLAG (CM_FLAG_ROI | CM_FLAG_RAW_TEXTURE)

// Shapes smaller than this (in captured pixels) are treated as a stray click.
#define MIN_SHAPE_PIXELS 2

#define COLOR_SHAPE 0xFF00FF00   // green
#define COLOR_DRAWING 0xFFFFFFFF // white

static void roi_update(void *, obs_data_t *);
static void roi_surface_cb(void *data, struct cm_surface_data *surface_data);

static inline int min_int(int a, int b)
{
	return a < b ? a : b;
}
static inline int max_int(int a, int b)
{
	return a > b ? a : b;
}
static inline void swap_int(int *a, int *b)
{
	int c = *a;
	*a = *b;
	*b = c;
}
static inline float clamp01(float v)
{
	return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v;
}

static const char *roi_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ROI");
}

static void cb_get_roi(void *data, calldata_t *cd)
{
	calldata_set_ptr(cd, "roi", data);
}

static void *roi_create(obs_data_t *settings, obs_source_t *source)
{
	struct roi_source *src = bzalloc(sizeof(struct roi_source));

	pthread_mutex_init(&src->sources_mutex, NULL);
	pthread_mutex_init(&src->shapes_mutex, NULL);

	src->cm.flags = ROI_DEFAULT_CM_FLAG;
	cm_create(&src->cm, settings, source);
	src->mask.hue_half_width = 180.0f;
	src->mask.sat_max = 1.0f;
	src->mask.luma_max = 1.0f;
	src->cm.mask = &src->mask;
	cm_request(&src->cm, roi_surface_cb, src);

	src->cm.x0 = -1;
	src->cm.x1 = -1;
	src->cm.y0 = -1;
	src->cm.y1 = -1;
	src->x_start = INT_MIN;
	src->y_start = INT_MIN;

	roi_update(src, settings);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void get_roi(out ptr roi)", cb_get_roi, src);
	return src;
}

static void roi_destroy(void *data)
{
	struct roi_source *src = data;

	cm_destroy(&src->cm);

	obs_enter_graphics();
	gs_texture_destroy(src->mask_tex);
	obs_leave_graphics();
	bfree(src->mask_buf);

	da_free(src->shapes);
	da_free(src->sources);
	pthread_mutex_destroy(&src->shapes_mutex);
	pthread_mutex_destroy(&src->sources_mutex);

	bfree(src);
}

/* ---------- shapes: storage ---------- */

static void shapes_load(struct roi_source *src, obs_data_t *settings)
{
	obs_data_array_t *array = obs_data_get_array(settings, "shapes");
	size_t n = obs_data_array_count(array);

	pthread_mutex_lock(&src->shapes_mutex);
	da_resize(src->shapes, 0);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		struct roi_shape s = {
			.type = (int)obs_data_get_int(item, "type") == ROI_SHAPE_ELLIPSE ? ROI_SHAPE_ELLIPSE
											 : ROI_SHAPE_RECT,
			.x0 = clamp01((float)obs_data_get_double(item, "x0")),
			.y0 = clamp01((float)obs_data_get_double(item, "y0")),
			.x1 = clamp01((float)obs_data_get_double(item, "x1")),
			.y1 = clamp01((float)obs_data_get_double(item, "y1")),
		};
		if (s.x1 > s.x0 && s.y1 > s.y0)
			da_push_back(src->shapes, &s);
		obs_data_release(item);
	}
	src->shapes_dirty = true;
	pthread_mutex_unlock(&src->shapes_mutex);

	obs_data_array_release(array);
}

/* Writes the shapes into the source settings so they are saved with the dock. */
static void shapes_store(struct roi_source *src)
{
	obs_data_array_t *array = obs_data_array_create();

	pthread_mutex_lock(&src->shapes_mutex);
	for (size_t i = 0; i < src->shapes.num; i++) {
		const struct roi_shape *s = src->shapes.array + i;
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "type", s->type);
		obs_data_set_double(item, "x0", s->x0);
		obs_data_set_double(item, "y0", s->y0);
		obs_data_set_double(item, "x1", s->x1);
		obs_data_set_double(item, "y1", s->y1);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&src->shapes_mutex);

	obs_data_t *settings = obs_source_get_settings(src->cm.self);
	obs_data_set_array(settings, "shapes", array);
	obs_data_release(settings);
	obs_data_array_release(array);
}

/* ---------- settings ---------- */

static void roi_update(void *data, obs_data_t *settings)
{
	struct roi_source *src = data;
	cm_update(&src->cm, settings);

	src->n_interleave = (int)obs_data_get_int(settings, "interleave");

	shapes_load(src, settings);

	int sat_min = (int)obs_data_get_int(settings, "qual_sat_min");
	int sat_max = (int)obs_data_get_int(settings, "qual_sat_max");
	int luma_min = (int)obs_data_get_int(settings, "qual_luma_min");
	int luma_max = (int)obs_data_get_int(settings, "qual_luma_max");
	if (sat_min > sat_max)
		swap_int(&sat_min, &sat_max);
	if (luma_min > luma_max)
		swap_int(&luma_min, &luma_max);

	src->mask.hue_center = (float)obs_data_get_int(settings, "qual_hue_center");
	src->mask.hue_half_width = (float)obs_data_get_int(settings, "qual_hue_width") * 0.5f;
	src->mask.sat_min = sat_min / 100.0f;
	src->mask.sat_max = sat_max / 100.0f;
	src->mask.luma_min = luma_min / 100.0f;
	src->mask.luma_max = luma_max / 100.0f;
	src->mask.qualifier = obs_data_get_bool(settings, "qual_enable");
}

static void roi_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "interleave", 1);
	obs_data_set_default_bool(settings, "qual_enable", false);
	obs_data_set_default_int(settings, "qual_hue_center", 30);
	obs_data_set_default_int(settings, "qual_hue_width", 360);
	obs_data_set_default_int(settings, "qual_sat_min", 0);
	obs_data_set_default_int(settings, "qual_sat_max", 100);
	obs_data_set_default_int(settings, "qual_luma_min", 0);
	obs_data_set_default_int(settings, "qual_luma_max", 100);
}

static obs_properties_t *roi_get_properties(void *data)
{
	struct roi_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);

	obs_properties_add_int(props, "interleave", obs_module_text("Interleave"), 0, 1, 1);
	properties_add_colorspace(props, "colorspace", obs_module_text("Color space"));

	obs_properties_t *qp = obs_properties_create();
	obs_properties_add_int_slider(qp, "qual_hue_center", obs_module_text("Qualifier.HueCenter"), 0, 360, 1);
	obs_properties_add_int_slider(qp, "qual_hue_width", obs_module_text("Qualifier.HueWidth"), 1, 360, 1);
	obs_properties_add_int_slider(qp, "qual_sat_min", obs_module_text("Qualifier.SatMin"), 0, 100, 1);
	obs_properties_add_int_slider(qp, "qual_sat_max", obs_module_text("Qualifier.SatMax"), 0, 100, 1);
	obs_properties_add_int_slider(qp, "qual_luma_min", obs_module_text("Qualifier.LumaMin"), 0, 100, 1);
	obs_properties_add_int_slider(qp, "qual_luma_max", obs_module_text("Qualifier.LumaMax"), 0, 100, 1);
	obs_properties_add_group(props, "qual_enable", obs_module_text("Qualifier"), OBS_GROUP_CHECKABLE, qp);

	return props;
}

static inline uint32_t roi_get_width(const struct roi_source *src)
{
	return src->cm.texrender_width;
}

static inline uint32_t roi_get_height(const struct roi_source *src)
{
	return src->cm.texrender_height;
}

static uint32_t roi_get_width_1(void *data)
{
	return roi_get_width(data);
}

static uint32_t roi_get_height_1(void *data)
{
	return roi_get_height(data);
}

/* ---------- shapes: mask texture (graphics thread) ---------- */

static inline bool shape_contains(const struct roi_shape *s, float u, float v)
{
	if (u < s->x0 || u > s->x1 || v < s->y0 || v > s->y1)
		return false;
	if (s->type == ROI_SHAPE_ELLIPSE) {
		const float cx = (s->x0 + s->x1) * 0.5f;
		const float cy = (s->y0 + s->y1) * 0.5f;
		const float rx = (s->x1 - s->x0) * 0.5f;
		const float ry = (s->y1 - s->y0) * 0.5f;
		if (rx <= 0.0f || ry <= 0.0f)
			return false;
		const float du = (u - cx) / rx;
		const float dv = (v - cy) / ry;
		return du * du + dv * dv <= 1.0f;
	}
	return true;
}

static void rasterize_shape(uint8_t *buf, uint32_t w, uint32_t h, const struct roi_shape *s)
{
	int px0 = max_int((int)floorf(s->x0 * w), 0);
	int py0 = max_int((int)floorf(s->y0 * h), 0);
	int px1 = min_int((int)ceilf(s->x1 * w), (int)w);
	int py1 = min_int((int)ceilf(s->y1 * h), (int)h);
	for (int y = py0; y < py1; y++) {
		const float v = (y + 0.5f) / h;
		uint8_t *row = buf + (size_t)y * w;
		for (int x = px0; x < px1; x++) {
			if (shape_contains(s, (x + 0.5f) / w, v))
				row[x] = 255;
		}
	}
}

static void update_mask_texture(struct roi_source *src)
{
	const uint32_t w = roi_get_width(src);
	const uint32_t h = roi_get_height(src);
	if (!w || !h)
		return;

	const bool resized = w != src->mask_w || h != src->mask_h;
	if (!src->shapes_dirty && !resized)
		return;

	pthread_mutex_lock(&src->shapes_mutex);
	src->shapes_dirty = false;
	const size_t n = src->shapes.num;
	if (n) {
		if (resized || !src->mask_buf) {
			bfree(src->mask_buf);
			src->mask_buf = bmalloc((size_t)w * h);
		}
		memset(src->mask_buf, 0, (size_t)w * h);
		for (size_t i = 0; i < n; i++)
			rasterize_shape(src->mask_buf, w, h, src->shapes.array + i);
	}
	pthread_mutex_unlock(&src->shapes_mutex);

	if (!n) {
		src->mask.tex = NULL;
		return;
	}

	if (src->mask_tex && !resized) {
		gs_texture_set_image(src->mask_tex, src->mask_buf, w, false);
	} else {
		gs_texture_destroy(src->mask_tex);
		const uint8_t *data = src->mask_buf;
		src->mask_tex = gs_texture_create(w, h, GS_R8, 1, &data, GS_DYNAMIC);
	}
	src->mask_w = w;
	src->mask_h = h;
	src->mask.tex = src->mask_tex;
}

/* ---------- drawing outlines ---------- */

static void draw_rect_outline(float x0, float y0, float x1, float y1)
{
	gs_vertex2f(x0, y0);
	gs_vertex2f(x1, y0);
	gs_vertex2f(x1, y0);
	gs_vertex2f(x1, y1);
	gs_vertex2f(x1, y1);
	gs_vertex2f(x0, y1);
	gs_vertex2f(x0, y1);
	gs_vertex2f(x0, y0);
}

static void draw_ellipse_outline(float x0, float y0, float x1, float y1)
{
	const int n_seg = 72;
	const float cx = (x0 + x1) * 0.5f;
	const float cy = (y0 + y1) * 0.5f;
	const float rx = (x1 - x0) * 0.5f;
	const float ry = (y1 - y0) * 0.5f;
	float px = cx + rx, py = cy;
	for (int i = 1; i <= n_seg; i++) {
		const float t = (float)i * 2.0f * (float)M_PI / (float)n_seg;
		const float qx = cx + rx * cosf(t);
		const float qy = cy + ry * sinf(t);
		gs_vertex2f(px, py);
		gs_vertex2f(qx, qy);
		px = qx;
		py = qy;
	}
}

static void draw_shape_outline(int type, float x0, float y0, float x1, float y1, uint32_t color)
{
	if (x1 <= x0 || y1 <= y0)
		return;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), color);
	while (gs_effect_loop(effect, "Solid")) {
		gs_render_start(false);
		if (type == ROI_SHAPE_ELLIPSE)
			draw_ellipse_outline(x0, y0, x1, y1);
		else
			draw_rect_outline(x0, y0, x1, y1);
		gs_render_stop(GS_LINES);
	}
}

/* Box of the shape being drawn, in captured pixels; Shift keeps it square (a circle). */
static void drag_box(const struct roi_source *src, int *x0, int *y0, int *x1, int *y1)
{
	int xm = src->x_mouse;
	int ym = src->y_mouse;
	if (src->shift_down) {
		const int dx = xm - src->x_start;
		const int dy = ym - src->y_start;
		const int m = max_int(abs(dx), abs(dy));
		xm = src->x_start + (dx < 0 ? -m : m);
		ym = src->y_start + (dy < 0 ? -m : m);
	}
	*x0 = min_int(src->x_start, xm);
	*y0 = min_int(src->y_start, ym);
	*x1 = max_int(src->x_start, xm);
	*y1 = max_int(src->y_start, ym);
}

static inline int draw_mode_shape(int mode)
{
	return mode == ROI_DRAW_ELLIPSE ? ROI_SHAPE_ELLIPSE : ROI_SHAPE_RECT;
}

/* ---------- rendering ---------- */

bool roi_target_render(struct roi_source *src)
{
	src->interleave_rendered = true;
	if (src->i_interleave != 0 && src->n_interleave > 0)
		return true;

	update_mask_texture(src);
	cm_render_target(&src->cm);

	if (src->n_interleave <= 0)
		return true;
	return false;
}

static void roi_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct roi_source *src = data;

	PROFILE_START(prof_render_name);

	roi_target_render(src);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	cm_render_target(&src->cm);
	update_mask_texture(src);

	const float w = (float)roi_get_width(src);
	const float h = (float)roi_get_height(src);

	gs_texture_t *tex = gs_texrender_get_texture(src->cm.texrender);
	if (tex && src->cm.effect) {
		// Darkens everything outside the shapes and the qualifier.
		gs_effect_t *dim = src->cm.effect;
		cm_set_mask_params(dim, &src->mask);
		gs_effect_set_texture(gs_effect_get_param_by_name(dim, "image"), tex);
		while (gs_effect_loop(dim, "DrawDimmed")) {
			gs_draw_sprite(tex, 0, src->cm.texrender_width, src->cm.texrender_height);
		}
	} else if (tex) {
		gs_effect_t *def = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), tex);
		while (gs_effect_loop(def, "Draw")) {
			gs_draw_sprite(tex, 0, src->cm.texrender_width, src->cm.texrender_height);
		}
	}

	pthread_mutex_lock(&src->shapes_mutex);
	for (size_t i = 0; i < src->shapes.num; i++) {
		const struct roi_shape *s = src->shapes.array + i;
		draw_shape_outline(s->type, s->x0 * w, s->y0 * h, s->x1 * w, s->y1 * h, COLOR_SHAPE);
	}
	pthread_mutex_unlock(&src->shapes_mutex);

	if (src->dragging) {
		int x0, y0, x1, y1;
		drag_box(src, &x0, &y0, &x1, &y1);
		draw_shape_outline(draw_mode_shape(src->draw_mode), (float)x0, (float)y0, (float)x1, (float)y1,
				   COLOR_DRAWING);
	}

	gs_blend_state_pop();

	PROFILE_END(prof_render_name);
}

void roi_register_source(struct roi_source *src, struct cm_source *cm)
{
	pthread_mutex_lock(&src->sources_mutex);
	da_push_back(src->sources, &cm);
	pthread_mutex_unlock(&src->sources_mutex);
}

void roi_unregister_source(struct roi_source *src, struct cm_source *cm)
{
	pthread_mutex_lock(&src->sources_mutex);
	da_erase_item(src->sources, &cm);
	pthread_mutex_unlock(&src->sources_mutex);
}

static void roi_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct roi_source *src = data;

	pthread_mutex_lock(&src->sources_mutex);
	for (size_t i = 0; i < src->sources.num; i++) {
		struct cm_source *cm = src->sources.array[i];
		if (cm->callback) {
			cm->callback(cm->callback_data, surface_data);
		}
	}
	pthread_mutex_unlock(&src->sources_mutex);
}

/* ---------- mouse ---------- */

static void roi_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct roi_source *src = data;
	if (mouse_leave || !event) {
		src->shift_down = false;
		return;
	}

	src->shift_down = !!(event->modifiers & INTERACT_SHIFT_KEY);
	src->x_mouse = event->x;
	src->y_mouse = event->y;
}

static void finish_drag(struct roi_source *src)
{
	src->dragging = false;

	const int w = (int)roi_get_width(src);
	const int h = (int)roi_get_height(src);
	if (w <= 0 || h <= 0)
		return;

	int x0, y0, x1, y1;
	drag_box(src, &x0, &y0, &x1, &y1);
	if (x1 - x0 < MIN_SHAPE_PIXELS || y1 - y0 < MIN_SHAPE_PIXELS)
		return;

	struct roi_shape s = {
		.type = draw_mode_shape(src->draw_mode),
		.x0 = clamp01((float)x0 / w),
		.y0 = clamp01((float)y0 / h),
		.x1 = clamp01((float)x1 / w),
		.y1 = clamp01((float)y1 / h),
	};
	if (s.x1 <= s.x0 || s.y1 <= s.y0)
		return;

	pthread_mutex_lock(&src->shapes_mutex);
	da_push_back(src->shapes, &s);
	src->shapes_dirty = true;
	pthread_mutex_unlock(&src->shapes_mutex);

	shapes_store(src);
}

static void roi_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			    uint32_t click_count)
{
	struct roi_source *src = data;

	UNUSED_PARAMETER(click_count);

	if (type != MOUSE_LEFT)
		return;

	src->shift_down = !!(event->modifiers & INTERACT_SHIFT_KEY);
	src->x_mouse = event->x;
	src->y_mouse = event->y;

	if (!mouse_up) {
		// Only draw while a shape button in the toolbar is active.
		if (src->draw_mode == ROI_DRAW_NONE)
			return;
		src->x_start = event->x;
		src->y_start = event->y;
		src->dragging = true;
		return;
	}

	if (src->dragging)
		finish_drag(src);
}

/* ---------- toolbar API ---------- */

void roi_set_draw_mode(struct roi_source *src, int mode)
{
	if (!src)
		return;
	if (mode != ROI_DRAW_RECT && mode != ROI_DRAW_ELLIPSE)
		mode = ROI_DRAW_NONE;
	src->draw_mode = mode;
	if (mode == ROI_DRAW_NONE)
		src->dragging = false;
}

int roi_get_draw_mode(const struct roi_source *src)
{
	return src ? src->draw_mode : ROI_DRAW_NONE;
}

void roi_undo_shape(struct roi_source *src)
{
	if (!src)
		return;
	pthread_mutex_lock(&src->shapes_mutex);
	if (src->shapes.num)
		da_pop_back(src->shapes);
	src->shapes_dirty = true;
	pthread_mutex_unlock(&src->shapes_mutex);
	shapes_store(src);
}

void roi_clear_shapes(struct roi_source *src)
{
	if (!src)
		return;
	pthread_mutex_lock(&src->shapes_mutex);
	da_resize(src->shapes, 0);
	src->shapes_dirty = true;
	pthread_mutex_unlock(&src->shapes_mutex);
	src->dragging = false;
	shapes_store(src);
}

size_t roi_get_shape_count(struct roi_source *src)
{
	if (!src)
		return 0;
	pthread_mutex_lock(&src->shapes_mutex);
	size_t n = src->shapes.num;
	pthread_mutex_unlock(&src->shapes_mutex);
	return n;
}

/* ---------- tick ---------- */

/* Crops the analysis to the bounding box of all shapes; the mask texture does the rest. */
static void roi_send_range(struct roi_source *src)
{
	const int w = (int)roi_get_width(src);
	const int h = (int)roi_get_height(src);

	int x0 = 0, y0 = 0, x1 = w, y1 = h;

	pthread_mutex_lock(&src->shapes_mutex);
	if (src->shapes.num && w > 0 && h > 0) {
		float bx0 = 1.0f, by0 = 1.0f, bx1 = 0.0f, by1 = 0.0f;
		for (size_t i = 0; i < src->shapes.num; i++) {
			const struct roi_shape *s = src->shapes.array + i;
			bx0 = fminf(bx0, s->x0);
			by0 = fminf(by0, s->y0);
			bx1 = fmaxf(bx1, s->x1);
			by1 = fmaxf(by1, s->y1);
		}
		x0 = max_int((int)floorf(bx0 * w), 0);
		y0 = max_int((int)floorf(by0 * h), 0);
		x1 = min_int((int)ceilf(bx1 * w), w);
		y1 = min_int((int)ceilf(by1 * h), h);
		if (x1 <= x0 || y1 <= y0) {
			x0 = y0 = 0;
			x1 = w;
			y1 = h;
		}
	}
	pthread_mutex_unlock(&src->shapes_mutex);

	src->cm.x0 = x0;
	src->cm.y0 = y0;
	src->cm.x1 = x1;
	src->cm.y1 = y1;
}

static void roi_tick(void *data, float unused)
{
	struct roi_source *src = data;

	if (src->interleave_rendered && src->i_interleave++ >= src->n_interleave)
		src->i_interleave = 0;
	src->interleave_rendered = false;

	if (src->i_interleave == 0 || src->n_interleave <= 0)
		cm_tick(data, unused);

	src->cm.flags = ROI_DEFAULT_CM_FLAG;
	pthread_mutex_lock(&src->sources_mutex);
	for (size_t i = 0; i < src->sources.num; i++) {
		struct cm_source *cm = src->sources.array[i];
		src->cm.flags |= cm->flags & (CM_FLAG_CONVERT_RGB | CM_FLAG_CONVERT_YUV);
	}
	pthread_mutex_unlock(&src->sources_mutex);

	roi_send_range(src);
}

struct roi_source *roi_from_source(obs_source_t *s)
{
	proc_handler_t *ph = obs_source_get_proc_handler(s);
	if (!ph)
		return NULL;

	struct roi_source *ret = NULL;

	calldata_t cd = {0};
	uint8_t stack[128];
	calldata_init_fixed(&cd, stack, sizeof(stack));
	proc_handler_call(ph, "get_roi", &cd);
	calldata_get_ptr(&cd, "roi", &ret);

	return ret;
}

bool is_roi_source_name(const char *name)
{
	obs_source_t *src = obs_get_source_by_name(name);
	if (!src)
		return false;
	struct roi_source *roi = roi_from_source(src);
	obs_source_release(src);
	return !!roi;
}

const struct obs_source_info colormonitor_roi = {
	.id = ID_PREFIX "roi",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags =
#ifndef SHOW_ROI
		OBS_SOURCE_CAP_DISABLED |
#endif
		OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION,
	.get_name = roi_get_name,
	.create = roi_create,
	.destroy = roi_destroy,
	.update = roi_update,
	.get_defaults = roi_get_defaults,
	.get_properties = roi_get_properties,
	.get_width = roi_get_width_1,
	.get_height = roi_get_height_1,
	.enum_active_sources = cm_enum_sources,
	.video_render = roi_render,
	.video_tick = roi_tick,
	.mouse_move = roi_mouse_move,
	.mouse_click = roi_mouse_click,
};
