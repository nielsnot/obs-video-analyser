/*
 * Diamond display (Tektronix style gamut diamond).
 *
 * Each pixel is plotted twice: the upper diamond shows B - G across and B + G upwards,
 * the lower diamond shows R - G across and R + G downwards. Black sits where the two
 * diamonds touch, white at their far tips. A pixel inside both outlines is within the
 * legal RGB gamut; anything drawn outside an outline is out of gamut.
 */
#include <obs-module.h>
#include <util/platform.h>
#include "plugin-macros.generated.h"
#include "common.h"
#include "util.h"
#include "text-render.h"

#define DM_SIZE 256

enum dm_color_e {
	DM_COLOR_GREEN = 0,
	DM_COLOR_WHITE = 1,
};

struct dm_source
{
	struct cm_source cm;

	gs_texture_t *tex;
	uint8_t *tex_buf[2];
	volatile int w_tex_buf;
	gs_effect_t *effect;

	int intensity;
	int trace_color;
	bool graticule;
	bool labels;
};

static void dm_update(void *, obs_data_t *);
static void dm_surface_cb(void *data, struct cm_surface_data *surface_data);

static const char *dm_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Diamond");
}

static void *dm_create(obs_data_t *settings, obs_source_t *source)
{
	struct dm_source *src = bzalloc(sizeof(struct dm_source));

	src->cm.flags = CM_FLAG_CONVERT_RGB;
	cm_create(&src->cm, settings, source);
	cm_request(&src->cm, dm_surface_cb, src);

	obs_enter_graphics();
	src->effect = create_effect_from_module_file("vectorscope.effect");
	obs_leave_graphics();

	dm_update(src, settings);
	return src;
}

static void dm_destroy(void *data)
{
	struct dm_source *src = data;

	obs_enter_graphics();
	gs_texture_destroy(src->tex);
	obs_leave_graphics();

	cm_destroy(&src->cm);

	bfree(src->tex_buf[0]);
	bfree(src->tex_buf[1]);
	bfree(src);
}

static void dm_update(void *data, obs_data_t *settings)
{
	struct dm_source *src = data;
	cm_update(&src->cm, settings);

	src->intensity = (int)obs_data_get_int(settings, "intensity");
	if (src->intensity < 1)
		src->intensity = 1;
	src->trace_color = (int)obs_data_get_int(settings, "trace_color");
	src->graticule = obs_data_get_bool(settings, "show_graticule");
	src->labels = obs_data_get_bool(settings, "labels");
}

static void dm_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "target_scale", 2);
	obs_data_set_default_int(settings, "intensity", 25);
	obs_data_set_default_int(settings, "trace_color", DM_COLOR_GREEN);
	obs_data_set_default_bool(settings, "show_graticule", true);
	obs_data_set_default_bool(settings, "labels", true);
}

static obs_properties_t *dm_get_properties(void *data)
{
	struct dm_source *src = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *prop;

	cm_get_properties(&src->cm, props);

	obs_properties_add_int(props, "intensity", obs_module_text("Intensity"), 1, 255, 1);

	prop = obs_properties_add_list(props, "trace_color", obs_module_text("Diamond.Prop.TraceColor"),
				       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(prop, obs_module_text("Green"), DM_COLOR_GREEN);
	obs_property_list_add_int(prop, obs_module_text("VS.Prop.ColorType.White"), DM_COLOR_WHITE);

	obs_properties_add_bool(props, "show_graticule", obs_module_text("Graticule"));
	obs_properties_add_bool(props, "labels", obs_module_text("Diamond.Prop.Labels"));

	return props;
}

static uint32_t dm_get_width(void *data)
{
	struct dm_source *src = data;
	return src->cm.bypass ? cm_bypass_get_width(&src->cm) : DM_SIZE;
}

static uint32_t dm_get_height(void *data)
{
	struct dm_source *src = data;
	return src->cm.bypass ? cm_bypass_get_height(&src->cm) : DM_SIZE;
}

static inline void inc_uint8(uint8_t *c)
{
	if (*c < 255)
		++*c;
}

static void dm_draw(uint8_t *dbuf, const struct cm_surface_data *surface_data)
{
	memset(dbuf, 0, DM_SIZE * DM_SIZE);

	const uint8_t *rgb = surface_data->rgb_data;
	for (uint32_t y = 0; y < surface_data->height; y++) {
		const uint8_t *p = rgb + (size_t)surface_data->linesize * y;
		for (uint32_t x = 0; x < surface_data->width; x++, p += 4) {
			if (!p[3])
				continue; // outside the selection
			const int b = p[0], g = p[1], r = p[2];

			// upper diamond: rows 0..127, black at the bottom of it
			int ux = 128 + (b - g) / 2;
			int uy = 127 - (b + g) / 4;
			// lower diamond: rows 128..255, black at the top of it
			int lx = 128 + (r - g) / 2;
			int ly = 128 + (r + g) / 4;

			if (ux > 255)
				ux = 255;
			if (lx > 255)
				lx = 255;
			if (uy < 0)
				uy = 0;
			if (ly > 255)
				ly = 255;

			inc_uint8(dbuf + uy * DM_SIZE + ux);
			inc_uint8(dbuf + ly * DM_SIZE + lx);
		}
	}
}

static void dm_surface_cb(void *data, struct cm_surface_data *surface_data)
{
	struct dm_source *src = data;
	if (!surface_data->rgb_data || !surface_data->width)
		return;

	if (!src->tex_buf[src->w_tex_buf])
		src->tex_buf[src->w_tex_buf] = bzalloc(DM_SIZE * DM_SIZE);

	dm_draw(src->tex_buf[src->w_tex_buf], surface_data);
	src->w_tex_buf ^= 1;
}

static void dashed_line(float x0, float y0, float x1, float y1, int n_dash)
{
	for (int i = 0; i < n_dash; i++) {
		const float t0 = (float)(2 * i) / (2 * n_dash - 1);
		const float t1 = (float)(2 * i + 1) / (2 * n_dash - 1);
		gs_vertex2f(x0 + (x1 - x0) * t0, y0 + (y1 - y0) * t0);
		gs_vertex2f(x0 + (x1 - x0) * t1, y0 + (y1 - y0) * t1);
	}
}

static void dm_render_graticule(const struct dm_source *src)
{
	const uint32_t color = 0xA0C8C8C8;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), color);

	// outlines: black (center), green (left), white (tip), blue / red (right)
	const float top[4][2] = {{128.0f, 127.5f}, {0.0f, 63.75f}, {128.0f, 0.0f}, {256.0f, 63.75f}};
	const float bot[4][2] = {{128.0f, 128.5f}, {0.0f, 192.25f}, {128.0f, 256.0f}, {256.0f, 192.25f}};

	while (gs_effect_loop(solid, "Solid")) {
		gs_render_start(false);
		for (int i = 0; i < 4; i++) {
			const int j = (i + 1) % 4;
			dashed_line(top[i][0], top[i][1], top[j][0], top[j][1], 14);
			dashed_line(bot[i][0], bot[i][1], bot[j][0], bot[j][1], 14);
		}
		gs_render_stop(GS_LINES);
	}

	if (!src->labels)
		return;

	const struct text_scale ts = text_scale_from_viewport((float)DM_SIZE, (float)DM_SIZE);
	const float px = 13.0f;
	const float m = 3.0f * ts.ux;
	text_draw(&ts, "Wh", 128.0f, 0.0f, px, color, TEXT_ALIGN_CENTER, TEXT_VALIGN_TOP);
	text_draw(&ts, "Wh", 128.0f, 256.0f, px, color, TEXT_ALIGN_CENTER, TEXT_VALIGN_BOTTOM);
	text_draw(&ts, "G", m, 63.75f, px, color, TEXT_ALIGN_LEFT, TEXT_VALIGN_BOTTOM);
	text_draw(&ts, "G", m, 192.25f, px, color, TEXT_ALIGN_LEFT, TEXT_VALIGN_TOP);
	text_draw(&ts, "B", 256.0f - m, 63.75f, px, color, TEXT_ALIGN_RIGHT, TEXT_VALIGN_BOTTOM);
	text_draw(&ts, "R", 256.0f - m, 192.25f, px, color, TEXT_ALIGN_RIGHT, TEXT_VALIGN_TOP);
}

static void dm_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct dm_source *src = data;
	if (src->cm.bypass) {
		cm_bypass_render(&src->cm);
		return;
	}

	cm_render_target(&src->cm);

	const int r_tex_buf = src->w_tex_buf ^ 1;
	if (src->tex_buf[r_tex_buf] && src->effect) {
		const uint8_t *buf = src->tex_buf[r_tex_buf];
		if (!src->tex)
			src->tex = gs_texture_create(DM_SIZE, DM_SIZE, GS_R8, 1, &buf, GS_DYNAMIC);
		else
			gs_texture_set_image(src->tex, buf, DM_SIZE, false);

		gs_effect_t *e = src->effect;
		struct vec4 color;
		if (src->trace_color == DM_COLOR_WHITE)
			vec4_set(&color, 1.0f, 1.0f, 1.0f, 1.0f);
		else
			vec4_set(&color, 0.35f, 1.0f, 0.35f, 1.0f);
		struct vec3 zero;
		vec3_zero(&zero);
		gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), src->tex);
		gs_effect_set_float(gs_effect_get_param_by_name(e, "intensity"), (float)src->intensity);
		gs_effect_set_vec4(gs_effect_get_param_by_name(e, "color"), &color);
		gs_effect_set_vec3(gs_effect_get_param_by_name(e, "color_u"), &zero);
		gs_effect_set_vec3(gs_effect_get_param_by_name(e, "color_v"), &zero);
		while (gs_effect_loop(e, "Draw"))
			gs_draw_sprite(src->tex, 0, DM_SIZE, DM_SIZE);
	}

	if (src->graticule)
		dm_render_graticule(src);
}

const struct obs_source_info colormonitor_diamond = {
	.id = ID_PREFIX "diamond_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = dm_get_name,
	.create = dm_create,
	.destroy = dm_destroy,
	.update = dm_update,
	.get_defaults = dm_get_defaults,
	.get_properties = dm_get_properties,
	.get_width = dm_get_width,
	.get_height = dm_get_height,
	.enum_active_sources = cm_enum_sources,
	.video_render = dm_render,
	.video_tick = cm_tick,
};
