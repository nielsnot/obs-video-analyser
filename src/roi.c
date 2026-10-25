#include <obs-module.h>
#include <util/platform.h>
#include <util/darray.h>
#include <graphics/matrix4.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include "plugin-macros.generated.h"
#include "obs-convenience.h"
#include "roi.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

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

static void roi_snapshot_cb(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	((struct roi_source *)data)->want_snapshot = true;
}

static void roi_compare_cb(void *data, calldata_t *cd)
{
	((struct roi_source *)data)->compare = calldata_bool(cd, "on");
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
	src->zoom = 1.0f;

	roi_update(src, settings);

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void get_roi(out ptr roi)", cb_get_roi, src);
	proc_handler_add(ph, "void va_snapshot()", roi_snapshot_cb, src);
	proc_handler_add(ph, "void va_set_compare(in bool on)", roi_compare_cb, src);
	return src;
}

static void roi_destroy(void *data)
{
	struct roi_source *src = data;

	cm_destroy(&src->cm);

	obs_enter_graphics();
	gs_texture_destroy(src->mask_tex);
	gs_texture_destroy(src->ref_tex);
	obs_leave_graphics();
	bfree(src->mask_buf);

	da_free(src->shapes);
	da_free(src->pins);
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

	obs_data_array_t *pins = obs_data_get_array(settings, "pins");
	const size_t np = obs_data_array_count(pins);
	da_resize(src->pins, 0);
	for (size_t i = 0; i < np && src->pins.num < ROI_MAX_PINS; i++) {
		obs_data_t *item = obs_data_array_item(pins, i);
		struct roi_pin pin = {
			.x = clamp01((float)obs_data_get_double(item, "x")),
			.y = clamp01((float)obs_data_get_double(item, "y")),
		};
		da_push_back(src->pins, &pin);
		obs_data_release(item);
	}
	obs_data_array_release(pins);
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

	obs_data_array_t *pins = obs_data_array_create();
	pthread_mutex_lock(&src->shapes_mutex);
	for (size_t i = 0; i < src->pins.num; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_double(item, "x", src->pins.array[i].x);
		obs_data_set_double(item, "y", src->pins.array[i].y);
		obs_data_array_push_back(pins, item);
		obs_data_release(item);
	}
	pthread_mutex_unlock(&src->shapes_mutex);

	obs_data_t *settings = obs_source_get_settings(src->cm.self);
	obs_data_set_array(settings, "shapes", array);
	obs_data_set_array(settings, "pins", pins);
	obs_data_array_release(pins);
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

/* ---------- pins ---------- */

static void rgb_to_hsl(int r, int g, int b, int *h, int *s, int *l)
{
	const int mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
	const int mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
	const int c = mx - mn;
	*l = (mx + mn) * 100 / 510;
	const int denom = mx + mn <= 255 ? mx + mn : 510 - mx - mn;
	*s = denom > 0 ? c * 100 / denom : 0;
	if (!c) {
		*h = 0;
		return;
	}
	float hf;
	if (mx == r)
		hf = (float)(g - b) / c;
	else if (mx == g)
		hf = (float)(b - r) / c + 2.0f;
	else
		hf = (float)(r - g) / c + 4.0f;
	if (hf < 0.0f)
		hf += 6.0f;
	*h = (int)(hf * 60.0f) % 360;
}

static void fill_quad(float x0, float y0, float x1, float y1, uint32_t color)
{
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		gs_vertex2f(x0, y0);
		gs_vertex2f(x1, y0);
		gs_vertex2f(x0, y1);
		gs_vertex2f(x1, y1);
		gs_render_stop(GS_TRISTRIP);
	}
}

/* Each pin: a cross, a swatch of the color read and its values (RGB, luma %, HSL). */
static void draw_pins(struct roi_source *src, float w, float h, float zoom, float view_x, float view_y)
{
	struct roi_pin pins[ROI_MAX_PINS];
	pthread_mutex_lock(&src->shapes_mutex);
	const size_t n = src->pins.num < ROI_MAX_PINS ? src->pins.num : ROI_MAX_PINS;
	memcpy(pins, src->pins.array, n * sizeof(struct roi_pin));
	pthread_mutex_unlock(&src->shapes_mutex);
	if (!n)
		return;

	const struct text_scale ts = text_scale_from_viewport(w, h);
	const float px = 12.0f;
	const float arm = 7.0f;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	for (size_t i = 0; i < n; i++) {
		const float x = (pins[i].x * w - view_x) * zoom, y = (pins[i].y * h - view_y) * zoom;
		if (x < 0.0f || y < 0.0f || x > w || y > h)
			continue; // zoomed out of view

		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFFFFFFFF);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			gs_vertex2f(x - arm * ts.ux, y);
			gs_vertex2f(x + arm * ts.ux, y);
			gs_vertex2f(x, y - arm * ts.uy);
			gs_vertex2f(x, y + arm * ts.uy);
			gs_render_stop(GS_LINES);
		}

		char line1[64], line2[64];
		if (pins[i].valid) {
			const int r = pins[i].r, g = pins[i].g, b = pins[i].b;
			const int luma = (int)lroundf((0.2126f * r + 0.7152f * g + 0.0722f * b) / 2.55f);
			int hh, ss, ll;
			rgb_to_hsl(r, g, b, &hh, &ss, &ll);
			snprintf(line1, sizeof(line1), "%d  R%d G%d B%d  Y %d%%", (int)i + 1, r, g, b, luma);
			snprintf(line2, sizeof(line2), "H %d\xc2\xb0  S %d%%  L %d%%", hh, ss, ll);
		} else {
			snprintf(line1, sizeof(line1), "%d", (int)i + 1);
			line2[0] = 0;
		}

		const float tw = fmaxf(text_width(&ts, line1, px), text_width(&ts, line2, px));
		const float sw = 2.0f * px * ts.ux; // swatch spans both lines
		const float pad = 4.0f * ts.ux;
		const float box_w = sw + tw + pad * 3.0f;
		const float box_h = (2.0f * px + 6.0f) * ts.uy;
		// place the label beside the pin, flipped near the right or bottom edge
		float bx = x + arm * ts.ux + pad;
		float by = y + arm * ts.uy;
		if (bx + box_w > w)
			bx = x - arm * ts.ux - pad - box_w;
		if (by + box_h > h)
			by = y - arm * ts.uy - box_h;

		fill_quad(bx, by, bx + box_w, by + box_h, 0xC0101012);
		if (pins[i].valid) {
			// gs_effect_set_color takes 0xAARRGGBB
			const uint32_t c = 0xFF000000 | ((uint32_t)pins[i].r << 16) | ((uint32_t)pins[i].g << 8) |
					   pins[i].b;
			fill_quad(bx + pad, by + 3.0f * ts.uy, bx + pad + sw, by + box_h - 3.0f * ts.uy, c);
		}
		text_draw(&ts, line1, bx + pad * 2.0f + sw, by + 3.0f * ts.uy, px, 0xFFF0F0F0, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_TOP);
		text_draw(&ts, line2, bx + pad * 2.0f + sw, by + (3.0f + px) * ts.uy, px, 0xFFB8B8C0, TEXT_ALIGN_LEFT,
			  TEXT_VALIGN_TOP);
	}
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

	const float zoom = src->zoom;
	const float view_x = src->view_x, view_y = src->view_y;
	const bool zoomed = zoom > 1.001f;

	if (src->want_snapshot && tex) {
		src->want_snapshot = false;
		const uint32_t tw = gs_texture_get_width(tex), th = gs_texture_get_height(tex);
		if (!src->ref_tex || src->ref_w != tw || src->ref_h != th) {
			gs_texture_destroy(src->ref_tex);
			src->ref_tex = gs_texture_create(tw, th, gs_texture_get_color_format(tex), 1, NULL, 0);
			src->ref_w = tw;
			src->ref_h = th;
		}
		if (src->ref_tex)
			gs_copy_texture(src->ref_tex, tex);
	}

	if (zoomed) {
		// view = (picture - view origin) * zoom; the viewport clips what falls outside
		struct matrix4 tr = {
			{.ptr = {zoom, 0.0f, 0.0f, 0.0f}},
			{.ptr = {0.0f, zoom, 0.0f, 0.0f}},
			{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
			{.ptr = {-view_x * zoom, -view_y * zoom, 0.0f, 1.0f}},
		};
		gs_matrix_push();
		gs_matrix_mul(&tr);
	}

	if (tex && src->cm.effect) {
		// Darkens everything outside the shapes and the qualifier.
		gs_effect_t *dim = src->cm.effect;
		cm_set_mask_params(dim, &src->mask);
		cm_set_signal_params(dim, src->cm.colorspace);
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

	// comparing: the reference frame fills the left half, with a divider line
	const bool comparing = src->compare && src->ref_tex && src->ref_w == src->cm.texrender_width &&
			       src->ref_h == src->cm.texrender_height;
	if (comparing) {
		// the reference has the captured format: linear float for HDR, shown the same way as the live picture
		const bool hdr_ref = gs_texture_get_color_format(src->ref_tex) == GS_RGBA16F && src->cm.effect;
		gs_effect_t *def = hdr_ref ? src->cm.effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
		if (hdr_ref)
			cm_set_signal_params(def, CM_CS_2100_HLG);
		gs_effect_set_texture(gs_effect_get_param_by_name(def, "image"), src->ref_tex);
		while (gs_effect_loop(def, hdr_ref ? "DrawDisplay" : "Draw"))
			gs_draw_sprite_subregion(src->ref_tex, 0, 0, 0, src->ref_w / 2, src->ref_h);
		gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xFFFF59D9);
		const float xm = (float)(src->ref_w / 2);
		const struct text_scale ts = text_scale_from_viewport(w, h);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			gs_vertex2f(xm - 1.0f * ts.ux, 0.0f);
			gs_vertex2f(xm + 1.0f * ts.ux, 0.0f);
			gs_vertex2f(xm - 1.0f * ts.ux, h);
			gs_vertex2f(xm + 1.0f * ts.ux, h);
			gs_render_stop(GS_TRISTRIP);
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

	if (zoomed)
		gs_matrix_pop();

	// labels keep their size: drawn in view coordinates
	const struct text_scale ts = text_scale_from_viewport(w, h);
	if (comparing) {
		const float xm = ((float)(src->ref_w / 2) - view_x) * zoom;
		if (xm > 0.0f && xm < w) {
			text_draw(&ts, obs_module_text("ROI.Reference"), xm - 6.0f * ts.ux, h - 6.0f * ts.uy, 12.0f,
				  0xFFFF59D9, TEXT_ALIGN_RIGHT, TEXT_VALIGN_BOTTOM);
			text_draw(&ts, obs_module_text("ROI.Live"), xm + 6.0f * ts.ux, h - 6.0f * ts.uy, 12.0f,
				  0xFFFFFFFF, TEXT_ALIGN_LEFT, TEXT_VALIGN_BOTTOM);
		}
	}

	draw_pins(src, w, h, zoom, view_x, view_y);

	if (zoomed) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%.1fx", zoom);
		fill_quad(w - text_width(&ts, buf, 12.0f) - 10.0f * ts.ux, 4.0f * ts.uy, w - 4.0f * ts.ux,
			  (4.0f + 16.0f) * ts.uy, 0xA0000000);
		text_draw(&ts, buf, w - 7.0f * ts.ux, 6.0f * ts.uy, 12.0f, 0xFFFFFFFF, TEXT_ALIGN_RIGHT,
			  TEXT_VALIGN_TOP);
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

/* Reads the color under each pin, averaged over 3x3 pixels. Pins need the whole frame,
 * so the analysis isn't cropped while any exist (see roi_send_range). */
static void read_pins(struct roi_source *src, const struct cm_surface_data *sd)
{
	if (!sd->rgb_data || !sd->width || !sd->height)
		return;
	pthread_mutex_lock(&src->shapes_mutex);
	for (size_t i = 0; i < src->pins.num; i++) {
		struct roi_pin *pin = src->pins.array + i;
		const int cx = (int)(pin->x * sd->width);
		const int cy = (int)(pin->y * sd->height);
		int sum[3] = {0, 0, 0}, n = 0;
		for (int dy = -1; dy <= 1; dy++) {
			for (int dx = -1; dx <= 1; dx++) {
				const int x = cx + dx, y = cy + dy;
				if (x < 0 || y < 0 || x >= (int)sd->width || y >= (int)sd->height)
					continue;
				const uint8_t *p = sd->rgb_data + (size_t)sd->linesize * y + (size_t)x * 4;
				sum[0] += p[2];
				sum[1] += p[1];
				sum[2] += p[0];
				n++;
			}
		}
		if (n) {
			pin->r = (uint8_t)(sum[0] / n);
			pin->g = (uint8_t)(sum[1] / n);
			pin->b = (uint8_t)(sum[2] / n);
			pin->valid = true;
		}
	}
	pthread_mutex_unlock(&src->shapes_mutex);
}

static void roi_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct roi_source *src = data;

	if (src->pins.num)
		read_pins(src, surface_data);

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

/* ---------- zoom ---------- */

#define ROI_ZOOM_MAX 8.0f

static void clamp_view(struct roi_source *src)
{
	const float w = (float)roi_get_width(src), h = (float)roi_get_height(src);
	const float max_x = w - w / src->zoom, max_y = h - h / src->zoom;
	src->view_x = src->view_x < 0.0f ? 0.0f : src->view_x > max_x ? max_x : src->view_x;
	src->view_y = src->view_y < 0.0f ? 0.0f : src->view_y > max_y ? max_y : src->view_y;
}

/* Mouse events arrive in view coordinates; shapes and pins live in picture coordinates. */
static inline int to_picture_x(const struct roi_source *src, int x)
{
	return (int)floorf(x / src->zoom + src->view_x);
}
static inline int to_picture_y(const struct roi_source *src, int y)
{
	return (int)floorf(y / src->zoom + src->view_y);
}

/* The wheel zooms around the pointer; back at 1x the whole picture shows again. */
static void roi_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	UNUSED_PARAMETER(x_delta);
	struct roi_source *src = data;
	if (!y_delta || !event)
		return;

	const float px = event->x / src->zoom + src->view_x;
	const float py = event->y / src->zoom + src->view_y;

	float zoom = src->zoom * (y_delta > 0 ? 1.25f : 0.8f);
	if (zoom < 1.05f)
		zoom = 1.0f;
	if (zoom > ROI_ZOOM_MAX)
		zoom = ROI_ZOOM_MAX;

	src->zoom = zoom;
	src->view_x = px - event->x / zoom;
	src->view_y = py - event->y / zoom;
	clamp_view(src);
}

static void roi_mouse_move(void *data, const struct obs_mouse_event *event, bool mouse_leave)
{
	struct roi_source *src = data;
	if (mouse_leave || !event) {
		src->shift_down = false;
		return;
	}

	src->shift_down = !!(event->modifiers & INTERACT_SHIFT_KEY);

	if (src->panning) {
		src->view_x = src->pan_view_x - (event->x - src->pan_mouse_x) / src->zoom;
		src->view_y = src->pan_view_y - (event->y - src->pan_mouse_y) / src->zoom;
		clamp_view(src);
		return;
	}

	src->x_mouse = to_picture_x(src, event->x);
	src->y_mouse = to_picture_y(src, event->y);
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

static void add_pin(struct roi_source *src, int x, int y)
{
	const int w = (int)roi_get_width(src);
	const int h = (int)roi_get_height(src);
	if (w <= 0 || h <= 0)
		return;
	struct roi_pin pin = {.x = clamp01((float)x / w), .y = clamp01((float)y / h)};
	pthread_mutex_lock(&src->shapes_mutex);
	if (src->pins.num >= ROI_MAX_PINS)
		da_erase(src->pins, 0); // the oldest pin makes room
	da_push_back(src->pins, &pin);
	pthread_mutex_unlock(&src->shapes_mutex);
	shapes_store(src);
}

static void roi_mouse_click(void *data, const struct obs_mouse_event *event, int32_t type, bool mouse_up,
			    uint32_t click_count)
{
	struct roi_source *src = data;

	UNUSED_PARAMETER(click_count);

	// Pan with the middle button, or with the left one while no tool is on.
	const bool pan_button = type == MOUSE_MIDDLE || (type == MOUSE_LEFT && src->draw_mode == ROI_DRAW_NONE);
	if (pan_button) {
		if (!mouse_up && src->zoom > 1.001f) {
			src->panning = true;
			src->pan_mouse_x = event->x;
			src->pan_mouse_y = event->y;
			src->pan_view_x = src->view_x;
			src->pan_view_y = src->view_y;
		} else if (mouse_up) {
			src->panning = false;
		}
		return;
	}

	if (type != MOUSE_LEFT)
		return;

	src->shift_down = !!(event->modifiers & INTERACT_SHIFT_KEY);
	src->x_mouse = to_picture_x(src, event->x);
	src->y_mouse = to_picture_y(src, event->y);

	if (!mouse_up) {
		if (src->draw_mode == ROI_DRAW_PIN) {
			add_pin(src, src->x_mouse, src->y_mouse);
			return;
		}
		src->x_start = src->x_mouse;
		src->y_start = src->y_mouse;
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
	if (mode != ROI_DRAW_RECT && mode != ROI_DRAW_ELLIPSE && mode != ROI_DRAW_PIN)
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

void roi_undo_pin(struct roi_source *src)
{
	if (!src)
		return;
	pthread_mutex_lock(&src->shapes_mutex);
	if (src->pins.num)
		da_pop_back(src->pins);
	pthread_mutex_unlock(&src->shapes_mutex);
	shapes_store(src);
}

void roi_clear_pins(struct roi_source *src)
{
	if (!src)
		return;
	pthread_mutex_lock(&src->shapes_mutex);
	da_resize(src->pins, 0);
	pthread_mutex_unlock(&src->shapes_mutex);
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
	if (src->shapes.num && !src->pins.num && w > 0 && h > 0) {
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
	if (src->pins.num)
		src->cm.flags |= CM_FLAG_CONVERT_RGB;

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
	.mouse_wheel = roi_mouse_wheel,
};
