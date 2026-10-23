#include <obs-module.h>
#include <util/platform.h>
#include "plugin-macros.generated.h"
#include <graphics/matrix4.h>
#include "common.h"
#include "util.h"
#include "text-render.h"

#ifdef ENABLE_PROFILE
#define PROFILE_START(x) profile_start(x)
#define PROFILE_END(x) profile_end(x)
static const char *prof_render_name = "wvs_render";
static const char *prof_draw_waveform_name = "draw_waveform";
static const char *prof_draw_name = "draw";
static const char *prof_draw_graticule_name = "graticule";
#else // ENABLE_PROFILE
#define PROFILE_START(x)
#define PROFILE_END(x)
#endif // ! ENABLE_PROFILE

#define WV_SIZE 256

enum scale_labels_e {
	SCALE_NONE = 0,
	SCALE_PERCENT = 1,
	SCALE_IRE = 2,
	SCALE_8BIT = 3,
	SCALE_10BIT = 4, // legal range, 64 at black and 940 at white
};

#define ZOOM_MAX 8.0f

#define DISP_OVERLAY 0
#define DISP_STACK 1
#define DISP_PARADE 2

#define COMP_RGB 0x07
#define COMP_Y 0x20
#define COMP_UV 0x50
#define COMP_YUV (COMP_Y | COMP_UV)
// Luma next to R, G and B; luma goes into the fourth texture channel.
#define COMP_YRGB_FLAG 0x80
#define COMP_YRGB (COMP_RGB | COMP_YRGB_FLAG)

static inline bool is_yrgb(uint32_t components)
{
	return (components & COMP_YRGB_FLAG) != 0;
}

struct wvs_source
{
	struct cm_source cm;

	gs_effect_t *effect;
	gs_texture_t *tex_wv;
	uint32_t tex_wv_width;
	bool tex_wv_alpha;
	uint8_t *tex_buf[2];
	uint32_t tex_buf_width[2];
	volatile int w_tex_buf;
	int r_tex_buf;

	gs_vertbuffer_t *graticule_line_vbuf;

	int display;
	uint32_t components;
	int intensity;
	int graticule_lines, graticule_lines_prev;
	int scale_labels; // scale_labels_e

	// vertical zoom with the mouse wheel; zoom_off is the top of the view in trace units
	float zoom, zoom_off;

	// snapshot reference, shown over the live trace while comparing
	volatile bool want_snapshot;
	volatile bool compare;
	uint8_t *ref_buf;
	uint32_t ref_width;
	bool ref_alpha;
	bool ref_dirty;
	gs_texture_t *tex_ref;
};

static void wvs_update(void *, obs_data_t *);
static void wvs_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *wvs_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Waveform");
}

static void wvs_snapshot_cb(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	((struct wvs_source *)data)->want_snapshot = true;
}

static void wvs_compare_cb(void *data, calldata_t *cd)
{
	((struct wvs_source *)data)->compare = calldata_bool(cd, "on");
}

static void *wvs_create(obs_data_t *settings, obs_source_t *source)
{
	struct wvs_source *src = bzalloc(sizeof(struct wvs_source));

	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, wvs_surface_cb, src);
	src->zoom = 1.0f;

	proc_handler_t *ph = obs_source_get_proc_handler(source);
	proc_handler_add(ph, "void va_snapshot()", wvs_snapshot_cb, src);
	proc_handler_add(ph, "void va_set_compare(in bool on)", wvs_compare_cb, src);

	obs_enter_graphics();
	src->effect = create_effect_from_module_file("waveform.effect");
	obs_leave_graphics();

	wvs_update(src, settings);

	return src;
}

static void wvs_destroy(void *data)
{
	struct wvs_source *src = data;

	obs_enter_graphics();
	gs_texture_destroy(src->tex_wv);
	gs_texture_destroy(src->tex_ref);
	vertexbuffer_destroy_safe(src->graticule_line_vbuf);
	obs_leave_graphics();
	bfree(src->ref_buf);

	cm_destroy(&src->cm);

	bfree(src->tex_buf[0]);
	bfree(src->tex_buf[1]);

	bfree(src);
}

static void wvs_update(void *data, obs_data_t *settings)
{
	struct wvs_source *src = data;
	cm_update(&src->cm, settings);

	src->display = (int)obs_data_get_int(settings, "display");

	src->components = (uint32_t)obs_data_get_int(settings, "components");
	src->cm.flags = (src->components & COMP_RGB ? CM_FLAG_CONVERT_RGB : 0) |
			(src->components & COMP_YUV || is_yrgb(src->components) ? CM_FLAG_CONVERT_YUV : 0);

	src->intensity = (int)obs_data_get_int(settings, "intensity");
	if (src->intensity < 1)
		src->intensity = 1;

	src->graticule_lines = (int)obs_data_get_int(settings, "graticule_lines");
	src->scale_labels = (int)obs_data_get_int(settings, "scale_labels");
}

static void wvs_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "intensity", 51);
	obs_data_set_default_int(settings, "components", COMP_RGB);
	obs_data_set_default_int(settings, "graticule_lines", 5);
	obs_data_set_default_int(settings, "scale_labels", SCALE_PERCENT);
}

static bool components_changed(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	uint32_t components = settings ? (uint32_t)obs_data_get_int(settings, "components") : 0;
	obs_property_t *prop = obs_properties_get(props, "colorspace");
	// TODO: temporarily disable colorspace setting if the target is ROI
	bool vis = !!(components & COMP_YUV) || is_yrgb(components);
	if (vis && is_roi_source_name(obs_data_get_string(settings, "target_name")))
		vis = false;
	if (prop)
		obs_property_set_visible(prop, vis);
	return true;
}

static obs_properties_t *wvs_get_properties(void *data)
{
	struct wvs_source *src = data;
	obs_properties_t *props;
	obs_property_t *prop;
	props = obs_properties_create();

	cm_get_properties(&src->cm, props);

	prop = obs_properties_add_list(props, "display", obs_module_text("Display"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Overlay"), DISP_OVERLAY);
	obs_property_list_add_int(prop, obs_module_text("Stack"), DISP_STACK);
	obs_property_list_add_int(prop, obs_module_text("Parade"), DISP_PARADE);

	prop = obs_properties_add_list(props, "components", obs_module_text("Components"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_set_modified_callback(prop, components_changed);
	obs_property_list_add_int(prop, obs_module_text("RGB"), COMP_RGB);
	obs_property_list_add_int(prop, obs_module_text("Luma"), COMP_Y);
	obs_property_list_add_int(prop, obs_module_text("Chroma"), COMP_UV);
	obs_property_list_add_int(prop, obs_module_text("YUV"), COMP_YUV);
	obs_property_list_add_int(prop, obs_module_text("YRGB"), COMP_YRGB);

	// TODO: Disable this property if ROI target is selected.
	properties_add_colorspace(props, "colorspace", obs_module_text("Color space"));

	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);
	prop = obs_properties_add_list(props, "graticule_lines", obs_module_text("Graticule"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("None"), 0);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.100"), 1);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.50"), 2);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.25"), 4);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.20"), 5);
	obs_property_list_add_int(prop, obs_module_text("Graticule.Step.10"), 10);

	prop = obs_properties_add_list(props, "scale_labels", obs_module_text("WV.Prop.Scale"), OBS_COMBO_TYPE_LIST,
				       OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("None"), SCALE_NONE);
	obs_property_list_add_int(prop, obs_module_text("WV.Prop.Scale.Percent"), SCALE_PERCENT);
	obs_property_list_add_int(prop, obs_module_text("WV.Prop.Scale.IRE"), SCALE_IRE);
	obs_property_list_add_int(prop, obs_module_text("WV.Prop.Scale.8bit"), SCALE_8BIT);
	obs_property_list_add_int(prop, obs_module_text("WV.Prop.Scale.10bit"), SCALE_10BIT);

	return props;
}

static inline uint32_t n_components(const struct wvs_source *src)
{
	if (is_yrgb(src->components))
		return 4;
	uint32_t c = src->components & (COMP_RGB | COMP_YUV);
	c = c - ((c >> 1) & 0x55);
	c = (c & 0x33) + ((c >> 2) & 0x33);
	c = (c & 0x0F) + ((c >> 4) & 0x0F);
	return c;
}

static uint32_t wvs_get_width(void *data)
{
	struct wvs_source *src = data;
	if (src->cm.bypass)
		return cm_bypass_get_width(&src->cm);
	if (src->display == DISP_PARADE)
		return src->tex_buf_width[src->r_tex_buf] * n_components(src);
	return src->tex_buf_width[src->r_tex_buf];
}

static uint32_t wvs_get_height(void *data)
{
	struct wvs_source *src = data;
	if (src->cm.bypass)
		return cm_bypass_get_height(&src->cm);
	if (src->display == DISP_STACK)
		return WV_SIZE * n_components(src);
	return WV_SIZE;
}

static inline void inc_uint8(uint8_t *c)
{
	if (*c < 255)
		++*c;
}

static inline void ensure_tex_buf_size(struct wvs_source *src, const uint32_t width, int ix)
{
	if (src->tex_buf[ix] && src->tex_buf_width[ix] == width)
		return;

	if (!width)
		return;

	bfree(src->tex_buf[ix]);
	src->tex_buf[ix] = bzalloc(width * WV_SIZE * 4);
	src->tex_buf_width[ix] = width;
}

static inline void wvs_draw_waveform(struct wvs_source *src, uint8_t *dbuf, const struct cm_surface_data *surface_data)
{
	const uint32_t height = surface_data->height;
	const uint32_t width = surface_data->width;

	for (uint32_t i = 0; i < width * WV_SIZE * 4; i++)
		dbuf[i] = 0;

	const uint8_t *video_data = NULL;
	if (src->components & COMP_RGB)
		video_data = surface_data->rgb_data;
	else if (src->components & COMP_YUV)
		video_data = surface_data->yuv_data;
	if (!video_data)
		return;

	const bool calc_b = (src->components & 0x11) ? true : false;
	const bool calc_g = (src->components & 0x22) ? true : false;
	const bool calc_r = (src->components & 0x44) ? true : false;

	if (is_yrgb(src->components)) {
		const uint8_t *yuv_data = surface_data->yuv_data;
		if (!yuv_data)
			return;
		for (uint32_t y = 0; y < height; y++) {
			const uint8_t *v = video_data + surface_data->linesize * y;
			const uint8_t *l = yuv_data + surface_data->linesize * y;
			for (uint32_t x = 0; x < width; x++, v += 4, l += 4) {
				if (!v[3])
					continue;
				uint8_t *d = dbuf + x * 4;
				inc_uint8(d + (WV_SIZE - 1 - v[0]) * width * 4 + 0);
				inc_uint8(d + (WV_SIZE - 1 - v[1]) * width * 4 + 1);
				inc_uint8(d + (WV_SIZE - 1 - v[2]) * width * 4 + 2);
				inc_uint8(d + (WV_SIZE - 1 - l[1]) * width * 4 + 3); // luma
			}
		}
		return;
	}

	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *v = video_data + surface_data->linesize * y;
		for (uint32_t x = 0; x < width; x++) {
			const uint8_t b = *v++;
			const uint8_t g = *v++;
			const uint8_t r = *v++;
			const uint8_t a = *v++;
			if (!a)
				continue;
			if (calc_b)
				inc_uint8(dbuf + x * 4 + (WV_SIZE - 1 - b) * width * 4 + 0);
			if (calc_g)
				inc_uint8(dbuf + x * 4 + (WV_SIZE - 1 - g) * width * 4 + 1);
			if (calc_r)
				inc_uint8(dbuf + x * 4 + (WV_SIZE - 1 - r) * width * 4 + 2);
		}
	}
}

static void wvs_set_image(struct wvs_source *src, const uint8_t *tex_buf, uint32_t width)
{
	// YRGB keeps luma in the fourth channel, so it needs a texture with alpha.
	const bool alpha = is_yrgb(src->components);
	if (src->tex_wv && src->tex_wv_width == width && src->tex_wv_alpha == alpha) {
		gs_texture_set_image(src->tex_wv, tex_buf, width * 4, false);
		return;
	}

	if (src->tex_wv)
		gs_texture_destroy(src->tex_wv);
	src->tex_wv = gs_texture_create(width, WV_SIZE, alpha ? GS_BGRA : GS_BGRX, 1, &tex_buf, GS_DYNAMIC);
	src->tex_wv_width = width;
	src->tex_wv_alpha = alpha;
}

static void wvs_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct wvs_source *src = data;

	if ((src->components & COMP_RGB) && !surface_data->rgb_data)
		return;
	if ((src->components & COMP_YUV || is_yrgb(src->components)) && !surface_data->yuv_data)
		return;
	if (!surface_data->width)
		return;

	ensure_tex_buf_size(src, surface_data->width, src->w_tex_buf);

	PROFILE_START(prof_draw_waveform_name);
	wvs_draw_waveform(src, src->tex_buf[src->w_tex_buf], surface_data);
	PROFILE_END(prof_draw_waveform_name);
	src->w_tex_buf ^= 1;
}

static void create_graticule_vbuf(struct wvs_source *src)
{
	obs_enter_graphics();
	vertexbuffer_destroy_safe(src->graticule_line_vbuf);
	src->graticule_line_vbuf = NULL;
	if (src->graticule_lines > 0) {
		gs_render_start(true);
		for (int i = 0; i <= src->graticule_lines; i++) {
			gs_vertex2f(0.0f, 256.0f * i / src->graticule_lines);
			gs_vertex2f(1.0f, 256.0f * i / src->graticule_lines);
		}
		src->graticule_line_vbuf = gs_render_save();
	}
	obs_leave_graphics();
}

static void wvs_render_graticule(struct wvs_source *src)
{
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0x80FFBF00); // amber
	while (gs_effect_loop(effect, "Solid")) {
		bool stack = src->display == DISP_STACK;
		bool parade = src->display == DISP_PARADE;
		int n_stack = stack ? n_components(src) : 1;
		for (int i = 0; i < n_stack; i++) {
			const float yoff = stack ? WV_SIZE * i + 0.5f : 0.0f;
			const float xcoe =
				(float)(src->tex_buf_width[src->r_tex_buf] * (parade ? n_components(src) : 1));
			struct matrix4 tr = {
				{.ptr = {xcoe, 0.0f, 0.0f, 0.0f}},
				{.ptr = {0.0f, 1.0f, 0.0f, 0.0f}},
				{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
				{.ptr = {0.0f, yoff, 0.0f, 1.0f}},
			};
			gs_matrix_push();
			gs_matrix_mul(&tr);
			gs_load_vertexbuffer(src->graticule_line_vbuf);
			gs_draw(GS_LINES, stack && i ? 2 : 0, 0);
			gs_matrix_pop();
		}
	}
}

/* Takes the trace being shown as the reference; graphics thread. */
static void wvs_take_snapshot(struct wvs_source *src)
{
	src->want_snapshot = false;
	const uint8_t *cur = src->tex_buf[src->r_tex_buf];
	const uint32_t width = src->tex_buf_width[src->r_tex_buf];
	if (!cur || !width)
		return;
	const size_t size = (size_t)width * WV_SIZE * 4;
	if (src->ref_width != width) {
		bfree(src->ref_buf);
		src->ref_buf = bmalloc(size);
		src->ref_width = width;
	}
	memcpy(src->ref_buf, cur, size);
	src->ref_alpha = is_yrgb(src->components);
	src->ref_dirty = true;
}

static void render_reference(struct wvs_source *src)
{
	if (!src->ref_buf || !src->effect)
		return;
	if (src->ref_dirty || !src->tex_ref) {
		gs_texture_destroy(src->tex_ref);
		const uint8_t *buf = src->ref_buf;
		src->tex_ref =
			gs_texture_create(src->ref_width, WV_SIZE, src->ref_alpha ? GS_BGRA : GS_BGRX, 1, &buf, 0);
		src->ref_dirty = false;
	}
	gs_effect_t *e = src->effect;
	const int n = (int)n_components(src);
	int w = src->ref_width, h = WV_SIZE;
	float mode = 0.0f;
	if (src->display == DISP_STACK) {
		h *= n;
		mode = 1.0f;
	} else if (src->display == DISP_PARADE) {
		w *= n;
		mode = 2.0f;
	}
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "ref_image"), src->tex_ref);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "intensity"), (float)src->intensity);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "ref_mode"), mode);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "ref_n"), (float)n);
	gs_effect_set_float(gs_effect_get_param_by_name(e, "ref_yrgb"), src->ref_alpha ? 1.0f : 0.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(e, "DrawRef"))
		gs_draw_sprite(src->tex_ref, 0, w, h);
	gs_blend_state_pop();
}

static void render_waveform(struct wvs_source *src)
{
	gs_effect_t *effect = src->effect ? src->effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), src->tex_wv);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "intensity"), (float)src->intensity);
	const char *name = "Draw";
	int w = src->tex_wv_width;
	int h = WV_SIZE;
	int n = n_components(src);
	if (src->effect)
		switch (src->display) {
		case DISP_STACK:
			name = n == 4 ? "DrawStackYRGB" : n == 3 ? "DrawStack" : n == 2 ? "DrawStackUV" : "DrawOverlay";
			h *= n;
			break;
		case DISP_PARADE:
			name = n == 4   ? "DrawParadeYRGB"
			       : n == 3 ? "DrawParade"
			       : n == 2 ? "DrawParadeUV"
					: "DrawOverlay";
			w *= n;
			break;
		default:
			name = n == 4 ? "DrawOverlayYRGB" : "DrawOverlay";
			break;
		}

	while (gs_effect_loop(effect, name))
		gs_draw_sprite(src->tex_wv, 0, w, h);
}

static inline bool zoom_active(const struct wvs_source *src)
{
	return src->zoom > 1.001f && src->display != DISP_STACK;
}

static const char *scale_label(int scale, float level, char *buf, size_t size)
{
	switch (scale) {
	case SCALE_PERCENT:
		snprintf(buf, size, "%d%%", (int)lroundf(level * 100.0f));
		break;
	case SCALE_IRE:
		snprintf(buf, size, "%d", (int)lroundf(level * 100.0f));
		break;
	case SCALE_8BIT:
		snprintf(buf, size, "%d", (int)lroundf(level * 255.0f));
		break;
	case SCALE_10BIT:
		snprintf(buf, size, "%d", (int)lroundf(64.0f + level * 876.0f));
		break;
	default:
		buf[0] = 0;
	}
	return buf;
}

/* Values beside the graticule lines, drawn on the left edge so they don't change the layout. */
static void wvs_render_scale(struct wvs_source *src, float width, float height)
{
	if (src->scale_labels == SCALE_NONE || src->graticule_lines <= 0)
		return;

	const struct text_scale ts = text_scale_from_viewport(width, height);
	const bool stack = src->display == DISP_STACK;
	const int n_stack = stack ? (int)n_components(src) : 1;
	const float zoom = zoom_active(src) ? src->zoom : 1.0f;
	const float off = zoom_active(src) ? src->zoom_off : 0.0f;
	const float px = 11.0f;
	const float pad = 3.0f * ts.ux;

	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	for (int k = 0; k < n_stack; k++) {
		const float base = (float)(WV_SIZE * k);
		for (int i = 0; i <= src->graticule_lines; i++) {
			const float level = 1.0f - (float)i / src->graticule_lines; // top line is white
			const float y = base + (WV_SIZE * (float)i / src->graticule_lines - off) * zoom;
			if (y < base - 1.0f || y > base + WV_SIZE + 1.0f)
				continue; // outside the zoomed view
			char buf[16];
			scale_label(src->scale_labels, level, buf, sizeof(buf));
			const float tw = text_width(&ts, buf, px);
			// keep labels inside the trace area
			int valign = TEXT_VALIGN_MIDDLE;
			if (y - px * 0.5f * ts.uy < base)
				valign = TEXT_VALIGN_TOP;
			else if (y + px * 0.5f * ts.uy > base + WV_SIZE)
				valign = TEXT_VALIGN_BOTTOM;
			const float y0 = valign == TEXT_VALIGN_TOP      ? y
					 : valign == TEXT_VALIGN_BOTTOM ? y - px * ts.uy
									: y - px * 0.5f * ts.uy;
			gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0xA0000000);
			while (gs_effect_loop(solid, "Solid")) {
				gs_render_start(false);
				gs_vertex2f(0.0f, y0);
				gs_vertex2f(tw + pad * 2.0f, y0);
				gs_vertex2f(0.0f, y0 + px * ts.uy);
				gs_vertex2f(tw + pad * 2.0f, y0 + px * ts.uy);
				gs_render_stop(GS_TRISTRIP);
			}
			text_draw(&ts, buf, pad, y, px, 0xE0FFD060, TEXT_ALIGN_LEFT, valign);
		}
	}

	if (zoom_active(src)) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%.1fx", src->zoom);
		text_draw(&ts, buf, width - pad, pad, px, 0xE0FFFFFF, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
	}
}

static void wvs_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct wvs_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	PROFILE_START(prof_render_name);

	cm_render_target(&src->cm);

	const bool zoomed = zoom_active(src);
	if (zoomed) {
		// y' = (y - off) * zoom; the dock's viewport clips what falls outside
		struct matrix4 tr = {
			{.ptr = {1.0f, 0.0f, 0.0f, 0.0f}},
			{.ptr = {0.0f, src->zoom, 0.0f, 0.0f}},
			{.ptr = {0.0f, 0.0f, 1.0f, 0.0f}},
			{.ptr = {0.0f, -src->zoom_off * src->zoom, 0.0f, 1.0f}},
		};
		gs_matrix_push();
		gs_matrix_mul(&tr);
	}

	PROFILE_START(prof_draw_name);
	if (src->want_snapshot)
		wvs_take_snapshot(src);
	if (src->tex_buf[src->r_tex_buf]) {
		wvs_set_image(src, src->tex_buf[src->r_tex_buf], src->tex_buf_width[src->r_tex_buf]);
		render_waveform(src);
	}
	if (src->compare && src->ref_buf && src->ref_width == src->tex_buf_width[src->r_tex_buf])
		render_reference(src);
	PROFILE_END(prof_draw_name);

	PROFILE_START(prof_draw_graticule_name);
	if (src->graticule_lines > 0) {
		if (src->graticule_lines != src->graticule_lines_prev) {
			create_graticule_vbuf(src);
			src->graticule_lines_prev = src->graticule_lines;
		}
		wvs_render_graticule(src);
	}
	PROFILE_END(prof_draw_graticule_name);

	if (zoomed)
		gs_matrix_pop();

	wvs_render_scale(src, (float)wvs_get_width(src), (float)wvs_get_height(src));

	PROFILE_END(prof_render_name);
}

/* Wheel zooms the trace vertically around the pointer, e.g. into the blacks; zooming out to 1x resets. */
static void wvs_mouse_wheel(void *data, const struct obs_mouse_event *event, int x_delta, int y_delta)
{
	UNUSED_PARAMETER(x_delta);
	struct wvs_source *src = data;
	if (src->display == DISP_STACK || !y_delta)
		return;

	const float h = (float)WV_SIZE;
	const float ys = event ? (float)event->y : h * 0.5f; // pointer, in source units
	const float yd = ys / src->zoom + src->zoom_off;     // trace level under the pointer

	float zoom = src->zoom * (y_delta > 0 ? 1.25f : 0.8f);
	if (zoom < 1.0f)
		zoom = 1.0f;
	if (zoom > ZOOM_MAX)
		zoom = ZOOM_MAX;

	float off = yd - ys / zoom;
	if (off < 0.0f)
		off = 0.0f;
	if (off > h - h / zoom)
		off = h - h / zoom;

	src->zoom = zoom;
	src->zoom_off = zoom > 1.001f ? off : 0.0f;
}

static void wvs_tick(void *data, float second)
{
	struct wvs_source *src = data;
	cm_tick(data, second);

	src->r_tex_buf = src->w_tex_buf ^ 1;
}

const struct obs_source_info colormonitor_waveform = {
	.id = ID_PREFIX "waveform_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_INTERACTION,
	.get_name = wvs_get_name,
	.create = wvs_create,
	.destroy = wvs_destroy,
	.update = wvs_update,
	.get_defaults = wvs_get_defaults,
	.get_properties = wvs_get_properties,
	.get_width = wvs_get_width,
	.get_height = wvs_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = wvs_render,
	.video_tick = wvs_tick,
	.mouse_wheel = wvs_mouse_wheel,
};
