/*
 * Channel plot: every pixel plotted by two of its channels against each other, for example R across and G up,
 * or Cb across and Cr up. A neutral picture lies on the diagonal of an R'G'B' pair; with the raw signal of a video
 * source a box marks the legal range (0 % to 100 %) and a dashed box the preferred range of EBU R103 (-5 % to 105 %),
 * so values that a conversion would clip stand out.
 */
#include <obs-module.h>
#include <util/platform.h>
#include <math.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define CP_SIZE 256

enum cp_pair_e {
	CP_R_G = 0,
	CP_R_B = 1,
	CP_G_B = 2,
	CP_Y_CB = 3,
	CP_Y_CR = 4,
	CP_CB_CR = 5,
};

/* The channels of a pair: index into B, G, R, Y (0..3) for x and y, or 4 for Cb, 5 for Cr. */
static const int pair_channels[6][2] = {{2, 1}, {2, 0}, {1, 0}, {3, 4}, {3, 5}, {4, 5}};
static const char *channel_names[6] = {"B", "G", "R", "Y", "Cb", "Cr"};

struct cp_source
{
	struct cm_source cm;
	volatile bool ext; // R, G, B and Y from the raw signal: legal codes, 16 is 0 %, 235 is 100 %

	gs_texture_t *tex;
	uint8_t *tex_buf[2];
	volatile int w_tex_buf;
	gs_effect_t *effect;

	int pair;
	int intensity;
	bool graticule;
};

static void cp_update(void *, obs_data_t *);
static void cp_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *cp_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ChannelPlot");
}

static void *cp_create(obs_data_t *settings, obs_source_t *source)
{
	struct cp_source *src = bzalloc(sizeof(struct cp_source));
	src->cm.flags = CM_FLAG_CONVERT_RGB | CM_FLAG_CONVERT_YUV | CM_FLAG_EXT;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, cp_surface_cb, src);

	obs_enter_graphics();
	src->effect = create_effect_from_module_file("vectorscope.effect");
	obs_leave_graphics();

	cp_update(src, settings);
	return src;
}

static void cp_destroy(void *data)
{
	struct cp_source *src = data;
	obs_enter_graphics();
	gs_texture_destroy(src->tex); // the effect is shared: OBS keeps one per file
	obs_leave_graphics();
	cm_destroy(&src->cm);
	bfree(src->tex_buf[0]);
	bfree(src->tex_buf[1]);
	bfree(src);
}

static void cp_update(void *data, obs_data_t *settings)
{
	struct cp_source *src = data;
	cm_update(&src->cm, settings);
	src->pair = (int)obs_data_get_int(settings, "cp_pair");
	if (src->pair < 0 || src->pair > CP_CB_CR)
		src->pair = CP_R_G;
	src->intensity = (int)obs_data_get_int(settings, "intensity");
	if (src->intensity < 1)
		src->intensity = 1;
	src->graticule = obs_data_get_bool(settings, "show_graticule");
}

static void cp_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "cp_pair", CP_R_G);
	obs_data_set_default_int(settings, "intensity", 25);
	obs_data_set_default_bool(settings, "show_graticule", true);
}

static obs_properties_t *cp_get_properties(void *data)
{
	struct cp_source *src = data;
	obs_properties_t *props = obs_properties_create();
	cm_get_properties(&src->cm, props);
	obs_property_t *p = obs_properties_add_list(props, "cp_pair", obs_module_text("ChannelPlot.Pair"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "R / G", CP_R_G);
	obs_property_list_add_int(p, "R / B", CP_R_B);
	obs_property_list_add_int(p, "G / B", CP_G_B);
	obs_property_list_add_int(p, "Y / Cb", CP_Y_CB);
	obs_property_list_add_int(p, "Y / Cr", CP_Y_CR);
	obs_property_list_add_int(p, "Cb / Cr", CP_CB_CR);
	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);
	obs_properties_add_bool(props, "show_graticule", obs_module_text("Graticule"));
	return props;
}

static uint32_t cp_get_width(void *data)
{
	struct cp_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : CP_SIZE;
}

static uint32_t cp_get_height(void *data)
{
	struct cp_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : CP_SIZE;
}

/* Channel c (B, G, R, Y, Cb, Cr) of a pixel, 0..255: the raw legal code when there is one. */
static inline int channel(int c, const uint8_t *rgb, const uint8_t *yuv, const uint8_t *ext)
{
	if (c == 4)
		return yuv[0]; // the conversion keeps U in B, Y in G, V in R
	if (c == 5)
		return yuv[2];
	if (ext)
		return ext[c];
	return c == 3 ? yuv[1] : rgb[c];
}

static void cp_surface_cb(void *data, struct cm_surface_data *sd)
{
	struct cp_source *src = data;
	if (!sd->rgb_data || !sd->yuv_data || !sd->width)
		return;
	if (!src->tex_buf[src->w_tex_buf])
		src->tex_buf[src->w_tex_buf] = bzalloc(CP_SIZE * CP_SIZE);
	uint8_t *dbuf = src->tex_buf[src->w_tex_buf];
	memset(dbuf, 0, CP_SIZE * CP_SIZE);

	const int cx = pair_channels[src->pair][0], cy = pair_channels[src->pair][1];
	// the raw signal counts only when both channels have it (Cb and Cr do not)
	const bool ext = sd->ext_data && cx < 4 && cy < 4;
	for (uint32_t y = 0; y < sd->height; y++) {
		const uint8_t *rgb = sd->rgb_data + (size_t)sd->linesize * y;
		const uint8_t *yuv = sd->yuv_data + (size_t)sd->linesize * y;
		const uint8_t *e = ext ? sd->ext_data + (size_t)sd->ext_linesize * y : NULL;
		for (uint32_t x = 0; x < sd->width; x++, rgb += 4, yuv += 4) {
			if (!rgb[3])
				continue;
			const uint8_t *ex = e ? e + x * 4 : NULL;
			const int vx = channel(cx, rgb, yuv, ex), vy = channel(cy, rgb, yuv, ex);
			uint8_t *c = dbuf + (CP_SIZE - 1 - vy) * CP_SIZE + vx;
			if (*c < 255)
				++*c;
		}
	}
	src->ext = ext;
	src->w_tex_buf ^= 1;
}

static void rect_lines(float x0, float y0, float x1, float y1, bool dashed)
{
	if (!dashed) {
		gs_vertex2f(x0, y0), gs_vertex2f(x1, y0);
		gs_vertex2f(x1, y0), gs_vertex2f(x1, y1);
		gs_vertex2f(x1, y1), gs_vertex2f(x0, y1);
		gs_vertex2f(x0, y1), gs_vertex2f(x0, y0);
		return;
	}
	for (float t = 0.0f; t < 1.0f; t += 0.04f) {
		const float t1 = fminf(t + 0.02f, 1.0f);
		gs_vertex2f(x0 + (x1 - x0) * t, y0), gs_vertex2f(x0 + (x1 - x0) * t1, y0);
		gs_vertex2f(x0 + (x1 - x0) * t, y1), gs_vertex2f(x0 + (x1 - x0) * t1, y1);
		gs_vertex2f(x0, y0 + (y1 - y0) * t), gs_vertex2f(x0, y0 + (y1 - y0) * t1);
		gs_vertex2f(x1, y0 + (y1 - y0) * t), gs_vertex2f(x1, y0 + (y1 - y0) * t1);
	}
}

static void cp_render_graticule(const struct cp_source *src)
{
	const float S = (float)CP_SIZE;
	const int cx = pair_channels[src->pair][0], cy = pair_channels[src->pair][1];
	const bool rgb_pair = cx < 3 && cy < 3;
	const bool ext = src->ext;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *col = gs_effect_get_param_by_name(solid, "color");

	gs_effect_set_color(col, 0x30FFFFFF);
	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		for (int i = 1; i < 4; i++) {
			const float t = S * i / 4.0f;
			gs_vertex2f(0.0f, t), gs_vertex2f(S, t);
			gs_vertex2f(t, 0.0f), gs_vertex2f(t, S);
		}
		gs_render_stop(GS_LINES);
	}
	if (rgb_pair) {
		// neutral: both channels equal
		gs_effect_set_color(col, 0x60FFFFFF);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			gs_vertex2f(0.0f, S), gs_vertex2f(S, 0.0f);
			gs_render_stop(GS_LINES);
		}
	}
	if (ext) {
		// the legal range (0 % to 100 %) and the preferred range of EBU R103 (-5 % to 105 %), in codes
		gs_effect_set_color(col, 0xB0FFD060);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			rect_lines(16.0f, S - 235.0f, 235.0f, S - 16.0f, false);
			gs_render_stop(GS_LINES);
		}
		gs_effect_set_color(col, 0x90FF9040);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			rect_lines(5.0f, S - 246.0f, 246.0f, S - 5.0f, true);
			gs_render_stop(GS_LINES);
		}
	}
	const struct text_scale ts = text_scale_from_viewport(S, S);
	const float px = 10.0f, m = 3.0f * ts.ux;
	text_draw(&ts, channel_names[cy], m, m, px, 0xE0FFD060, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	text_draw(&ts, channel_names[cx], S - m, S - m, px, 0xE0FFD060, TEXT_ALIGN_RIGHT, TEXT_VALIGN_BOTTOM);
	if (ext)
		text_draw(&ts, "RAW", S - m, m, px, 0xC0F0D060, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
}

static void cp_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct cp_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}
	cm_render_target(&src->cm);

	const int r = src->w_tex_buf ^ 1;
	if (src->tex_buf[r] && src->effect) {
		const uint8_t *buf = src->tex_buf[r];
		if (!src->tex)
			src->tex = gs_texture_create(CP_SIZE, CP_SIZE, GS_R8, 1, &buf, GS_DYNAMIC);
		else
			gs_texture_set_image(src->tex, buf, CP_SIZE, false);
		gs_effect_t *e = src->effect;
		struct vec4 color;
		struct vec3 zero;
		vec4_set(&color, 1.0f, 1.0f, 1.0f, 1.0f);
		vec3_zero(&zero);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), src->tex);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "intensity"), (float)src->intensity);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, "color"), &color);
		gs_effect_set_vec3(gs_effect_get_param_by_name(e, "color_u"), &zero);
		gs_effect_set_vec3(gs_effect_get_param_by_name(e, "color_v"), &zero);
		while (gs_effect_loop(e, "Draw"))
			gs_draw_sprite(src->tex, 0, CP_SIZE, CP_SIZE);
	}
	if (src->graticule)
		cp_render_graticule(src);
}

const struct obs_source_info colormonitor_channelplot = {
	.id = ID_PREFIX "channelplot_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = cp_get_name,
	.create = cp_create,
	.destroy = cp_destroy,
	.update = cp_update,
	.get_defaults = cp_get_defaults,
	.get_properties = cp_get_properties,
	.get_width = cp_get_width,
	.get_height = cp_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = cp_render,
	.video_tick = cm_tick,
};
